#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "omnisave.h"

// ── Atomic write helper (tmp + bak + rename) ──────────────────────────────────
//
// Crash-safe sequence that avoids the delete-then-rename data-loss window:
//   1. Write new content to .tmp  (durable after commit)
//   2. Rename original → .bak    (safe even if no original exists)
//   3. Rename .tmp → target       (makes new content live)
//   4. Delete .bak                (stale; harmless if this crashes)
//
// Crash recovery in recovery_sweep handles any orphaned .bak or .tmp.

static void atomic_write(FsFileSystem* sd, const char* path, const char* text) {
    char tmp[FS_MAX_PATH + 8];
    char bak[FS_MAX_PATH + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    snprintf(bak, sizeof(bak), "%s.bak", path);

    fs_write_text_file(sd, tmp, text);
    fsFsCommit(sd);  // ensure .tmp is durable before touching the original

    fsFsRenameFile(sd, path, bak);  // original → .bak (ignore error: no original on first write)
    fsFsRenameFile(sd, tmp, path);  // .tmp → target (makes new content live)
    fsFsDeleteFile(sd, bak);        // .bak is now stale
    fsFsCommit(sd);
}

// ── In-place write helper for status.json ────────────────────────────────────
// Does NOT delete the file before writing — eliminates the gap where the overlay
// reads a missing file and flashes "Sysmodule offline".
// Only suitable for display-only files where stale content is acceptable.

// Replace `"` and `\` with safe ASCII so they can't break the strstr-based JSON parsers.
static void sanitize_for_json(const char* src, char* dst, size_t sz) {
    size_t i = 0;
    for (; *src && i + 1 < sz; src++) {
        if      (*src == '"')  dst[i++] = '\'';
        else if (*src == '\\') dst[i++] = '/';
        else if (*src == '{')  dst[i++] = '(';
        else if (*src == '}')  dst[i++] = ')';
        else dst[i++] = *src;
    }
    dst[i] = '\0';
}

static void write_inplace(FsFileSystem* sd, const char* path, const char* text) {
    const s64 len = (s64)strlen(text);
    FsFile f;
    // Open with Read|Write so fsFileGetSize works reliably on all Horizon FW versions.
    if (R_FAILED(fsFsOpenFile(sd, path, FsOpenMode_Write | FsOpenMode_Read, &f))) {
        fsFsCreateFile(sd, path, len, 0);
        if (R_FAILED(fsFsOpenFile(sd, path, FsOpenMode_Write | FsOpenMode_Read, &f))) return;
        fsFileWrite(&f, 0, text, (u64)len, FsWriteOption_Flush);
        fsFileClose(&f);
        fsFsCommit(sd);
        return;
    }
    s64 cur = 0;
    fsFileGetSize(&f, &cur);
    if (len > cur) {
        // Growing: extend first so write doesn't under-run; overlay sees old valid content.
        fsFileSetSize(&f, len);
    }
    fsFileWrite(&f, 0, text, (u64)len, FsWriteOption_Flush);
    if (len < cur) {
        // Shrinking: truncate after write so overlay never reads a partial/empty file.
        fsFileSetSize(&f, len);
    }
    fsFileClose(&f);
    fsFsCommit(sd);
}

// ── Monotonic tick counter (advances on every status write) ───────────────────

static int      s_tick            = 0;
static uint32_t s_sync_generation = 0;

// ── device.json write helper (includes sync_generation) ───────────────────────

static void write_device_json(FsFileSystem* sd) {
    char json[512];
    snprintf(json, sizeof(json),
        "{\n"
        "  \"device_id\": \"%s\",\n"
        "  \"hw_type\": \"%s\",\n"
        "  \"fw_version\": \"%s\",\n"
        "  \"client_version\": \"" OMNISAVE_VERSION "\",\n"
        "  \"sync_generation\": %u\n"
        "}\n",
        s_device_id, s_hw_type, s_fw_version, s_sync_generation);
    atomic_write(sd, OMNI_ROOT "/state/device.json", json);
}

// ── state_init ─────────────────────────────────────────────────────────────────

static void load_events_from_disk(FsFileSystem* sd);  // defined after _bk_str

void state_init(FsFileSystem* sd) {
    ensure_dir(sd, OMNI_ROOT "/state");
    ensure_dir(sd, OMNI_ROOT "/state/lineage");
    ensure_dir(sd, OMNI_ROOT "/signals");

    // Preserve sync_generation across reboots before rewriting device.json.
    {
        char existing[512] = {0};
        if (fs_read_text_file(sd, OMNI_ROOT "/state/device.json", existing, sizeof(existing))) {
            const char* p = strstr(existing, "\"sync_generation\":");
            if (p) {
                p += 18;
                while (*p == ' ') p++;
                unsigned long gen = 0;
                sscanf(p, "%lu", &gen);
                s_sync_generation = (uint32_t)gen;
            }
        }
    }

    write_device_json(sd);
    state_write_status(sd, IDLE, NULL, 0, 0);
    load_events_from_disk(sd);
}

// ── state_write_status ─────────────────────────────────────────────────────────

void state_write_status(FsFileSystem* sd, FsmState fsm, const char* key,
                        s64 verified_bytes, s64 total_bytes) {
    s_tick++;

    // ── current_activity ────────────────────────────────────────────────────
    char activity[384] = "null";
    if (key && key[0]) {
        char tid[17] = {0}, uid[17] = {0};
        sscanf(key, "%*[^-]-%16[^-]-%16s", tid, uid);
        const char* dir = (fsm == UPLOADING) ? "outbound" : "inbound";
        snprintf(activity, sizeof(activity),
            "{\"title_id\":\"%s\",\"switch_user_id\":\"%s\","
            "\"target_snapshot_id\":\"%s\",\"direction\":\"%s\"}",
            tid, uid, key, dir);
    }

    // ── ui block ─────────────────────────────────────────────────────────────
    int pct = -1;
    int show = 0;
    if (total_bytes > 0) {
        pct = (int)((verified_bytes * 100) / total_bytes);
        show = 1;
    }
    const char* render_mode;
    switch (fsm) {
        case UPLOADING:
        case DOWNLOADING:   render_mode = "NETWORK"; break;
        case INBOUND_READY:
        case DELIVERING:    render_mode = "LOCAL";   break;
        case RETRY_BACKOFF: render_mode = "RETRY";   break;
        default:            render_mode = "IDLE";    break;
    }

    char vb_str[24], tb_str[24];
    if (total_bytes > 0) {
        snprintf(vb_str, sizeof(vb_str), "%lld", (long long)verified_bytes);
        snprintf(tb_str, sizeof(tb_str), "%lld", (long long)total_bytes);
    } else {
        snprintf(vb_str, sizeof(vb_str), "null");
        snprintf(tb_str, sizeof(tb_str), "null");
    }

    char json[768];
    snprintf(json, sizeof(json),
        "{\"tick\":%d,\"fsm_state\":\"%s\","
        "\"current_activity\":%s,"
        "\"transport\":{\"verified_bytes\":%s,\"total_bytes\":%s},"
        "\"ui\":{\"progress_pct\":%d,\"show_progress\":%s,"
        "\"render_mode\":\"%s\"}}\n",
        s_tick, fsm_state_name(fsm),
        activity,
        vb_str, tb_str,
        pct, show ? "true" : "false", render_mode);

    write_inplace(sd, OMNI_ROOT "/state/status.json", json);
}

// ── Shared ISO-8601 UTC timestamp ──────────────────────────────────────────────

static void iso_utc_now(char* out, size_t sz) {
    u64 posix = get_posix_utc();
    TimeCalendarTime cal;
    if (posix > 0 && R_SUCCEEDED(timeToCalendarTimeWithMyRule(posix, &cal, NULL)))
        snprintf(out, sz, "%04d-%02d-%02dT%02d:%02d:%02dZ",
                 (int)cal.year, (int)cal.month, (int)cal.day,
                 (int)cal.hour, (int)cal.minute, (int)cal.second);
    else
        snprintf(out, sz, "1970-01-01T00:00:00Z");
}

// ── Session event log (ring buffer, newest-first, max 20 entries) ─────────────

#define MAX_EVENTS   10
#define EVENTS_PATH  OMNI_ROOT "/state/events.json"

typedef struct { char ts[24]; char level[9]; char msg[64]; } EvEntry;

static EvEntry s_events[MAX_EVENTS];
static int     s_event_count = 0;
static char    s_ev_json[4096];

static void flush_events(FsFileSystem* sd) {
    int pos = 0;
    pos += snprintf(s_ev_json + pos, sizeof(s_ev_json) - pos, "[\n");
    for (int i = 0; i < s_event_count; i++) {
        char safe_msg[64];
        sanitize_for_json(s_events[i].msg, safe_msg, sizeof(safe_msg));
        int written = snprintf(s_ev_json + pos, sizeof(s_ev_json) - pos,
            "{\"ts\":\"%s\",\"level\":\"%s\",\"msg\":\"%s\"}%s\n",
            s_events[i].ts, s_events[i].level, safe_msg,
            (i < s_event_count - 1) ? "," : "");
        if (written <= 0) break;
        pos += written;
        if (pos >= (int)sizeof(s_ev_json) - 4) break;
    }
    snprintf(s_ev_json + pos, sizeof(s_ev_json) - pos, "]\n");
    atomic_write(sd, EVENTS_PATH, s_ev_json);
}

void state_append_event(FsFileSystem* sd, const char* level, const char* msg) {
    if (s_event_count > 0 && strcmp(s_events[0].msg, msg) == 0) return;
    if (s_event_count < MAX_EVENTS) s_event_count++;
    memmove(&s_events[1], &s_events[0], sizeof(EvEntry) * (size_t)(s_event_count - 1));
    iso_utc_now(s_events[0].ts, sizeof(s_events[0].ts));
    snprintf(s_events[0].level, sizeof(s_events[0].level), "%s", level);
    snprintf(s_events[0].msg,   sizeof(s_events[0].msg),   "%s", msg);
    flush_events(sd);
}

// ── state_get/set_sync_generation ─────────────────────────────────────────────

uint32_t state_get_sync_generation(void) {
    return s_sync_generation;
}

void state_set_sync_generation(FsFileSystem* sd, uint32_t gen) {
    s_sync_generation = gen;
    write_device_json(sd);
}

// ── state_apply_backup_updates ─────────────────────────────────────────────────
// Parses a JSON array of {title_id, snapshot_sequence, committed_at} objects and
// writes per-title last_backup_{title_id}.json files plus global last_backup.json.

static bool _bk_str(const char* json, const char* key, char* out, size_t sz) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char* p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ') p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < sz) out[i++] = *p++;
    out[i] = '\0';
    return i > 0;
}

