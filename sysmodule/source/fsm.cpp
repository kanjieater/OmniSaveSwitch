#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "omnisave.h"
#include "sync_prefs.h"

// ── FSM persistent state ───────────────────────────────────────────────────────

static FsmState s_state          = IDLE;
static char     s_active_key[SNAP_KEY_LEN + 2] = {0};
static int      s_active_snap_id = 0;
static int      s_retry_delay    = 5;
static int      s_retry_ticks    = 0;
static int      s_recent_remain  = 0;
static bool     s_server_notified      = false;
static bool     s_unpaired_notif_shown = false;
static char     s_upload_notif_key[SNAP_KEY_LEN + 2] = {0};

// ── Inject retry state ────────────────────────────────────────────────────────
// Per-title_id failure streak. After 3 failures: permanent FAILED on server.
// Resets on success, boot, or wake (game might have been installed since).

static char s_inject_fail_tid[17] = {0};
static int  s_inject_fail_count   = 0;

// Titles blocked for the session (inject returned -1: game not installed).
// Cleared on boot/wake. transport_poll_inbound checks this before downloading.
#define INJECT_BLOCK_MAX 8
static char s_inject_blocked[INJECT_BLOCK_MAX][17] = {{0}};
static int  s_inject_blocked_count = 0;

// ── Upload scheduler state ─────────────────────────────────────────────────────
// Per-key failure streak + wall-clock deferral cooldown.

static char s_fail_key[SNAP_KEY_LEN + 2]    = {0};
static int  s_fail_streak                    = 0;
static char s_deferred_key[SNAP_KEY_LEN + 2] = {0};
static u64  s_deferred_until_posix           = 0;

// ── Helpers ────────────────────────────────────────────────────────────────────

static void set_state(FsmState next, FsFileSystem* sd) {
    if (s_state != next)
        fs_log(sd, "FSM %s → %s", fsm_state_name(s_state), fsm_state_name(next));
    s_state = next;
    state_write_status(sd, next, s_active_key, 0, 0);
}

// Transition state without writing status.json or logging.
// Use for transient intermediate states that should be invisible to the overlay.
static void set_state_quiet(FsmState next) {
    s_state = next;
}

void title_label(u64 title_id, char* out, size_t sz) {
    char hex[17];
    snprintf(hex, sizeof(hex), "%016llX", (unsigned long long)title_id);
    if (!get_title_name(title_id, out, sz) &&
        !sync_prefs_get_game_name(title_id, out, sz))
        snprintf(out, sz, "%.16s", hex);
    if (strlen(out) > 28) out[28] = '\0';
}

static void clear_scheduler(void) {
    s_fail_streak         = 0;
    s_fail_key[0]         = '\0';
    s_deferred_key[0]     = '\0';
    s_deferred_until_posix = 0;
}

// ── Public predicates ──────────────────────────────────────────────────────────

bool fsm_is_title_inject_blocked(const char* title_id) {
    for (int i = 0; i < s_inject_blocked_count; i++)
        if (strcmp(s_inject_blocked[i], title_id) == 0) return true;
    return false;
}

static void fsm_block_title(const char* title_id) {
    if (fsm_is_title_inject_blocked(title_id)) return;
    if (s_inject_blocked_count < INJECT_BLOCK_MAX)
        snprintf(s_inject_blocked[s_inject_blocked_count++], 17, "%s", title_id);
}

bool fsm_is_key_deferred(const char* key) {
    return s_deferred_key[0] != '\0' &&
           strcmp(key, s_deferred_key) == 0 &&
           get_posix_utc() < s_deferred_until_posix;
}

// ── IDLE processing ────────────────────────────────────────────────────────────

