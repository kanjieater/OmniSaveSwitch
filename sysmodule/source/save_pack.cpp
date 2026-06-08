#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "omnisave.h"

#pragma GCC diagnostic ignored "-Wformat-truncation"

// ── Bidirectional arena: ZipEntry array front, path strings back ──────────────

#define ARENA_SIZE (512 * 1024)

typedef struct {
    char* path;
    u16   path_len;
    u32   crc32;
    u32   file_size;
    u32   lhdr_off;
} ZipEntry;

static uint8_t   s_arena[ARENA_SIZE];
static ZipEntry* s_ents;
static int       s_ent_cnt;
static uint8_t*  s_path_cur;
static bool      s_arena_err;

static void arena_reset(void) {
    s_ents      = (ZipEntry*)s_arena;
    s_ent_cnt   = 0;
    s_path_cur  = s_arena + ARENA_SIZE;
    s_arena_err = false;
}

static void arena_add(const char* rp, u16 plen, u32 fsize) {
    uint8_t* path_start = s_path_cur - ((size_t)plen + 1u);
    if (s_ent_cnt >= 65535 || path_start < (uint8_t*)(s_ents + s_ent_cnt + 1))
        { s_arena_err = true; return; }
    memcpy(path_start, rp, (size_t)plen + 1u);
    s_path_cur = path_start;
    ZipEntry* e = &s_ents[s_ent_cnt++];
    e->path     = (char*)path_start;
    e->path_len = plen;
    e->crc32    = 0;
    e->file_size = fsize;
    e->lhdr_off  = 0;
}

// ── Write helpers ─────────────────────────────────────────────────────────────

static bool wU16(FsFile* f, s64* o, uint16_t v) {
    uint8_t b[2] = {(uint8_t)v, (uint8_t)(v>>8)};
    if (R_FAILED(fsFileWrite(f, *o, b, 2, FsWriteOption_None))) return false;
    *o += 2; return true;
}
static bool wU32(FsFile* f, s64* o, uint32_t v) {
    uint8_t b[4] = {(uint8_t)v,(uint8_t)(v>>8),(uint8_t)(v>>16),(uint8_t)(v>>24)};
    if (R_FAILED(fsFileWrite(f, *o, b, 4, FsWriteOption_None))) return false;
    *o += 4; return true;
}
static bool wU64(FsFile* f, s64* o, uint64_t v) {
    uint8_t b[8]; for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8*i));
    if (R_FAILED(fsFileWrite(f, *o, b, 8, FsWriteOption_None))) return false;
    *o += 8; return true;
}
static bool wBuf(FsFile* f, s64* o, const void* d, size_t n) {
    if (R_FAILED(fsFileWrite(f, *o, d, n, FsWriteOption_None))) return false;
    *o += (s64)n; return true;
}

// ── Flat enumeration ──────────────────────────────────────────────────────────

static int cmp_ent(const void* a, const void* b) {
    return strcmp(((const ZipEntry*)a)->path, ((const ZipEntry*)b)->path);
}

static void enum_dir(FsFileSystem* src, const char* adir, const char* rel) {
    if (s_arena_err) return;
    FsDir d;
    if (R_FAILED(fsFsOpenDirectory(src, adir,
            FsDirOpenMode_ReadFiles | FsDirOpenMode_ReadDirs, &d))) return;
    FsDirectoryEntry batch[ENTRY_BATCH];
    s64 cnt = 0;
    while (!s_arena_err) {
        if (R_FAILED(fsDirRead(&d, &cnt, ENTRY_BATCH, batch)) || cnt == 0) break;
        for (s64 i = 0; i < cnt && !s_arena_err; i++) {
            char abs[FS_MAX_PATH], rp[FS_MAX_PATH];
            path_join(abs, sizeof(abs), adir, batch[i].name);
            if (rel[0]) snprintf(rp, sizeof(rp), "%s/%s", rel, batch[i].name);
            else        snprintf(rp, sizeof(rp), "%s",    batch[i].name);
            if (batch[i].type == FsDirEntryType_Dir) {
                enum_dir(src, abs, rp);
            } else {
                if ((u64)batch[i].file_size > 0xFFFFFFFFu) { s_arena_err = true; break; }
                size_t plen = strlen(rp);
                if (plen == 0 || plen >= FS_MAX_PATH) continue;
                arena_add(rp, (u16)plen, (u32)batch[i].file_size);
            }
        }
    }
    fsDirClose(&d);
}

// ── ZIP record writers ────────────────────────────────────────────────────────

static bool write_lfh(FsFile* f, s64* o, const ZipEntry* e) {
    return wU32(f, o, 0x04034b50u)
        && wU16(f, o, 20) && wU16(f, o, 0) && wU16(f, o, 0)
        && wU16(f, o, 0)  && wU16(f, o, 0)
        && wU32(f, o, 0)               // CRC-32 placeholder; patched after stream
        && wU32(f, o, e->file_size) && wU32(f, o, e->file_size)
        && wU16(f, o, e->path_len)  && wU16(f, o, 0)
        && wBuf(f, o, e->path, e->path_len);
}