static int _bk_int(const char* json, const char* key, int def) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char* p = strstr(json, needle);
    if (!p) return def;
    p += strlen(needle);
    while (*p == ' ') p++;
    int v = def;
    sscanf(p, "%d", &v);
    return v;
}

// Load persisted events from disk into in-memory ring buffer on boot.
static void load_events_from_disk(FsFileSystem* sd) {
    char buf[4096] = {};
    if (!fs_read_text_file(sd, EVENTS_PATH, buf, sizeof(buf))) return;
    const char* p = strchr(buf, '[');
    if (!p) return;
    int n = 0;
    while (n < MAX_EVENTS) {
        const char* obj_start = strchr(p, '{');
        if (!obj_start) break;
        const char* obj_end = strchr(obj_start, '}');
        if (!obj_end) break;
        size_t obj_len = (size_t)(obj_end - obj_start + 1);
        if (obj_len > 0 && obj_len < 256) {
            char obj[256] = {};
            memcpy(obj, obj_start, obj_len);
            _bk_str(obj, "ts",    s_events[n].ts,    sizeof(s_events[n].ts));
            _bk_str(obj, "level", s_events[n].level, sizeof(s_events[n].level));
            _bk_str(obj, "msg",   s_events[n].msg,   sizeof(s_events[n].msg));
            if (s_events[n].msg[0]) n++;
        }
        p = obj_end + 1;
    }
    s_event_count = n;
}