static void do_idle(FsFileSystem* sd, const InputSnapshot* snap) {
    // Expire deferral cooldown at tick start.
    if (s_deferred_key[0] != '\0' && get_posix_utc() >= s_deferred_until_posix)
        s_deferred_key[0] = '\0';

    if (snap->storage_critical) {
        notif_push("storage-full", "SD Card Full (>95%) — Backup Paused");
        state_append_event(sd, "warning", "SD Card Full (>95%) — Backup Paused");
        return;
    }

    // Outbound takes precedence over inbound (spec §2.4).
    if (snap->has_outbound) {
        if (g_config.device_token[0] == '\0') {
            // No token — device not paired. Block upload; notify once per boot/wake.
            if (!s_unpaired_notif_shown) {
                s_unpaired_notif_shown = true;
                notif_push("unpaired", "Not Paired — Visit OmniSave to Pair");
                state_append_event(sd, "warning", "Upload blocked: device not paired");
                fs_log(sd, "UPLOAD_SKIP_UNPAIRED outbound_pending");
            }
            // Fall through — don't upload, still check heartbeat/inbound below.
        } else if (fsm_is_key_deferred(snap->outbound_key)) {
            // Cooldown still active — fall through to inbound/heartbeat.
        } else {
            char _ob_tid[17] = {0};
            sscanf(snap->outbound_key, "%*[^-]-%16[^-]", _ob_tid);
            u64 _ob_tid_u = (u64)strtoull(_ob_tid, NULL, 16);
            if (!sync_prefs_is_enabled(_ob_tid_u)) {
                // User disabled this title — remove staged outbound and skip upload.
                char _ob_dir[FS_MAX_PATH], _ob_json[FS_MAX_PATH];
                snprintf(_ob_dir,  sizeof(_ob_dir),  OMNI_ROOT "/outbound/%s",      snap->outbound_key);
                snprintf(_ob_json, sizeof(_ob_json), OMNI_ROOT "/outbound/%s.json", snap->outbound_key);
                fsFsDeleteDirectoryRecursively(sd, _ob_dir);
                fsFsDeleteFile(sd, _ob_json);
                fsFsCommit(sd);
                fs_log(sd, "UPLOAD_SKIP_PREFS title=%s", _ob_tid);
                // Fall through to inbound/heartbeat.
            } else {
                snprintf(s_active_key, sizeof(s_active_key), "%s", snap->outbound_key);
                s_active_snap_id = 0;
                set_state(UPLOADING, sd);
                return;
            }
        }
    }

    if (snap->has_inbound && !snap->game_running) {
        char _ib_tid[17] = {0};
        sscanf(snap->inbound_key, "%*[^-]-%16[^-]", _ib_tid);
        u64 _ib_tid_u = (u64)strtoull(_ib_tid, NULL, 16);
        if (!sync_prefs_is_enabled(_ib_tid_u)) {
            // User disabled this title — keep inbound on SD, skip inject.
            fs_log(sd, "INJECT_SKIP_PREFS title=%s", _ib_tid);
            // Fall through to heartbeat check.
        } else {
            snprintf(s_active_key, sizeof(s_active_key), "%s", snap->inbound_key);
            s_active_snap_id = snap->inbound_snap_id;
            set_state(INBOUND_READY, sd);
            return;
        }
    }

    if (snap->heartbeat_due && g_config.server_host[0] != '\0' &&
            g_config.device_token[0] != '\0') {
        set_state_quiet(DOWNLOADING);
    }
}

// ── DOWNLOADING processing ─────────────────────────────────────────────────────

static void do_downloading(FsFileSystem* sd, const InputSnapshot* snap) {
    (void)snap;
    if (!wait_network_ready()) {
        s_network_was_down = true;
        set_state(IDLE, sd);
        return;
    }
    if (s_network_was_down) {
        s_network_was_down    = false;
        s_poll_hot_remain     = (s_poll_hot_remain > 30) ? s_poll_hot_remain : 30;
        s_last_activity_posix = get_posix_utc();
        s_heartbeat_ticks     = 1;
        fs_log(sd, "NETWORK_RECONNECT hot");
    }
    if (!s_server_notified) {
        notif_verbose("server-ready", "Connected to OmniSave");
        s_server_notified = true;
    }
    int poll_result = transport_poll_inbound(sd);
    if (poll_result == -2) {
        // Server rejected token (401) — revoked. Clear locally so device re-pairs.
        g_config.device_token[0] = '\0';
        config_set_device_token(sd, "");
        fs_log(sd, "TOKEN_REVOKED_CLEARING poll");
        set_state(IDLE, sd);
        return;
    }
    if (poll_result == 0) {
        set_state_quiet(IDLE); // nothing found — no write, no log
    } else {
        set_state(IDLE, sd);   // work found or transport error — write status
    }
}

