#include "fsm_deps.hpp"
#include "omnisave.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

// ── Mock state ─────────────────────────────────────────────────────────────────

int  g_mock_upload_rc            = 1;
int  g_mock_poll_inbound_calls   = 0;
int  g_mock_poll_inbound_rc      = 0;  // 0=success, -2=401 token revoked
int  g_mock_status_write_calls   = 0;
int  g_mock_inject_rc            = 1;
int  g_mock_ack_calls            = 0;
int  g_mock_error_fail_calls     = 0;
int  g_mock_network_ready        = 1;
u64  g_mock_posix_utc            = 1000000;
int  g_mock_lineage_counter_out  = 0;
int  g_mock_save_extract_rc      = 0;  // 0=dedup (no change), 1=new, -1=error

void mock_reset(void) {
    g_mock_upload_rc             = 1;
    g_mock_poll_inbound_calls    = 0;
    g_mock_poll_inbound_rc       = 0;
    g_mock_status_write_calls    = 0;
    g_mock_inject_rc             = 1;
    g_mock_ack_calls             = 0;
    g_mock_error_fail_calls      = 0;
    g_mock_network_ready         = 1;
    g_mock_posix_utc             = 1000000;
    g_mock_lineage_counter_out   = 0;
    g_mock_save_extract_rc       = 0;
}

// ── FSM external dependency stubs ─────────────────────────────────────────────
// These are C++ definitions matching the C++ declarations in omnisave.h.
// Do NOT wrap in extern "C" — omnisave.h does not use extern "C".

// transport
int  transport_upload(FsFileSystem*, const char*, int*, bool) { return g_mock_upload_rc; }
int  transport_poll_inbound(FsFileSystem*)                    { g_mock_poll_inbound_calls++; return g_mock_poll_inbound_rc; }
void transport_ack(FsFileSystem*, const char*)                { g_mock_ack_calls++; }
void transport_error_fail(FsFileSystem*, const char*)         { g_mock_error_fail_calls++; }

// save_ops
int  save_extract(u64, const char*, FsFileSystem*, char*, size_t) { return g_mock_save_extract_rc; }
int  save_inject(const char*, FsFileSystem*, const char*)         { return g_mock_inject_rc; }

// notif (silent in tests)
void notif_push(const char*, const char*)    {}
void notif_verbose(const char*, const char*) {}

// state
void state_write_status(FsFileSystem*, FsmState, const char*, s64, s64) {
    g_mock_status_write_calls++;
}
void state_write_last_backup(FsFileSystem*, const char*, const char*, int)  {}
void state_write_last_restore(FsFileSystem*, const char*, const char*, int) {}

// snap_hash_update_from_archive is provided by the real save_hash.cpp (in omni_core).
// The real implementation is safe: zip_fingerprint silently fails on missing ZIPs.

void state_append_event(FsFileSystem*, const char*, const char*) {}

void state_write_lineage(FsFileSystem*, const char*, const char*,
                         const char*, int) {}
bool state_read_lineage(FsFileSystem*, const char*, const char*,
                        char* out, size_t sz, int* counter_out) {
    if (out && sz > 0) out[0] = '\0';
    if (counter_out) *counter_out = g_mock_lineage_counter_out;
    return false;
}

// main.cpp helpers
u64  get_posix_utc(void)                        { return g_mock_posix_utc; }
bool wait_network_ready(void)                   { return g_mock_network_ready != 0; }
bool get_title_name(u64, char* out, size_t sz)  { if (sz) out[0] = '\0'; return false; }
void format_local_time(char* out, size_t, u64)  { if (out) out[0] = '\0'; }
void make_ts_folder(char* out, size_t, u64)     { if (out) out[0] = '\0'; }
void refresh_dns_servers(void)                  {}

// fs_helpers — fs_log is silent to avoid VFS log-file side-effects in tests.
// fs_write_text_file / fs_read_text_file / ensure_dir use VFS directly.
void fs_log(FsFileSystem*, const char*, ...) {}

void path_join(char* out, size_t sz, const char* dir, const char* name) {
    if (dir[0] == '/' && dir[1] == '\0')
        snprintf(out, sz, "/%s", name);
    else
        snprintf(out, sz, "%s/%s", dir, name);
}

void ensure_dir(FsFileSystem* fs, const char* path) {
    fsFsCreateDirectory(fs, path);
}

void fs_write_text_file(FsFileSystem* fs, const char* path, const char* text) {
    const size_t len = strlen(text);
    FsFile file;
    fsFsDeleteFile(fs, path);
    if (R_FAILED(fsFsCreateFile(fs, path, (s64)len, 0))) return;
    if (R_FAILED(fsFsOpenFile(fs, path, FsOpenMode_Write, &file))) return;
    fsFileWrite(&file, 0, text, (u64)len, FsWriteOption_None);
    fsFileClose(&file);
    fsFsCommit(fs);
}

bool fs_read_text_file(FsFileSystem* fs, const char* path, char* buf, size_t buf_sz) {
    FsFile file;
    if (R_FAILED(fsFsOpenFile(fs, path, FsOpenMode_Read, &file))) return false;
    s64 size = 0;
    fsFileGetSize(&file, &size);
    if (size <= 0 || (size_t)size >= buf_sz) { fsFileClose(&file); return false; }
    u64 nread = 0;
    Result rc = fsFileRead(&file, 0, buf, (u64)size, FsReadOption_None, &nread);
    fsFileClose(&file);
    if (R_FAILED(rc)) return false;
    buf[nread] = '\0';
    return true;
}
