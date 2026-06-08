#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "omnisave.h"

#define LOG_PATH   OMNI_ROOT "/events.log"
#define LOG_PATH_1 OMNI_ROOT "/events.log.1"
#define LOG_MAX    524288  // rotate at 512 KB (~7 days of history across both files)

static void _open_or_create(FsFileSystem* fs, const char* path, FsFile* out, s64* size_out) {
    *size_out = 0;
    if (R_SUCCEEDED(fsFsOpenFile(fs, path, FsOpenMode_Write | FsOpenMode_Append, out))) {
        fsFileGetSize(out, size_out);
        return;
    }
    fsFsCreateFile(fs, path, 0, 0);
    fsFsOpenFile(fs, path, FsOpenMode_Write | FsOpenMode_Append, out);
}

void fs_log(FsFileSystem* fs, const char* fmt, ...) {
    char line[256];
    char ts[32];
    u64 posix = get_posix_utc();
    format_local_time(ts, sizeof(ts), posix);

    int hdr = snprintf(line, sizeof(line), "[%s] ", ts);
    if (hdr < 0 || hdr >= (int)sizeof(line)) hdr = 0;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + hdr, sizeof(line) - (size_t)hdr, fmt, ap);
    va_end(ap);

    size_t len = strlen(line);
    if (len < sizeof(line) - 1) { line[len] = '\n'; line[len + 1] = '\0'; len++; }

    FsFile file;
    s64 file_size = 0;
    _open_or_create(fs, LOG_PATH, &file, &file_size);
    if (file_size > LOG_MAX) {
        // Rotate: events.log → events.log.1 (drop previous .1), start fresh
        fsFileClose(&file);
        fsFsDeleteFile(fs, LOG_PATH_1);
        fsFsRenameFile(fs, LOG_PATH, LOG_PATH_1);
        fsFsCreateFile(fs, LOG_PATH, 0, 0);
        if (R_FAILED(fsFsOpenFile(fs, LOG_PATH, FsOpenMode_Write | FsOpenMode_Append, &file))) return;
        file_size = 0;
    }
    fsFileWrite(&file, file_size, line, len, FsWriteOption_Flush);
    fsFileClose(&file);
}

void fs_write_text_file(FsFileSystem* fs, const char* path, const char* text) {
    const size_t len = strlen(text);
    FsFile file;
    fsFsDeleteFile(fs, path);
    if (R_FAILED(fsFsCreateFile(fs, path, len, 0))) return;
    if (R_FAILED(fsFsOpenFile(fs, path, FsOpenMode_Write, &file))) return;
    fsFileWrite(&file, 0, text, len, FsWriteOption_Flush);
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

void ensure_dir(FsFileSystem* fs, const char* path) {
    fsFsCreateDirectory(fs, path);
}

void path_join(char* out, size_t sz, const char* dir, const char* name) {
    size_t dlen = strlen(dir);
    if (dlen > 0 && dir[dlen - 1] == '/')
        snprintf(out, sz, "%s%s", dir, name);
    else
        snprintf(out, sz, "%s/%s", dir, name);
}

bool copy_file(FsFileSystem* src_fs, const char* src_path,
               FsFileSystem* dst_fs, const char* dst_path) {
    FsFile src;
    if (R_FAILED(fsFsOpenFile(src_fs, src_path, FsOpenMode_Read, &src)))
        return false;

    s64 size = 0;
    fsFileGetSize(&src, &size);
    fsFsDeleteFile(dst_fs, dst_path);
    if (R_FAILED(fsFsCreateFile(dst_fs, dst_path, size, 0))) {
        fsFileClose(&src); return false;
    }
    FsFile dst;
    if (R_FAILED(fsFsOpenFile(dst_fs, dst_path, FsOpenMode_Write, &dst))) {
        fsFileClose(&src); return false;
    }
    s64 offset = 0;
    bool ok = true;
    while (offset < size) {
        u64 chunk = sizeof(s_copy_buf);
        if ((s64)chunk > size - offset) chunk = (u64)(size - offset);
        u64 nread = 0;
        if (R_FAILED(fsFileRead(&src, offset, s_copy_buf, chunk,
                FsReadOption_None, &nread)) || nread == 0)
            { ok = false; break; }
        if (R_FAILED(fsFileWrite(&dst, offset, s_copy_buf, nread,
                FsWriteOption_None)))
            { ok = false; break; }
        offset += (s64)nread;
    }
    fsFileClose(&dst);
    fsFileClose(&src);
    return ok && (offset == size);
}

bool copy_dir(FsFileSystem* src_fs, const char* src_path,
              FsFileSystem* dst_fs, const char* dst_path) {
    FsDir dir;
    if (R_FAILED(fsFsOpenDirectory(src_fs, src_path,
            FsDirOpenMode_ReadFiles | FsDirOpenMode_ReadDirs, &dir)))
        return false;

    FsDirectoryEntry entries[ENTRY_BATCH];
    bool ok = true;
    while (ok) {
        s64 count = 0;
        if (R_FAILED(fsDirRead(&dir, &count, ENTRY_BATCH, entries)) || count == 0)
            break;
        for (s64 i = 0; i < count && ok; i++) {
            char sp[FS_MAX_PATH], dp[FS_MAX_PATH];
            path_join(sp, sizeof(sp), src_path, entries[i].name);
            path_join(dp, sizeof(dp), dst_path, entries[i].name);
            if (entries[i].type == FsDirEntryType_Dir) {
                fsFsCreateDirectory(dst_fs, dp);
                ok = copy_dir(src_fs, sp, dst_fs, dp);
            } else {
                ok = copy_file(src_fs, sp, dst_fs, dp);
            }
        }
    }
    fsDirClose(&dir);
    return ok;
}