// ── UPLOADING processing ───────────────────────────────────────────────────────

static void do_uploading(FsFileSystem* sd, const InputSnapshot* snap) {
    (void)snap;

    if (g_config.server_host[0] == '\0') {
        fs_log(sd, "UPLOAD_SKIP no server configured");
        set_state(IDLE, sd);
        s_dirty = false;
        return;
    }

    if (g_config.device_token[0] == '\0') {
        if (!s_unpaired_notif_shown) {
            s_unpaired_notif_shown = true;
            notif_push("unpaired", "Not Paired — Visit OmniSave to Pair");
            state_append_event(sd, "warning", "Upload blocked: device not paired");
            fs_log(sd, "UPLOAD_SKIP_UNPAIRED key=%s", s_active_key);
        }
        set_state(IDLE, sd);
        return;
    }

    if (!wait_network_ready()) {
        fs_log(sd, "UPLOAD_FAIL no network key=%s", s_active_key);
        notif_verbose("upload-fail", "No Network — Will Retry");
        state_append_event(sd, "warning", "No Network — Will Retry");
        s_retry_ticks = s_retry_delay;
        s_retry_delay = (s_retry_delay * 2 > 60) ? 60 : s_retry_delay * 2;
        set_state(RETRY_BACKOFF, sd);
        return;
    }

    if (strcmp(s_active_key, s_upload_notif_key) != 0) {
        snprintf(s_upload_notif_key, sizeof(s_upload_notif_key), "%s", s_active_key);
        char tid[17] = {0}, uid[17] = {0}, ts[20] = {0};
        sscanf(s_active_key, "%18[^-]-%16[^-]-%16s", ts, tid, uid);
        char label[32];
        u64 tid_u = (u64)strtoull(tid, NULL, 16);
        title_label(tid_u, label, sizeof(label));
        char msg[96];
        snprintf(msg, sizeof(msg), "Uploading %s...", label);
        notif_push("upload-start", msg);
        state_append_event(sd, "info", msg);
    }
    int snap_seq = -1;
    int snap_id = transport_upload(sd, s_active_key, &snap_seq, false);

    if (snap_id == -2) {
        // Server rejected token (401) — revoked since last boot. Clear it locally so
        // poll_device_config takes over and the user gets the "not paired" notification.
        g_config.device_token[0] = '\0';
        config_set_device_token(sd, "");
        fs_log(sd, "TOKEN_REVOKED_CLEARING key=%s", s_active_key);
        set_state(IDLE, sd);
        return;
    }

    if (snap_id == -1) {
        // Cooperative yield: game closed or sleep during upload. No failure penalty.
        // Session remains ACTIVE on server for resume.
        fs_log(sd, "UPLOAD_YIELD key=%s", s_active_key);
        set_state(IDLE, sd);
        return;
    }

    if (snap_id > 0) {
        char tid[17] = {0}, uid[17] = {0}, ts[20] = {0};
        sscanf(s_active_key, "%18[^-]-%16[^-]-%16s", ts, tid, uid);

        int counter = 0;
        char old_base[SNAP_KEY_LEN + 2] = {0};
        state_read_lineage(sd, tid, uid, old_base, sizeof(old_base), &counter);
        state_write_lineage(sd, tid, uid, s_active_key, counter + 1);

        char out_path[FS_MAX_PATH];
        snprintf(out_path, sizeof(out_path), OMNI_ROOT "/outbound/%s/save.zip", s_active_key);
        snap_hash_update_from_archive(sd, s_active_key, out_path);
        snprintf(out_path, sizeof(out_path), OMNI_ROOT "/outbound/%s", s_active_key);
        fsFsDeleteDirectoryRecursively(sd, out_path);
        snprintf(out_path, sizeof(out_path), OMNI_ROOT "/outbound/%s.json", s_active_key);
        fsFsDeleteFile(sd, out_path);
        fsFsCommit(sd);

        char label[32];
        u64 tid_u = (u64)strtoull(tid, NULL, 16);
        title_label(tid_u, label, sizeof(label));
        char msg[96];
        snprintf(msg, sizeof(msg), "%s Backed Up", label);
        notif_push("upload-ok", msg);
        state_append_event(sd, "info", msg);
        clear_scheduler();
        s_retry_delay    = 5;
        s_recent_remain  = 4;
        s_dirty          = false;
        s_heartbeat_ticks    = 1;
        s_poll_hot_remain    = (s_poll_hot_remain > 15) ? s_poll_hot_remain : 15;
        s_last_activity_posix = get_posix_utc();
        fs_log(sd, "UPLOAD_OK key=%s snap=%d", s_active_key, snap_id);
        s_active_key[0] = '\0';
        set_state(IDLE, sd);
    } else {
        // Transport failure.
        fs_log(sd, "UPLOAD_FAIL key=%s err=%s", s_active_key, g_http_last_err);

        // Per-key failure streak.
        if (strcmp(s_active_key, s_fail_key) != 0) {
            s_fail_streak = 0;
            snprintf(s_fail_key, sizeof(s_fail_key), "%s", s_active_key);
        }
        if (++s_fail_streak >= 3) {
            snprintf(s_deferred_key, sizeof(s_deferred_key), "%s", s_active_key);
            s_deferred_until_posix = get_posix_utc() + 60;
            s_fail_streak = 0;
            fs_log(sd, "UPLOAD_DEFERRED key=%s", s_active_key);
        }

        char label[32];
        {
            char tid[17] = {0}, uid[17] = {0}, ts[20] = {0};
            sscanf(s_active_key, "%18[^-]-%16[^-]-%16s", ts, tid, uid);
            u64 tid_u = (u64)strtoull(tid, NULL, 16);
            title_label(tid_u, label, sizeof(label));
        }
        char msg[96];
        snprintf(msg, sizeof(msg), "%s Upload Failed — Retrying", label);
        notif_verbose("upload-fail", msg);
        state_append_event(sd, "warning", msg);
        s_retry_ticks = s_retry_delay;
        s_retry_delay = (s_retry_delay * 2 > 60) ? 60 : s_retry_delay * 2;
        set_state(RETRY_BACKOFF, sd);
    }
}