void state_apply_backup_updates(FsFileSystem* sd, const char* arr) {
    char best_tid[17]  = {0};
    char best_at[32]   = {0};
    int  best_seq      = 0;

    const char* p = arr;
    while (1) {
        const char* obj_start = strchr(p, '{');
        if (!obj_start) break;
        const char* obj_end = strchr(obj_start, '}');
        if (!obj_end) break;

        size_t obj_len = (size_t)(obj_end - obj_start + 1);
        if (obj_len >= 256) { p = obj_end + 1; continue; }
        char entry[256];
        memcpy(entry, obj_start, obj_len);
        entry[obj_len] = '\0';

        char tid[17] = {0};
        char at[32]  = {0};
        if (!_bk_str(entry, "title_id", tid, sizeof(tid))) { p = obj_end + 1; continue; }
        int seq = _bk_int(entry, "snapshot_sequence", 0);
        if (seq <= 0) { p = obj_end + 1; continue; }
        _bk_str(entry, "committed_at", at, sizeof(at));
        if (!at[0]) snprintf(at, sizeof(at), "1970-01-01T00:00:00Z");

        char safe_user[33];
        sanitize_for_json(s_account_nickname, safe_user, sizeof(safe_user));
        char json[320];
        snprintf(json, sizeof(json),
                 "{\"title_id\":\"%s\",\"snapshot_id\":\"\",\"completed_at\":\"%s\","
                 "\"snapshot_counter\":%d,\"username\":\"%s\"}\n",
                 tid, at, seq, safe_user);

        char per_game[FS_MAX_PATH];
        snprintf(per_game, sizeof(per_game), OMNI_ROOT "/state/last_backup_%s.json", tid);
        atomic_write(sd, per_game, json);

        // Track the most-recent entry (ISO-8601 UTC strings compare lexicographically)
        if (strcmp(at, best_at) > 0) {
            snprintf(best_tid, sizeof(best_tid), "%s", tid);
            snprintf(best_at,  sizeof(best_at),  "%s", at);
            best_seq = seq;
        }

        p = obj_end + 1;
    }

    if (best_seq > 0) {
        char safe_user[33];
        sanitize_for_json(s_account_nickname, safe_user, sizeof(safe_user));
        char json[320];
        snprintf(json, sizeof(json),
                 "{\"title_id\":\"%s\",\"snapshot_id\":\"\",\"completed_at\":\"%s\","
                 "\"snapshot_counter\":%d,\"username\":\"%s\"}\n",
                 best_tid, best_at, best_seq, safe_user);
        atomic_write(sd, OMNI_ROOT "/state/last_backup.json", json);
    }
}

