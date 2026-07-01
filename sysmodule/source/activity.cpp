#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "activity.h"
#include "omnisave.h"

#define ACTIVITY_BATCH_MAX        200
#define ACTIVITY_EVENT_MAX_CHARS  210
#define ACTIVITY_JSON_BUF         (ACTIVITY_BATCH_MAX * ACTIVITY_EVENT_MAX_CHARS + 32)
#define ACTIVITY_OFFSET_PATH      OMNI_ROOT "/state/activity_offset.json"

static u32  s_last_offset       = 0;
static bool s_activity_flushing = false;

// Reconstruct a u64 from two u32s stored with high/low words swapped (pdm.h convention).
static u64 pdm_u32pair_to_u64(u32 hi, u32 lo) {
    return ((u64)hi << 32) | (u64)lo;
}

static void save_offset(FsFileSystem* sd, u32 offset) {
    char json[64];
    snprintf(json, sizeof(json), "{\"last_offset\":%u}\n", (unsigned)offset);

    char tmp[FS_MAX_PATH + 8];
    char bak[FS_MAX_PATH + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", ACTIVITY_OFFSET_PATH);
    snprintf(bak, sizeof(bak), "%s.bak", ACTIVITY_OFFSET_PATH);

    fs_write_text_file(sd, tmp, json);

    // Verify .tmp landed before touching the live file.
    // fs_write_text_file returns void; if SD was full or I/O failed, .tmp won't
    // exist, and proceeding would destroy the only good copy via the rename below.
    FsFile verify;
    if (R_FAILED(fsFsOpenFile(sd, tmp, FsOpenMode_Read, &verify))) return;
    fsFileClose(&verify);

    fsFsRenameFile(sd, ACTIVITY_OFFSET_PATH, bak);
    fsFsRenameFile(sd, tmp, ACTIVITY_OFFSET_PATH);
    fsFsDeleteFile(sd, bak);
    fsFsCommit(sd);
}

void activity_init(FsFileSystem* sd) {
    s_last_offset = 0;

    char buf[128] = {0};
    if (fs_read_text_file(sd, ACTIVITY_OFFSET_PATH, buf, sizeof(buf))) {
        const char* p = strstr(buf, "\"last_offset\":");
        if (p) {
            p += 14;
            s_last_offset = (u32)strtoul(p, NULL, 10);
        }
    }
}

int activity_flush(FsFileSystem* sd) {
    if (s_activity_flushing) return 0;
    s_activity_flushing = true;

    // Check available range and handle log rotation.
    s32 total_entries, start_idx, end_idx;
    if (R_SUCCEEDED(pdmqryGetAvailablePlayEventRange(&total_entries, &start_idx, &end_idx))) {
        if ((s32)s_last_offset < start_idx) s_last_offset = (u32)start_idx;
        if ((s32)s_last_offset >= end_idx) {
            s_activity_flushing = false;
            return 0;
        }
    }

    static PdmPlayEvent events[ACTIVITY_BATCH_MAX];
    s32 total_read = 0;
    Result rc = pdmqryQueryPlayEvent((s32)s_last_offset, events, ACTIVITY_BATCH_MAX, &total_read);
    if (R_FAILED(rc) || total_read == 0) {
        s_activity_flushing = false;
        return 0;
    }

    static char json[ACTIVITY_JSON_BUF];
    int pos = snprintf(json, sizeof(json), "{\"events\":[");
    int written  = 0;
    s32 consumed = 0;

    for (s32 i = 0; i < total_read; i++) {
        PdmPlayEvent* e = &events[i];
        const char* etype = NULL;
        char app_id[17]  = {0};
        char prof_id[33] = {0};

        if (e->play_event_type == PdmPlayEventType_Applet) {
            if (e->event_data.applet.log_policy != PdmPlayLogPolicy_All) {
                consumed++;
                continue;
            }
            switch (e->event_data.applet.event_type) {
                case 0:                 etype = "APPLICATION_STARTED";   break;
                case 1: case 5: case 6: etype = "APPLICATION_EXITED";    break;
                case 2:                 etype = "APPLICATION_FOCUSED";   break;
                case 3: case 4:         etype = "APPLICATION_UNFOCUSED"; break;
                default:                consumed++; continue;
            }
            u64 tid = pdm_u32pair_to_u64(e->event_data.applet.program_id[0],
                                         e->event_data.applet.program_id[1]);
            snprintf(app_id, sizeof(app_id), "%016llX", (unsigned long long)tid);
        } else if (e->play_event_type == PdmPlayEventType_Account) {
            if (e->event_data.account.type == 2) { consumed++; continue; }
            etype = (e->event_data.account.type == 0) ? "PROFILE_ACTIVE" : "PROFILE_INACTIVE";
            u64 u0 = pdm_u32pair_to_u64(e->event_data.account.uid[0],
                                        e->event_data.account.uid[1]);
            u64 u1 = pdm_u32pair_to_u64(e->event_data.account.uid[2],
                                        e->event_data.account.uid[3]);
            snprintf(prof_id, sizeof(prof_id), "%016llX%016llX",
                     (unsigned long long)u0, (unsigned long long)u1);
        } else {
            consumed++;
            continue;  // PowerStateChange, OperationModeChange, Initialize
        }

        // Pre-flight: ensure space for this event + closing ]}
        // Stop here if it won't fit — consumed stays at i so these events retry next flush.
        if ((int)sizeof(json) - pos < ACTIVITY_EVENT_MAX_CHARS + 2) break;

        int n = snprintf(json + pos, sizeof(json) - pos,
            "%s{\"event_type\":\"%s\"%s%s%s%s%s%s"
            ",\"event_timestamp\":%llu,\"monotonic_timestamp\":%llu}",
            written ? "," : "",
            etype,
            app_id[0]  ? ",\"application_id\":\"" : "", app_id,  app_id[0]  ? "\"" : "",
            prof_id[0] ? ",\"profile_id\":\""      : "", prof_id, prof_id[0] ? "\"" : "",
            (unsigned long long)e->timestamp_user,
            (unsigned long long)e->timestamp_steady);
        if (n <= 0 || n >= (int)(sizeof(json) - pos)) break;
        pos += n;
        written++;
        consumed++;
    }

    snprintf(json + pos, sizeof(json) - pos, "]}");

    // All events were filtered (e.g. all PowerStateChange) — advance past them.
    if (written == 0) {
        s_last_offset += (u32)(consumed > 0 ? consumed : total_read);
        save_offset(sd, s_last_offset);
        s_activity_flushing = false;
        return 0;
    }

    char resp[256] = {0};
    int status = http_post_json("/api/v1/activity/events", json, resp, sizeof(resp));
    if (status >= 200 && status < 300) {
        // Advance by consumed, not total_read: if JSON buffer filled mid-batch,
        // the remaining records were not serialized and must retry next flush.
        s_last_offset += (u32)consumed;
        save_offset(sd, s_last_offset);
    }
    s_activity_flushing = false;
    return status;
}