// ── INBOUND_READY processing ───────────────────────────────────────────────────

static void do_inbound_ready(FsFileSystem* sd, const InputSnapshot* snap) {
    if (snap->game_running) {
        // Defer; will re-evaluate next tick when game closes.
        return;
    }
    // Outbound lock: if new outbound appeared, abort inbound.
    if (snap->has_outbound) {
        set_state(IDLE, sd);
        return;
    }
    set_state(DELIVERING, sd);
}

// ── DELIVERING processing ──────────────────────────────────────────────────────

static void do_delivering(FsFileSystem* sd, const InputSnapshot* snap) {
    (void)snap;

    char tid[17] = {0}, uid[17] = {0}, ts[20] = {0};
    sscanf(s_active_key, "%18[^-]-%16[^-]-%16s", ts, tid, uid);
    u64 tid_u = (u64)strtoull(tid, NULL, 16);
    char label[32];
    title_label(tid_u, label, sizeof(label));

    char inbound_dir[FS_MAX_PATH], inbound_json[FS_MAX_PATH];
    snprintf(inbound_dir,  sizeof(inbound_dir),  OMNI_ROOT "/inbound/%s",      s_active_key);
    snprintf(inbound_json, sizeof(inbound_json), OMNI_ROOT "/inbound/%s.json", s_active_key);

    // ── Pre-restore preservation (W3 invariant) ───────────────────────────────
    // Before overwriting local save, ensure it is in the cloud.
    // Invariant: no delivery may proceed until preservation upload completes
    // or is confirmed unnecessary (local save unchanged since last backup).
    {
        char pres_key[SNAP_KEY_LEN + 2] = {0};
        // Pass "" so open_save_fs accepts any local account's save — the source
        // uid belongs to a foreign device and may not exist on this one.
        int extract_rc = save_extract(tid_u, "", sd, pres_key, sizeof(pres_key));
        if (extract_rc == 1) {
            // Local save differs from last known cloud state — upload it first.
            int pres_snap_id = transport_upload(sd, pres_key, nullptr, true);
            char pres_dir[FS_MAX_PATH];
            snprintf(pres_dir, sizeof(pres_dir), OMNI_ROOT "/outbound/%s", pres_key);
            if (pres_snap_id > 0) {
                char pres_zip[FS_MAX_PATH];
                snprintf(pres_zip, sizeof(pres_zip),
                         OMNI_ROOT "/outbound/%s/save.zip", pres_key);
                snap_hash_update_from_archive(sd, pres_key, pres_zip);
                fsFsDeleteDirectoryRecursively(sd, pres_dir);
                fsFsCommit(sd);
                fs_log(sd, "PRESERVE_OK key=%s", pres_key);
            } else {
                // Upload failed — do NOT proceed with delivery.
                fsFsDeleteDirectoryRecursively(sd, pres_dir);
                fsFsCommit(sd);
                fs_log(sd, "PRESERVE_FAIL key=%s — delivery deferred", pres_key);
                return;
            }
        } else if (extract_rc == -1) {
            // Game save inaccessible (not installed or FS error) — cannot preserve.
            // Reuse inject-fail streak; after 3 attempts fail permanently for session.
            if (strcmp(tid, s_inject_fail_tid) != 0) {
                s_inject_fail_count = 0;
                snprintf(s_inject_fail_tid, sizeof(s_inject_fail_tid), "%s", tid);
            }
            s_inject_fail_count++;
            if (s_inject_fail_count < 3) {
                fs_log(sd, "PRESERVE_FAIL_EXTRACT title=%s attempt=%d/3", tid, s_inject_fail_count);
                set_state(IDLE, sd);
                return;
            }
            transport_error_fail(sd, s_active_key);
            fsFsDeleteDirectoryRecursively(sd, inbound_dir);
            fsFsDeleteFile(sd, inbound_json);
            fsFsCommit(sd);
            fsm_block_title(tid);
            s_inject_fail_tid[0] = '\0';
            s_inject_fail_count  = 0;
            {
                char pf_msg[96];
                snprintf(pf_msg, sizeof(pf_msg), "%s Restore Failed", label);
                notif_push("inject-fail", pf_msg);
                state_append_event(sd, "error", pf_msg);
            }
            fs_log(sd, "PRESERVE_FAIL_PERMANENT key=%s (game not installed?)", s_active_key);
            s_active_key[0]  = '\0';
            s_active_snap_id = 0;
            s_heartbeat_ticks = 1;
            set_state(IDLE, sd);
            return;
        }
        // extract_rc == 0: save unchanged since last backup — preservation not needed.
    }

    {
        char inj_msg[96];
        snprintf(inj_msg, sizeof(inj_msg), "Restoring %s...", label);
        notif_push("inject-start", inj_msg);
    }
    int inject_rc = save_inject(s_active_key, sd, uid);

    if (inject_rc == 1) {
        transport_ack(sd, s_active_key);
        int restore_counter = 0;
        {
            char old_base[SNAP_KEY_LEN + 2] = {0};
            state_read_lineage(sd, tid, uid, old_base, sizeof(old_base), &restore_counter);
            state_write_lineage(sd, tid, uid, s_active_key, restore_counter + 1);
        }
        fsFsDeleteDirectoryRecursively(sd, inbound_dir);
        fsFsDeleteFile(sd, inbound_json);
        fsFsCommit(sd);

        s_inject_fail_tid[0] = '\0';
        s_inject_fail_count  = 0;

        char msg[96];
        snprintf(msg, sizeof(msg), "%s Restored", label);
        notif_push("inject-ok", msg);
        state_append_event(sd, "info", msg);
        if (s_active_snap_id > 0) {
            state_write_last_restore(sd, tid, s_active_key, s_active_snap_id);
        } else {
            fs_log(sd, "RESTORE_SKIPPED_NO_SEQUENCE key=%s", s_active_key);
        }
        fs_log(sd, "INJECT_OK key=%s snap=%d", s_active_key, s_active_snap_id);
    } else if (inject_rc == -1) {
        // Permanent: game not installed on this device. Mark FAILED immediately — no retries.
        // Block the title for this session so future downloads are skipped until boot/wake.
        transport_error_fail(sd, s_active_key);
        fsFsDeleteDirectoryRecursively(sd, inbound_dir);
        fsFsDeleteFile(sd, inbound_json);
        fsFsCommit(sd);
        fsm_block_title(tid);
        char msg[96];
        snprintf(msg, sizeof(msg), "%s Restore Failed", label);
        notif_push("inject-fail", msg);
        state_append_event(sd, "error", msg);
        fs_log(sd, "INJECT_FAIL_PERMANENT key=%s (create failed)", s_active_key);
    } else {
        // Transient: track per-title streak; retry up to 3 times before giving up.
        if (strcmp(tid, s_inject_fail_tid) != 0) {
            s_inject_fail_count = 0;
            snprintf(s_inject_fail_tid, sizeof(s_inject_fail_tid), "%s", tid);
        }
        s_inject_fail_count++;

        if (s_inject_fail_count >= 3) {
            transport_error_fail(sd, s_active_key);
            fsFsDeleteDirectoryRecursively(sd, inbound_dir);
            fsFsDeleteFile(sd, inbound_json);
            fsFsCommit(sd);
            char msg[96];
            snprintf(msg, sizeof(msg), "%s Restore Failed", label);
            notif_push("inject-fail", msg);
            state_append_event(sd, "error", msg);
            fs_log(sd, "INJECT_FAIL_PERMANENT key=%s attempt=%d", s_active_key, s_inject_fail_count);
        } else {
            // Transient failure: retry locally next tick. Server is not notified —
            // inbound artifact is preserved for the fast-path reuse in transport_poll.
            fsFsCommit(sd);
            fs_log(sd, "INJECT_FAIL_RETRY key=%s attempt=%d/3", s_active_key, s_inject_fail_count);
        }
    }

    s_active_key[0]  = '\0';
    s_active_snap_id = 0;
    s_heartbeat_ticks    = 1;
    s_poll_hot_remain    = (s_poll_hot_remain > 15) ? s_poll_hot_remain : 15;
    s_last_activity_posix = get_posix_utc();
    set_state(IDLE, sd);
}

