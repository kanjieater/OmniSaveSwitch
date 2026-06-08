#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "omnisave.h"
#include "sync_prefs.h"

#pragma GCC diagnostic ignored "-Wformat-truncation"

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

// Find "key":[...] and parse comma-separated uint32 values.
// Returns count on success (may be 0 for empty array), -1 on parse error.
static int json_array_u32(const char* json, const char* key,
                           uint32_t* out, int max_count) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char* p = strstr(json, needle);
    if (!p) return -1;
    p += strlen(needle);
    while (*p == ' ') p++;
    if (*p != '[') return -1;
    p++;
    int count = 0;
    while (count < max_count) {
        while (*p == ' ' || *p == ',') p++;
        if (*p == ']' || *p == '\0') break;
        char* end;
        unsigned long v = strtoul(p, &end, 10);
        if (end == p) return -1;
        out[count++] = (uint32_t)v;
        p = end;
    }
    return count;
}

// ── Truncate a file on the SD card to exactly new_size bytes ─────────────────

static void truncate_sd_file(FsFileSystem* sd, const char* path, s64 new_size) {
    FsFile f;
    if (R_SUCCEEDED(fsFsOpenFile(sd, path, FsOpenMode_Write, &f))) {
        fsFileSetSize(&f, new_size);
        fsFileClose(&f);
    }
}

// ── Download state ────────────────────────────────────────────────────────────

typedef struct {
    char             txn_id[64];
    s64              total_bytes;
    int              snap_id;
    CheckpointLedger ledger;
} DownloadState;

// ── download_transaction ──────────────────────────────────────────────────────
// Returns: 1 = success, 0 = transport failure, -1 = cooperative yield

