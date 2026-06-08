#include "vfs.hpp"
#include "switch.h"
#include "omnisave.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

// ── Time stubs ─────────────────────────────────────────────────────────────────

Result timeToCalendarTimeWithMyRule(u64, TimeCalendarTime* out, void*) {
    if (out) {
        out->year = 2024; out->month = 1; out->day = 1;
        out->hour = 12;   out->minute = 0; out->second = 0;
    }
    return 0;
}

// ── main.cpp helpers ───────────────────────────────────────────────────────────

u64  get_posix_utc(void)                       { return 1704067200ULL; }
void format_local_time(char* out, size_t sz, u64) { snprintf(out, sz, "2024-01-01 00:00:00"); }
void make_ts_folder(char* out, size_t sz, u64)    { snprintf(out, sz, "20240101_000000"); }
bool get_title_name(u64, char* out, size_t sz)    { if (sz) out[0] = '\0'; return false; }
void title_label(u64, char* out, size_t sz)       { if (sz) out[0] = '\0'; }
void refresh_dns_servers(void)                    {}
bool wait_network_ready(void)                     { return false; }

// ── fs_helpers.cpp ─────────────────────────────────────────────────────────────

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

bool copy_file(FsFileSystem*, const char*, FsFileSystem*, const char*) { return false; }
bool copy_dir(FsFileSystem*, const char*, FsFileSystem*, const char*)  { return false; }

// ── notif.cpp ──────────────────────────────────────────────────────────────────

void notif_push(const char*, const char*)    {}
void notif_verbose(const char*, const char*) {}

// ── snap_hash stubs (not needed by state.cpp, but recovery.cpp is co-linked) ──

bool snap_hash_read(FsFileSystem*, u64, const char*, u32* out) { if (out) *out = 0; return false; }
void snap_hash_write(FsFileSystem*, u64, const char*, u32)     {}
void snap_hash_update_from_archive(FsFileSystem*, const char*, const char*) {}
bool sig_read(FsFileSystem*, u64, const char*, SaveSignature* out) {
    if (out) *out = {0, 0, 0}; return false;
}
void sig_write(FsFileSystem*, u64, const char*, const SaveSignature*) {}
