#define XXH_INLINE_ALL
#include "xxhash.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "omnisave.h"

#pragma GCC diagnostic ignored "-Wformat-truncation"

// max chars for "[u32,u32,...,u32]" with MAX_CHECKPOINTS entries
#define LEDGER_JSON_MAX  (MAX_CHECKPOINTS * 12 + 4)
// wrapper: {"checkpoint_size":4194304,"checkpoint_ledger":<ledger>}
#define MANIFEST_MAX     (LEDGER_JSON_MAX + 64)

// ── JSON helpers ──────────────────────────────────────────────────────────────

static bool json_str(const char* json, const char* key,
                     char* out, size_t out_sz) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char* p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ') p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_sz)
        out[i++] = *p++;
    out[i] = '\0';
    return i > 0;
}

static long long json_int(const char* json, const char* key, long long def) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char* p = strstr(json, needle);
    if (!p) return def;
    p += strlen(needle);
    while (*p == ' ') p++;
    long long v = def;
    sscanf(p, "%lld", &v);
    return v;
}

// ── Ledger build + serialize ──────────────────────────────────────────────────

static bool build_ledger(FsFileSystem* sd, const char* path,
                         s64 total_bytes, CheckpointLedger* out) {
    FsFile f;
    if (R_FAILED(fsFsOpenFile(sd, path, FsOpenMode_Read, &f))) return false;
    out->count = 0;
    s64 pos = 0;
    while (pos < total_bytes && out->count < MAX_CHECKPOINTS) {
        s64 end = pos + CHECKPOINT_SIZE;
        if (end > total_bytes) end = total_bytes;
        XXH32_state_t st;
        XXH32_reset(&st, 0);
        for (s64 off = pos; off < end; ) {
            u64 nr = 0;
            s64 take = end - off;
            if (take > (s64)sizeof(s_copy_buf)) take = sizeof(s_copy_buf);
            if (R_FAILED(fsFileRead(&f, off, s_copy_buf, (size_t)take,
                                    FsReadOption_None, &nr))) {
                fsFileClose(&f);
                return false;
            }
            XXH32_update(&st, s_copy_buf, (size_t)nr);
            off += (s64)nr;
        }
        out->h[out->count++] = XXH32_digest(&st);
        pos = end;
    }
    fsFileClose(&f);
    return true;
}

static void ledger_to_json(const CheckpointLedger* l, char* out, size_t sz) {
    int w = snprintf(out, sz, "[");
    for (int i = 0; i < l->count && (size_t)w < sz - 16; i++) {
        if (i > 0) w += snprintf(out + w, sz - w, ",");
        w += snprintf(out + w, sz - w, "%u", (unsigned)l->h[i]);
    }
    snprintf(out + w, sz - w, "]");
}

// ── Session persistence ───────────────────────────────────────────────────────

static void session_path(const char* key, char* out, size_t sz) {
    snprintf(out, sz, OMNI_ROOT "/outbound/%s/session.json", key);
}

// Returns server_verified_bytes (>= 0) on valid resume, -1 on stale/unavailable.
static s64 try_resume_session(FsFileSystem* sd, const char* key,
                               s64 local_total_bytes,
                               char* session_id_out, size_t sid_sz) {
    char sj_path[FS_MAX_PATH];
    session_path(key, sj_path, sizeof(sj_path));

    char buf[256] = {0};
    if (!fs_read_text_file(sd, sj_path, buf, sizeof(buf))) return -1;

    char saved_sid[64] = {0};
    if (!json_str(buf, "session_id", saved_sid, sizeof(saved_sid))) goto discard;
    if (json_int(buf, "manifest_posted", 0) != 1) goto discard;
    {
        long long saved_bytes = json_int(buf, "total_bytes", -1);
        if (saved_bytes != local_total_bytes) goto discard;

        char url[256], resp[256];
        snprintf(url, sizeof(url), "/api/v1/sync/sessions/%s/resume", saved_sid);
        int st = http_get_body(url, resp, sizeof(resp));
        if (st == 404) goto discard;
        if (st < 200 || st >= 300) return -1;  // transient — preserve session.json

        if (!strstr(resp, "ACTIVE")) goto discard;

        long long server_bytes = json_int(resp, "total_bytes", -1);
        if (server_bytes != local_total_bytes) goto discard;

        long long svb = json_int(resp, "server_verified_bytes", 0);
        snprintf(session_id_out, sid_sz, "%s", saved_sid);
        return (s64)svb;
    }

discard:
    fsFsDeleteFile(sd, sj_path);
    return -1;
}

// ── transport_upload (V2 window protocol) ─────────────────────────────────────
//
// Returns:  1 = success
//           0 = transport failure
//          -1 = cooperative yield (game-close interrupt, no failure penalty)
//
// preservation=true: upload with "preservation":true in the inbound body.
// Preservation uploads are archived server-side but never advance HEAD and
// never fan out to peers — they are a pre-restore safety backup only.