// ── RETRY_BACKOFF processing ───────────────────────────────────────────────────

static void do_retry_backoff(FsFileSystem* sd, const InputSnapshot* snap) {
    (void)snap;
    if (--s_retry_ticks <= 0) {
        set_state(IDLE, sd);
    }
}

// ── Public API ─────────────────────────────────────────────────────────────────

void fsm_init(void) {
    s_state               = IDLE;
    s_active_key[0]       = '\0';
    s_active_snap_id      = 0;
    s_retry_delay         = 5;
    s_retry_ticks         = 0;
    s_recent_remain       = 0;
    s_server_notified      = false;
    s_unpaired_notif_shown = false;
    s_upload_notif_key[0]  = '\0';
    s_inject_fail_tid[0]  = '\0';
    s_inject_fail_count   = 0;
    s_inject_blocked_count = 0;
    clear_scheduler();
}

void fsm_on_wake(void) {
    s_server_notified      = false;
    s_unpaired_notif_shown = false;
    s_inject_fail_tid[0]   = '\0';
    s_inject_fail_count    = 0;
    s_inject_blocked_count = 0;
}

void fsm_tick(FsFileSystem* sd, const InputSnapshot* snap) {
    switch (s_state) {
        case IDLE:          do_idle(sd, snap);          break;
        case UPLOADING:     do_uploading(sd, snap);     break;
        case DOWNLOADING:   do_downloading(sd, snap);   break;
        case INBOUND_READY: do_inbound_ready(sd, snap); break;
        case DELIVERING:    do_delivering(sd, snap);    break;
        case RETRY_BACKOFF: do_retry_backoff(sd, snap); break;
    }
}

FsmState fsm_get_state(void) { return s_state; }