static bool write_cdh(FsFile* f, s64* o, const ZipEntry* e) {
    return wU32(f, o, 0x02014b50u)
        && wU16(f, o, 20) && wU16(f, o, 20) && wU16(f, o, 0) && wU16(f, o, 0)
        && wU16(f, o, 0)  && wU16(f, o, 0)
        && wU32(f, o, e->crc32)
        && wU32(f, o, e->file_size) && wU32(f, o, e->file_size)
        && wU16(f, o, e->path_len)
        && wU16(f, o, 0) && wU16(f, o, 0) && wU16(f, o, 0)
        && wU16(f, o, 0) && wU32(f, o, 0) && wU32(f, o, e->lhdr_off)
        && wBuf(f, o, e->path, e->path_len);
}

// ── Public: pack_save ─────────────────────────────────────────────────────────

bool pack_save(FsFileSystem* src_fs, const char* src_dir,
               FsFileSystem* dst_fs, const char* out_path,
               u64 save_data_size, u32* fp_out, int* out_file_count) {
    crc_init();
    arena_reset();
    enum_dir(src_fs, src_dir, "");
    if (s_arena_err) {
        fs_log(dst_fs, "PACK_LIMIT_EXCEEDED entries=%d path=%s", s_ent_cnt, out_path);
        return false;
    }
    qsort(s_ents, (size_t)s_ent_cnt, sizeof(ZipEntry), cmp_ent);

    fsFsDeleteFile(dst_fs, out_path);
    if (R_FAILED(fsFsCreateFile(dst_fs, out_path, 0, 0))) return false;

    FsFile out_f;
    if (R_FAILED(fsFsOpenFile(dst_fs, out_path,
            FsOpenMode_Write | FsOpenMode_Append, &out_f))) return false;

    s64 off = 0;
    bool ok = true;

    for (int i = 0; i < s_ent_cnt && ok; i++) {
        ZipEntry* e = &s_ents[i];
        if (off > (s64)0xFFFFFFFFu) { ok = false; break; }
        e->lhdr_off = (u32)off;
        if (!write_lfh(&out_f, &off, e)) { ok = false; break; }

        if (e->file_size > 0) {
            char abs[FS_MAX_PATH];
            snprintf(abs, sizeof(abs), "/%s", e->path);
            FsFile src_f;
            if (R_FAILED(fsFsOpenFile(src_fs, abs, FsOpenMode_Read, &src_f)))
                { ok = false; break; }
            uint32_t crc = 0;
            s64 rem = (s64)e->file_size, src_off = 0;
            while (rem > 0 && ok) {
                u64 chunk = sizeof(s_copy_buf);
                if ((s64)chunk > rem) chunk = (u64)rem;
                u64 nr = 0;
                if (R_FAILED(fsFileRead(&src_f, src_off, s_copy_buf, chunk,
                        FsReadOption_None, &nr)) || nr == 0) { ok = false; break; }
                crc = crc_run(crc, s_copy_buf, (size_t)nr);
                if (!wBuf(&out_f, &off, s_copy_buf, nr)) { ok = false; break; }
                src_off += (s64)nr;
                rem     -= (s64)nr;
            }
            fsFileClose(&src_f);
            if (ok) {
                uint8_t cb[4] = {(uint8_t)crc,(uint8_t)(crc>>8),
                                 (uint8_t)(crc>>16),(uint8_t)(crc>>24)};
                if (R_FAILED(fsFileWrite(&out_f, (s64)e->lhdr_off + 14, cb, 4,
                        FsWriteOption_None))) ok = false;
                else e->crc32 = crc;
            }
        }
    }

    u32 cd_off = (u32)off;
    for (int i = 0; i < s_ent_cnt && ok; i++)
        if (!write_cdh(&out_f, &off, &s_ents[i])) { ok = false; break; }
    u32 cd_sz = (u32)(off - cd_off);

    if (ok) {
        ok = wU32(&out_f, &off, 0x06054b50u)
          && wU16(&out_f, &off, 0) && wU16(&out_f, &off, 0)
          && wU16(&out_f, &off, (u16)s_ent_cnt)
          && wU16(&out_f, &off, (u16)s_ent_cnt)
          && wU32(&out_f, &off, cd_sz)
          && wU32(&out_f, &off, cd_off)
          && wU16(&out_f, &off, 8)
          && wU64(&out_f, &off, save_data_size);
    }

    // Fingerprint: CRC32 over sorted (path + file_size + file_crc32) metadata.
    if (ok && fp_out) {
        uint32_t fp = 0;
        for (int i = 0; i < s_ent_cnt; i++) {
            fp = crc_run(fp, s_ents[i].path, s_ents[i].path_len);
            fp = crc_run(fp, &s_ents[i].file_size, sizeof(u32));
            fp = crc_run(fp, &s_ents[i].crc32,     sizeof(u32));
        }
        *fp_out = fp;
    }

    fsFileClose(&out_f);
    if (!ok) fsFsDeleteFile(dst_fs, out_path);
    if (out_file_count) *out_file_count = s_ent_cnt;
    return ok;
}
