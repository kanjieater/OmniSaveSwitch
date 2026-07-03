#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "activity.h"
#include "omnisave.h"

#define ACTIVITY_BATCH_MAX        200
#define ACTIVITY_EVENT_MAX_CHARS  210
#define ACTIVITY_JSON_BUF         (ACTIVITY_BATCH_MAX * ACTIVITY_EVENT_MAX_CHARS + 64)
#define ACTIVITY_OFFSET_PATH      OMNI_ROOT "/state/activity_offset.json"

static u32  s_last_offset       = 0;
static bool s_activity_flushing = false;

// Reconstruct a u64 from two u32s stored with high/low words swapped (pdm.h convention).
static u64 pdm_u32pair_to_u64(u32 hi, u32 lo) {
    return ((u64)hi << 32) | (u64)lo;
}

// Returns true if the offset was durably persisted, false on I/O failure.
// Callers must not advance s_last_offset unless this returns true.
static bool save_offset(FsFileSystem* sd, u32 offset) {
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
    if (R_FAILED(fsFsOpenFile(sd, tmp, FsOpenMode_Read, &verify))) return false;
    fsFileClose(&verify);

    fsFsRenameFile(sd, ACTIVITY_OFFSET_PATH, bak);
    fsFsRenameFile(sd, tmp, ACTIVITY_OFFSET_PATH);
    fsFsDeleteFile(sd, bak);
    Result rc = fsFsCommit(sd);
    return R_SUCCEEDED(rc);
}

void activity_init(void) {
    s_last_offset = 0;
}

int activity_flush(FsFileSystem* sd) {
    if (s_activity_flushing) return 0;
    s_activity_flushing = true;

    // Re-query server watermark on every flush so a mid-session DB wipe causes
    // the Switch to re-drain from the server's new offset automatically.
    // Any non-200 (401 not-yet-paired, 500, network failure) defers this flush.
    char resp[128] = {0};
    if (http_get_body("/api/v1/activity/offset", resp, sizeof(resp)) == 200) {
        const char* p = strstr(resp, "\"last_offset\":");
        if (!p) { s_activity_flushing = false; return 0; }
        s_last_offset = (u32)strtoul(p + 14, NULL, 10);
    } else {
        s_activity_flushing = false;
        return 0;
    }

    // Allocate once; reused across all batches in this flush cycle.
    static PdmPlayEvent events[ACTIVITY_BATCH_MAX];
    static char json[ACTIVITY_JSON_BUF];

    int last_status = 0;

    // Cap at 100 batches (20 000 events) as a safety bound against a pathological
    // firmware state where PDM repeatedly reports the same non-empty range.
    for (int batch = 0; batch < 100; batch++) {
        // Check available range and handle log rotation.
        s32 total_entries, start_idx, end_idx;
        if (R_SUCCEEDED(pdmqryGetAvailablePlayEventRange(&total_entries, &start_idx, &end_idx))) {
            if ((s32)s_last_offset < start_idx) s_last_offset = (u32)start_idx;
            if ((s32)s_last_offset >= end_idx) break;  // caught up — done
        } else {
            break;
        }

        s32 total_read = 0;
        Result rc = pdmqryQueryPlayEvent((s32)s_last_offset, events, ACTIVITY_BATCH_MAX, &total_read);
        if (R_FAILED(rc) || total_read == 0) break;

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

            // Pre-flight: ensure space for this event + closing field.
            // Closing is ],\"next_offset\":NNNNNNNNNN} — 27 chars max + null = 28 bytes.
            // Stop here if it won't fit — consumed stays at i so these events retry next flush.
            if ((int)sizeof(json) - pos < ACTIVITY_EVENT_MAX_CHARS + 28) break;

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

        // Append next_offset so the server can durably track how far it has received.
        u32 next_offset = s_last_offset + (u32)consumed;
        snprintf(json + pos, sizeof(json) - pos, "],\"next_offset\":%u}", (unsigned)next_offset);

        // All events in this batch were filtered — advance past them and keep draining.
        if (written == 0) {
            u32 new_offset = s_last_offset + (u32)total_read;
            if (!save_offset(sd, new_offset)) break;
            s_last_offset = new_offset;
            continue;
        }

        char resp[256] = {0};
        int status = http_post_json("/api/v1/activity/events", json, resp, sizeof(resp));
        last_status = status;
        if (status >= 200 && status < 300) {
            // Only advance offset after confirmed persist. If save fails, the same
            // batch is retried next trigger — the server must handle duplicate ingestion.
            if (!save_offset(sd, next_offset)) break;
            s_last_offset = next_offset;
            // Continue to next batch.
        } else {
            break;  // POST failed — retry next trigger from current offset.
        }
    }

    s_activity_flushing = false;
    return last_status;
}