static int download_transaction(FsFileSystem* sd, const DownloadState* ds,
                                 const char* inbound_key) {
    char tmp_dir[FS_MAX_PATH], tmp_file[FS_MAX_PATH];
    snprintf(tmp_dir,  sizeof(tmp_dir),  OMNI_ROOT "/tmp_in/%s",          inbound_key);
    snprintf(tmp_file, sizeof(tmp_file), OMNI_ROOT "/tmp_in/%s/save.zip", inbound_key);

    // Resume: align existing file to last complete checkpoint boundary.
    s64 verified_bytes = 0;
    {
        FsFile pf;
        if (R_SUCCEEDED(fsFsOpenFile(sd, tmp_file, FsOpenMode_Read, &pf))) {
            s64 sz = 0;
            fsFileGetSize(&pf, &sz);
            fsFileClose(&pf);
            verified_bytes = (sz / CHECKPOINT_SIZE) * CHECKPOINT_SIZE;
            if (verified_bytes != sz)
                truncate_sd_file(sd, tmp_file, verified_bytes);
            if (verified_bytes > 0) {
                if (verified_bytes % CHECKPOINT_SIZE != 0 ||
                    verified_bytes > ds->total_bytes) {
                    fs_log(sd, "DL_CORRUPT_RESET vb=%lld total=%lld key=%s",
                           (long long)verified_bytes, (long long)ds->total_bytes,
                           inbound_key);
                    fsFsDeleteDirectoryRecursively(sd, tmp_dir);
                    verified_bytes = 0;
                } else {
                    fs_log(sd, "DL_RESUME vb=%lld/%lld key=%s",
                           (long long)verified_bytes, (long long)ds->total_bytes,
                           inbound_key);
                }
            }
        }
    }

    if (verified_bytes == 0) {
        fsFsDeleteDirectoryRecursively(sd, tmp_dir);
        fsFsCreateDirectory(sd, tmp_dir);
        if (R_FAILED(fsFsCreateFile(sd, tmp_file, 0, 0))) return 0;

        // Write state.json so recovery_sweep identifies this partial download on crash.
        char state_path[FS_MAX_PATH], state_json[96];
        snprintf(state_path, sizeof(state_path),
                 OMNI_ROOT "/tmp_in/%s/state.json", inbound_key);
        snprintf(state_json, sizeof(state_json),
                 "{\"transaction_id\":\"%s\"}\n", ds->txn_id);
        fs_write_text_file(sd, state_path, state_json);
        fsFsCommit(sd);
    }

    // Window loop.
    char url[256];
    snprintf(url, sizeof(url), "/api/v1/sync/transactions/%s/range", ds->txn_id);

    CURL* dl_curl = http_download_handle_create();
    if (!dl_curl) {
        fs_log(sd, "DL_HANDLE_FAIL key=%s", inbound_key);
        return 0;
    }

    // Write initial progress immediately so overlay shows % during setup.
    state_write_status(sd, DOWNLOADING, inbound_key, verified_bytes, ds->total_bytes);
    int last_pct = (int)((verified_bytes * 100) / ds->total_bytes);
    while (verified_bytes < ds->total_bytes) {
        if (s_pending_extract_title.load(std::memory_order_relaxed) != 0) {
            fs_log(sd, "DL_INTERRUPT game_close vb=%lld key=%s",
                   (long long)verified_bytes, inbound_key);
            http_handle_close(dl_curl);
            return -1;
        }
        if (s_system_sleeping) {
            fs_log(sd, "DL_INTERRUPT sleep vb=%lld key=%s",
                   (long long)verified_bytes, inbound_key);
            http_handle_close(dl_curl);
            return -1;
        }

        s64 new_vb = -1;
        for (int r = 0; r < 3; r++) {
            new_vb = http_get_window_validated(
                dl_curl, url, sd, tmp_file, verified_bytes, WINDOW_SIZE,
                &ds->ledger, CHECKPOINT_SIZE, ds->total_bytes);
            if (new_vb == -2 || s_system_sleeping) {
                fs_log(sd, "DL_INTERRUPT sleep vb=%lld key=%s",
                       (long long)verified_bytes, inbound_key);
                http_handle_close(dl_curl);
                return -1;
            }
            if (new_vb > verified_bytes) break;
            truncate_sd_file(sd, tmp_file, verified_bytes);
            fs_log(sd, "DL_RETRY vb=%lld r=%d key=%s",
                   (long long)verified_bytes, r, inbound_key);
            if (r + 1 < 3) svcSleepThread(1000000000ULL);
        }
        if (new_vb <= verified_bytes) {
            fs_log(sd, "DL_WINDOW_FAIL vb=%lld key=%s",
                   (long long)verified_bytes, inbound_key);
            http_handle_close(dl_curl);
            return 0;
        }
        verified_bytes = new_vb;
        fsFsCommit(sd);
        int pct = (int)((verified_bytes * 100) / ds->total_bytes);
        if (pct != last_pct) {
            state_write_status(sd, DOWNLOADING, inbound_key, verified_bytes, ds->total_bytes);
            last_pct = pct;
        }
    }
    if (last_pct != 100)
        state_write_status(sd, DOWNLOADING, inbound_key, ds->total_bytes, ds->total_bytes);

    http_handle_close(dl_curl);

    // Final size validation.
    {
        FsFile vf;
        s64 actual = 0;
        if (R_SUCCEEDED(fsFsOpenFile(sd, tmp_file, FsOpenMode_Read, &vf))) {
            fsFileGetSize(&vf, &actual);
            fsFileClose(&vf);
        }
        if (actual != ds->total_bytes) {
            fs_log(sd, "DL_SIZE_MISMATCH expected=%lld actual=%lld key=%s",
                   (long long)ds->total_bytes, (long long)actual, inbound_key);
            fsFsDeleteDirectoryRecursively(sd, tmp_dir);
            return 0;
        }
    }

    // Atomic promote: tmp_in/<key>/ → inbound/<key>/
    char inbound_dir[FS_MAX_PATH];
    snprintf(inbound_dir, sizeof(inbound_dir), OMNI_ROOT "/inbound/%s", inbound_key);
    fsFsDeleteDirectoryRecursively(sd, inbound_dir);
    if (R_FAILED(fsFsRenameDirectory(sd, tmp_dir, inbound_dir))) {
        fsFsDeleteDirectoryRecursively(sd, tmp_dir);
        return 0;
    }
    fsFsCommit(sd);
    fs_log(sd, "DL_OK vb=%lld key=%s txn=%s",
           (long long)verified_bytes, inbound_key, ds->txn_id);
    return 1;
}

