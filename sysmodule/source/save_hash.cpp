#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "omnisave.h"

#pragma GCC diagnostic ignored "-Wformat-truncation"

// ── Path helpers ──────────────────────────────────────────────────────────────

static void hash_path(char* out, size_t sz, u64 title_id, const char* uid_hex) {
    snprintf(out, sz, OMNI_ROOT "/state/hashes/%016llX-%s.fp",
             (unsigned long long)title_id, uid_hex);
}

static void sig_path(char* out, size_t sz, u64 title_id, const char* uid_hex) {
    snprintf(out, sz, OMNI_ROOT "/state/hashes/%016llX-%s.sig",
             (unsigned long long)title_id, uid_hex);
}

// ── CRC32 fingerprint (4-byte LE) ─────────────────────────────────────────────

bool snap_hash_read(FsFileSystem* sd, u64 title_id, const char* uid_hex, u32* out) {
    char p[FS_MAX_PATH]; hash_path(p, sizeof(p), title_id, uid_hex);
    FsFile f;
    if (R_FAILED(fsFsOpenFile(sd, p, FsOpenMode_Read, &f))) return false;
    uint8_t b[4]; uint64_t nr = 0;
    bool ok = R_SUCCEEDED(fsFileRead(&f, 0, b, 4, FsReadOption_None, &nr)) && nr == 4;
    fsFileClose(&f);
    if (ok) *out = (u32)(b[0] | (b[1]<<8) | (b[2]<<16) | (b[3]<<24));
    return ok;
}

void snap_hash_write(FsFileSystem* sd, u64 title_id, const char* uid_hex, u32 fp) {
    fsFsCreateDirectory(sd, OMNI_ROOT "/state");
    fsFsCreateDirectory(sd, OMNI_ROOT "/state/hashes");
    char p[FS_MAX_PATH]; hash_path(p, sizeof(p), title_id, uid_hex);
    fsFsDeleteFile(sd, p);
    if (R_FAILED(fsFsCreateFile(sd, p, 4, 0))) return;
    FsFile f;
    if (R_FAILED(fsFsOpenFile(sd, p, FsOpenMode_Write, &f))) return;
    uint8_t b[4] = {(uint8_t)fp,(uint8_t)(fp>>8),(uint8_t)(fp>>16),(uint8_t)(fp>>24)};
    fsFileWrite(&f, 0, b, 4, FsWriteOption_None);
    fsFileClose(&f);
    fsFsCommit(sd);
}

// ── Stat signature (20-byte LE): file_count(u32)|total_bytes(u64)|newest_mtime(u64) ──

bool sig_read(FsFileSystem* sd, u64 title_id, const char* uid_hex, SaveSignature* out) {
    char p[FS_MAX_PATH]; sig_path(p, sizeof(p), title_id, uid_hex);
    FsFile f;
    if (R_FAILED(fsFsOpenFile(sd, p, FsOpenMode_Read, &f))) return false;
    uint8_t b[20]; uint64_t nr = 0;
    bool ok = R_SUCCEEDED(fsFileRead(&f, 0, b, 20, FsReadOption_None, &nr)) && nr == 20;
    fsFileClose(&f);
    if (!ok) return false;
    out->file_count   = (u32)(b[0]|(u32)b[1]<<8|(u32)b[2]<<16|(u32)b[3]<<24);
    out->total_bytes  = 0;
    out->newest_mtime = 0;
    for (int i = 0; i < 8; i++) out->total_bytes  |= (u64)b[4+i]  << (8*i);
    for (int i = 0; i < 8; i++) out->newest_mtime |= (u64)b[12+i] << (8*i);
    return true;
}

void sig_write(FsFileSystem* sd, u64 title_id, const char* uid_hex, const SaveSignature* sig) {
    fsFsCreateDirectory(sd, OMNI_ROOT "/state");
    fsFsCreateDirectory(sd, OMNI_ROOT "/state/hashes");
    char p[FS_MAX_PATH]; sig_path(p, sizeof(p), title_id, uid_hex);
    fsFsDeleteFile(sd, p);
    if (R_FAILED(fsFsCreateFile(sd, p, 20, 0))) return;
    FsFile f;
    if (R_FAILED(fsFsOpenFile(sd, p, FsOpenMode_Write, &f))) return;
    uint8_t b[20];
    b[0]=(uint8_t)sig->file_count; b[1]=(uint8_t)(sig->file_count>>8);
    b[2]=(uint8_t)(sig->file_count>>16); b[3]=(uint8_t)(sig->file_count>>24);
    for (int i = 0; i < 8; i++) b[4+i]  = (uint8_t)(sig->total_bytes  >> (8*i));
    for (int i = 0; i < 8; i++) b[12+i] = (uint8_t)(sig->newest_mtime >> (8*i));
    fsFileWrite(&f, 0, b, 20, FsWriteOption_None);
    fsFileClose(&f);
    fsFsCommit(sd);
}

// ── snap_hash_update_from_archive ─────────────────────────────────────────────

void snap_hash_update_from_archive(FsFileSystem* sd, const char* key, const char* path) {
    char tid[17] = {0}, uid_hex[17] = {0}, ts[20] = {0};
    if (sscanf(key, "%18[^-]-%16[^-]-%16s", ts, tid, uid_hex) != 3) return;
    u64 title_id = (u64)strtoull(tid, NULL, 16);
    if (title_id == 0) return;
    u32 fp = 0;
    if (zip_fingerprint(sd, path, &fp))
        snap_hash_write(sd, title_id, uid_hex, fp);
}