int transport_upload(FsFileSystem* sd, const char* key, int* out_snap_seq,
                     bool preservation) {
    char packed[FS_MAX_PATH];
    snprintf(packed, sizeof(packed), OMNI_ROOT "/outbound/%s/save.zip", key);

    FsFile pf;
    if (R_FAILED(fsFsOpenFile(sd, packed, FsOpenMode_Read, &pf))) {
        fs_log(sd, "UPLOAD_NO_PACK key=%s", key);
        return 0;
    }
    s64 total_bytes = 0;
    fsFileGetSize(&pf, &total_bytes);
    fsFileClose(&pf);
    if (total_bytes <= 0) return 0;

    s64 n_checkpoints = (total_bytes + CHECKPOINT_SIZE - 1) / CHECKPOINT_SIZE;
    if (n_checkpoints > MAX_CHECKPOINTS) {
        fs_log(sd, "UPLOAD_TOO_LARGE key=%s bytes=%lld", key, (long long)total_bytes);
        return 0;
    }

    char tid[17] = {0};
    sscanf(key, "%*18[^-]-%16[^-]", tid);

    char session_id[64] = {0};
    s64 verified_bytes = try_resume_session(sd, key, total_bytes,
                                             session_id, sizeof(session_id));
    if (verified_bytes >= 0) {
        fs_log(sd, "UPLOAD_RESUME key=%s session=%s vb=%lld/%lld",
               key, session_id, (long long)verified_bytes, (long long)total_bytes);
    } else {
        // ── Fresh session ──────────────────────────────────────────────────────
        int parent_seq = 0;
        {
            char state_path[FS_MAX_PATH];
            char state_buf[256];
            snprintf(state_path, sizeof(state_path),
                     OMNI_ROOT "/state/last_backup_%s.json", tid);
            if (fs_read_text_file(sd, state_path, state_buf, sizeof(state_buf))) {
                long long n = json_int(state_buf, "snapshot_counter", 0);
                if (n > parent_seq) parent_seq = (int)n;
            }
            snprintf(state_path, sizeof(state_path),
                     OMNI_ROOT "/state/last_restore_%s.json", tid);
            if (fs_read_text_file(sd, state_path, state_buf, sizeof(state_buf))) {
                long long n = json_int(state_buf, "snapshot_counter", 0);
                if (n > parent_seq) parent_seq = (int)n;
            }
        }
        char req_body[320], resp[512];
        if (preservation) {
            snprintf(req_body, sizeof(req_body),
                "{\"title_id\":\"%s\","
                "\"total_size_bytes\":%lld,"
                "\"hardware_type\":\"%s\","
                "\"user_key\":\"%s\","
                "\"user_display\":\"%s\","
                "\"preservation\":true}",
                tid, (long long)total_bytes, s_hw_type, s_uid_hex, s_account_nickname);
        } else if (parent_seq > 0) {
            snprintf(req_body, sizeof(req_body),
                "{\"title_id\":\"%s\","
                "\"total_size_bytes\":%lld,"
                "\"hardware_type\":\"%s\","
                "\"user_key\":\"%s\","
                "\"user_display\":\"%s\","
                "\"parent_sequence_num\":%d}",
                tid, (long long)total_bytes, s_hw_type, s_uid_hex, s_account_nickname,
                parent_seq);
        } else {
            snprintf(req_body, sizeof(req_body),
                "{\"title_id\":\"%s\","
                "\"total_size_bytes\":%lld,"
                "\"hardware_type\":\"%s\","
                "\"user_key\":\"%s\","
                "\"user_display\":\"%s\"}",
                tid, (long long)total_bytes, s_hw_type, s_uid_hex, s_account_nickname);
        }

        int status = http_post_json("/api/v1/sync/transactions/inbound",
                                    req_body, resp, sizeof(resp));
        if (status == 401) {
            fs_log(sd, "UPLOAD_AUTH_REJECTED key=%s", key);
            return -2;
        }
        if (status < 200 || status >= 300) {
            fs_log(sd, "UPLOAD_START_FAIL status=%d key=%s", status, key);
            return 0;
        }
        if (!json_str(resp, "session_id", session_id, sizeof(session_id))) {
            fs_log(sd, "UPLOAD_PARSE_FAIL key=%s resp=%.128s", key, resp);
            return 0;
        }

        // Build checkpoint ledger from packed save.
        CheckpointLedger ledger;
        if (!build_ledger(sd, packed, total_bytes, &ledger)) {
            fs_log(sd, "UPLOAD_LEDGER_FAIL key=%s", key);
            return 0;
        }

        char ledger_json[LEDGER_JSON_MAX];
        ledger_to_json(&ledger, ledger_json, sizeof(ledger_json));

        char manifest_body[MANIFEST_MAX];
        snprintf(manifest_body, sizeof(manifest_body),
                 "{\"checkpoint_size\":%d,\"checkpoint_ledger\":%s}",
                 CHECKPOINT_SIZE, ledger_json);

        char manifest_url[256], manifest_resp[128];
        snprintf(manifest_url, sizeof(manifest_url),
                 "/api/v1/sync/sessions/%s/manifest", session_id);
        status = http_post_json(manifest_url, manifest_body,
                                manifest_resp, sizeof(manifest_resp));
        if (status < 200 || status >= 300) {
            fs_log(sd, "MANIFEST_FAIL status=%d key=%s", status, key);
            return 0;
        }

        // Persist session — manifest is now frozen on server.
        char sj_path[FS_MAX_PATH], sj_body[128];
        session_path(key, sj_path, sizeof(sj_path));
        snprintf(sj_body, sizeof(sj_body),
                 "{\"session_id\":\"%s\",\"total_bytes\":%lld,\"manifest_posted\":1}\n",
                 session_id, (long long)total_bytes);
        state_atomic_write(sd, sj_path, sj_body);
        verified_bytes = 0;
    }

    // ── Upload windows ─────────────────────────────────────────────────────────

    char window_url[256];
    snprintf(window_url, sizeof(window_url),
             "/api/v1/sync/sessions/%s/window", session_id);

    CURL* upload_curl = http_upload_handle_create();
    if (!upload_curl) {
        fs_log(sd, "UPLOAD_HANDLE_FAIL key=%s", key);
        return 0;
    }

    // Write 0% immediately so overlay shows progress during setup overhead.
    state_write_status(sd, UPLOADING, key, verified_bytes, total_bytes);
    int last_pct = (int)((verified_bytes * 100) / total_bytes);
    while (verified_bytes < total_bytes) {
        if (s_pending_extract_title.load(std::memory_order_relaxed) != 0) {
            fs_log(sd, "UPLOAD_INTERRUPT game_close vb=%lld key=%s",
                   (long long)verified_bytes, key);
            http_handle_close(upload_curl);
            return -1;
        }
        if (s_system_sleeping.load(std::memory_order_acquire)) {
            fs_log(sd, "UPLOAD_INTERRUPT sleep vb=%lld key=%s",
                   (long long)verified_bytes, key);
            http_handle_close(upload_curl);
            return -1;
        }

        s64 window = total_bytes - verified_bytes;
        if (window > WINDOW_SIZE) window = WINDOW_SIZE;

        char resp_buf[256];
        s64 svb = -1;
        for (int r = 0; r < 3; r++) {
            svb = http_put_window(upload_curl, window_url, sd, packed,
                                  verified_bytes, window,
                                  resp_buf, sizeof(resp_buf));
            if (svb >= 0) break;
            if (svb == -2 || s_system_sleeping.load(std::memory_order_acquire)) {
                fs_log(sd, "UPLOAD_INTERRUPT sleep vb=%lld key=%s",
                       (long long)verified_bytes, key);
                http_handle_close(upload_curl);
                return -1;
            }
            if (r + 1 < 3) svcSleepThread(1000000000ULL);
        }
        if (svb < 0) {
            fs_log(sd, "WINDOW_FAIL key=%s vb=%lld err=%s",
                   key, (long long)verified_bytes, g_http_last_err);
            http_handle_close(upload_curl);
            return 0;
        }
        verified_bytes = svb;
        int pct = (int)((verified_bytes * 100) / total_bytes);
        if (pct != last_pct) {
            state_write_status(sd, UPLOADING, key, verified_bytes, total_bytes);
            last_pct = pct;
        }
    }
    if (last_pct != 100)
        state_write_status(sd, UPLOADING, key, total_bytes, total_bytes);

    http_handle_close(upload_curl);

    // ── Commit ─────────────────────────────────────────────────────────────────

    char commit_url[256], commit_resp[128];
    snprintf(commit_url, sizeof(commit_url),
             "/api/v1/sync/sessions/%s/commit", session_id);
    int status = http_post_empty(commit_url, commit_resp, sizeof(commit_resp));
    if (status != 202 && (status < 200 || status >= 300)) {
        fs_log(sd, "COMMIT_FAIL status=%d key=%s", status, key);
        return 0;
    }

    /* seq is assigned asynchronously by the processing worker; absent from commit response */
    if (out_snap_seq) *out_snap_seq = -1;

    char sj_path[FS_MAX_PATH];
    session_path(key, sj_path, sizeof(sj_path));
    fsFsDeleteFile(sd, sj_path);

    fs_log(sd, "UPLOAD_OK key=%s session=%s bytes=%lld seq=pending",
           key, session_id, (long long)total_bytes);
    return 1;
}