// ── transport_poll_inbound ─────────────────────────────────────────────────────

int transport_poll_inbound(FsFileSystem* sd) {
    // Queue body now includes sync_prefs, backup_updates, and game_names in addition
    // to pending items with checkpoint ledgers.  64 KB gives comfortable headroom;
    // cb_write_buf aborts with CURLE_WRITE_ERROR if even this is exceeded.
    static char queue_body[65536];
    char url[128];
    snprintf(url, sizeof(url), "/api/v1/sync/queue?sync_generation=%u",
             state_get_sync_generation());
    int status = http_get_body(url, queue_body, sizeof(queue_body));
    if (status == 401) {
        fs_log(sd, "POLL_AUTH_REJECTED");
        return -2;
    }
    if (status < 200 || status >= 300) {
        fs_log(sd, "POLL_FAIL status=%d err=%s", status, g_http_last_err);
        return 0;
    }

    // Apply backup state delta before processing the pending queue.
    {
        long long srv_gen = json_int(queue_body, "sync_generation", -1);
        if (srv_gen > (long long)state_get_sync_generation()) {
            const char* upd = strstr(queue_body, "\"backup_updates\":");
            if (upd) {
                upd = strchr(upd, '[');
                if (upd) state_apply_backup_updates(sd, upd);
            }
            state_set_sync_generation(sd, (uint32_t)srv_gen);
            fs_log(sd, "SYNC_GEN_ADVANCE gen=%lld", srv_gen);
        }
    }

    const char* arr = strstr(queue_body, "\"pending\":");
    if (!arr) return 0;
    arr = strchr(arr, '[');
    if (!arr) return 0;
    arr++;

    int fetched = 0;
    while (fetched < 4) {
        if (s_pending_extract_title.load(std::memory_order_relaxed) != 0) {
            fs_log(sd, "POLL_INTERRUPT game_close_pending");
            break;
        }
        const char* obj_start = strchr(arr, '{');
        if (!obj_start) break;
        const char* obj_end = strchr(obj_start, '}');
        if (!obj_end) break;

        size_t obj_len = (size_t)(obj_end - obj_start + 1);
        if (obj_len >= 4096) { arr = obj_end + 1; continue; }
        char entry[4096];
        memcpy(entry, obj_start, obj_len);
        entry[obj_len] = '\0';

        char txn_id[64] = {0}, title_id[17] = {0}, user_key[17] = {0};
        if (!json_str(entry, "transaction_id", txn_id, sizeof(txn_id)) ||
            !json_str(entry, "title_id",       title_id, sizeof(title_id))) {
            arr = obj_end + 1;
            continue;
        }
        json_str(entry, "target_profile_uid", user_key, sizeof(user_key));

        if (fsm_is_title_inject_blocked(title_id)) {
            arr = obj_end + 1;
            continue;
        }

        DownloadState ds;
        memset(&ds, 0, sizeof(ds));
        snprintf(ds.txn_id, sizeof(ds.txn_id), "%s", txn_id);
        ds.total_bytes = json_int(entry, "total_bytes", 0);
        ds.snap_id     = (int)json_int(entry, "snapshot_sequence", 0);

        if (ds.total_bytes <= 0) {
            fs_log(sd, "POLL_NO_SIZE txn=%s", txn_id);
            arr = obj_end + 1;
            continue;
        }

        int lcount = json_array_u32(entry, "checkpoint_ledger",
                                     ds.ledger.h, MAX_CHECKPOINTS);
        if (lcount <= 0) {
            fs_log(sd, "POLL_NO_LEDGER txn=%s", txn_id);
            arr = obj_end + 1;
            continue;
        }
        ds.ledger.count = lcount;

        // Deterministic inbound key derived from txn_id so the same transaction
        // always maps to the same SD path — enables artifact reuse on inject retry.
        // Format: <18 hex chars from txn_id, dashes stripped>-<title_id>-<user_key>
        // (52 chars total, matching SNAP_KEY_LEN)
        char inbound_key[SNAP_KEY_LEN + 2] = {0};
        {
            char compact[20] = {0};
            int k = 0;
            for (int i = 0; txn_id[i] && k < 18; i++)
                if (txn_id[i] != '-') compact[k++] = txn_id[i];
            snprintf(inbound_key, sizeof(inbound_key),
                     "%.18s-%.16s-%.16s", compact, title_id,
                     user_key[0] ? user_key : "0000000000000000");
        }

        // Fast-path: validated artifact already present at the expected size.
        // Just refresh the meta JSON and skip the download.
        {
            char fast_bin[FS_MAX_PATH];
            snprintf(fast_bin, sizeof(fast_bin),
                     OMNI_ROOT "/inbound/%s/save.zip", inbound_key);
            FsFile ef;
            if (R_SUCCEEDED(fsFsOpenFile(sd, fast_bin, FsOpenMode_Read, &ef))) {
                s64 actual = 0;
                fsFileGetSize(&ef, &actual);
                fsFileClose(&ef);
                if (actual == ds.total_bytes) {
                    char meta_path[FS_MAX_PATH], meta_json[128];
                    snprintf(meta_path, sizeof(meta_path),
                             OMNI_ROOT "/inbound/%s.json", inbound_key);
                    snprintf(meta_json, sizeof(meta_json),
                             "{\"transaction_id\":\"%s\",\"snap_id\":%d}\n",
                             ds.txn_id, ds.snap_id);
                    fs_write_text_file(sd, meta_path, meta_json);
                    fsFsCommit(sd);
                    fs_log(sd, "DL_SKIP_EXISTS vb=%lld key=%s txn=%s",
                           (long long)ds.total_bytes, inbound_key, txn_id);
                    fetched++;
                    arr = obj_end + 1;
                    continue;
                }
            }
        }

        {
            char dl_label[32] = {0};
            u64 tid_u = (u64)strtoull(title_id, NULL, 16);
            title_label(tid_u, dl_label, sizeof(dl_label));
            char dl_msg[96];
            snprintf(dl_msg, sizeof(dl_msg), "Downloading %s...", dl_label);
            notif_push("download-start", dl_msg);
        }
        int dl_rc = download_transaction(sd, &ds, inbound_key);
        if (dl_rc == 1) {
            char meta_path[FS_MAX_PATH], meta_json[128];
            snprintf(meta_path, sizeof(meta_path),
                     OMNI_ROOT "/inbound/%s.json", inbound_key);
            snprintf(meta_json, sizeof(meta_json),
                     "{\"transaction_id\":\"%s\",\"snap_id\":%d}\n",
                     txn_id, ds.snap_id);
            fs_write_text_file(sd, meta_path, meta_json);
            fsFsCommit(sd);
            fetched++;
        } else if (dl_rc == -1) {
            fs_log(sd, "POLL_YIELD txn=%s", txn_id);
            break;
        } else {
            fs_log(sd, "DL_FAIL key=%s txn=%s", inbound_key, txn_id);
        }

        arr = obj_end + 1;
    }

    if (fetched > 0) {
        s_last_activity_posix = get_posix_utc();
        fs_log(sd, "POLL_OK fetched=%d", fetched);
    } else {
        const char* hint = strstr(queue_body, "\"hint\":");
        if (hint && strstr(hint, "queue_hint")) {
            if (s_poll_hot_remain < 5) s_poll_hot_remain = 5;
            s_heartbeat_ticks = 1;
            fs_log(sd, "POLL_HINT queue_hint");
        }
    }

    // Refresh client sync preferences and game names from every heartbeat response.
    sync_prefs_apply_from_queue(sd, queue_body);
    sync_prefs_apply_game_names(queue_body);
    return 0;
}