// ── state_write_last_backup ────────────────────────────────────────────────────

void state_write_last_backup(FsFileSystem* sd, const char* title_id,
                              const char* snap_key, int counter) {
    char ts[32];
    iso_utc_now(ts, sizeof(ts));
    char safe_user[33];
    sanitize_for_json(s_account_nickname, safe_user, sizeof(safe_user));
    char json[320];
    snprintf(json, sizeof(json),
             "{\"title_id\":\"%s\",\"snapshot_id\":\"%s\",\"completed_at\":\"%s\","
             "\"snapshot_counter\":%d,\"username\":\"%s\"}\n",
             title_id, snap_key, ts, counter, safe_user);
    atomic_write(sd, OMNI_ROOT "/state/last_backup.json", json);

    char per_game[FS_MAX_PATH];
    snprintf(per_game, sizeof(per_game),
             OMNI_ROOT "/state/last_backup_%s.json", title_id);
    atomic_write(sd, per_game, json);
}

// ── state_write_last_restore ───────────────────────────────────────────────────

void state_write_last_restore(FsFileSystem* sd, const char* title_id,
                               const char* snap_key, int counter) {
    char ts[32];
    iso_utc_now(ts, sizeof(ts));
    char safe_user[33];
    sanitize_for_json(s_account_nickname, safe_user, sizeof(safe_user));
    char json[320];
    snprintf(json, sizeof(json),
             "{\"title_id\":\"%s\",\"snapshot_id\":\"%s\",\"completed_at\":\"%s\","
             "\"snapshot_counter\":%d,\"username\":\"%s\"}\n",
             title_id, snap_key, ts, counter, safe_user);
    atomic_write(sd, OMNI_ROOT "/state/last_restore.json", json);

    char per_game[FS_MAX_PATH];
    snprintf(per_game, sizeof(per_game),
             OMNI_ROOT "/state/last_restore_%s.json", title_id);
    atomic_write(sd, per_game, json);
}

// ── state_write_lineage ────────────────────────────────────────────────────────

void state_write_lineage(FsFileSystem* sd, const char* title_id, const char* uid,
                          const char* snap_id, int counter) {
    ensure_dir(sd, OMNI_ROOT "/state/lineage");
    char path[FS_MAX_PATH];
    snprintf(path, sizeof(path),
             OMNI_ROOT "/state/lineage/%s_%s.json", title_id, uid);

    u64 posix = get_posix_utc();
    char ts[32];
    format_local_time(ts, sizeof(ts), posix);

    char json[512];
    snprintf(json, sizeof(json),
        "{\n"
        "  \"base_snapshot_id\": \"%s\",\n"
        "  \"snapshot_counter\": %d,\n"
        "  \"last_synced_at\": \"%s\"\n"
        "}\n",
        snap_id, counter, ts);
    atomic_write(sd, path, json);
}

// ── state_read_lineage ─────────────────────────────────────────────────────────

bool state_read_lineage(FsFileSystem* sd, const char* title_id, const char* uid,
                         char* base_snap_out, size_t snap_sz, int* counter_out) {
    if (counter_out) *counter_out = 0;
    if (base_snap_out && snap_sz > 0) base_snap_out[0] = '\0';

    char path[FS_MAX_PATH];
    snprintf(path, sizeof(path),
             OMNI_ROOT "/state/lineage/%s_%s.json", title_id, uid);

    char buf[512];
    if (!fs_read_text_file(sd, path, buf, sizeof(buf))) return false;

    if (base_snap_out) {
        const char* p = strstr(buf, "\"base_snapshot_id\":");
        if (p) {
            p += 19;
            while (*p == ' ' || *p == '"') p++;
            size_t i = 0;
            while (*p && *p != '"' && i < snap_sz - 1)
                base_snap_out[i++] = *p++;
            base_snap_out[i] = '\0';
        }
    }

    if (counter_out) {
        const char* p = strstr(buf, "\"snapshot_counter\":");
        if (p) {
            p += 19;
            while (*p == ' ') p++;
            *counter_out = (int)strtol(p, NULL, 10);
        }
    }

    return true;
}

void state_atomic_write(FsFileSystem* sd, const char* path, const char* text) {
    atomic_write(sd, path, text);
}
