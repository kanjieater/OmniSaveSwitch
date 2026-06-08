#include <string.h>
#include <stdio.h>
#include "omnisave.h"

#pragma GCC diagnostic ignored "-Wformat-truncation"

// ── Create full parent directory chain ────────────────────────────────────────

static void ensure_parents(FsFileSystem* fs, const char* dest) {
    char tmp[FS_MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s", dest);
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; fsFsCreateDirectory(fs, tmp); *p = '/'; }
    }
}

// ── Public: unpack_save ───────────────────────────────────────────────────────

bool unpack_save(FsFileSystem* sd, const char* packed_path,
                 FsFileSystem* save_fs, u64* save_data_size_out) {
    crc_init();
    FsFile in_f;
    if (R_FAILED(fsFsOpenFile(sd, packed_path, FsOpenMode_Read, &in_f))) {
        fs_log(sd, "UNPACK_OPEN_FAIL path=%s", packed_path);
        return false;
    }
    if (save_data_size_out) *save_data_size_out = 0;

    s64 off = 0;
    bool ok = true;

    while (ok) {
        uint8_t sig[4]; uint64_t nr = 0;
        if (R_FAILED(fsFileRead(&in_f, off, sig, 4, FsReadOption_None, &nr)) || nr != 4)
            { ok = false; break; }
        if (sig[0] == 'P' && sig[1] == 'K') {
            if ((sig[2]==0x01 && sig[3]==0x02) || (sig[2]==0x05 && sig[3]==0x06))
                break;  // Central Directory or EOCD — done
        }
        if (!(sig[0]=='P' && sig[1]=='K' && sig[2]==0x03 && sig[3]==0x04)) {
            fs_log(sd, "UNPACK_SIG_FAIL path=%s off=%lld sig=%02x%02x%02x%02x",
                   packed_path, (long long)off, sig[0], sig[1], sig[2], sig[3]);
            ok = false; break;
        }

        uint8_t hdr[30]; nr = 0;
        if (R_FAILED(fsFileRead(&in_f, off, hdr, 30, FsReadOption_None, &nr)) || nr != 30)
            { ok = false; break; }
        uint16_t method    = (uint16_t)(hdr[8]  | (hdr[9]  << 8));
        uint32_t exp_crc   = (uint32_t)(hdr[14] | (hdr[15]<<8) | (hdr[16]<<16) | (hdr[17]<<24));
        uint32_t fsize     = (uint32_t)(hdr[22] | (hdr[23]<<8) | (hdr[24]<<16) | (hdr[25]<<24));
        uint16_t fname_len = (uint16_t)(hdr[26] | (hdr[27] << 8));
        uint16_t extra_len = (uint16_t)(hdr[28] | (hdr[29] << 8));
        off += 30;

        if (method != 0) {
            fs_log(sd, "UNPACK_METHOD_FAIL method=%u off=%lld", method, (long long)off);
            ok = false; break;
        }
        if (fname_len == 0 || fname_len >= FS_MAX_PATH) { ok = false; break; }

        char relp[FS_MAX_PATH]; nr = 0;
        if (R_FAILED(fsFileRead(&in_f, off, relp, fname_len, FsReadOption_None, &nr))
                || nr != fname_len) { ok = false; break; }
        relp[fname_len] = '\0';
        off += fname_len + extra_len;

        if (relp[0] == '/' || relp[0] == '\0' || strstr(relp, "../")) {
            fs_log(sd, "UNPACK_TRAVERSAL path=%s", relp);
            ok = false; break;
        }

        char dest[FS_MAX_PATH];
        snprintf(dest, sizeof(dest), "/%s", relp);
        ensure_parents(save_fs, dest);
        fsFsDeleteFile(save_fs, dest);

        Result crc = fsFsCreateFile(save_fs, dest, (s64)fsize, 0);
        if (R_FAILED(crc)) {
            fs_log(sd, "UNPACK_CREATE_FAIL dest=%s sz=%u rc=%08x",
                   dest, fsize, (unsigned)crc);
            off += (s64)fsize;
            ok = false; break;
        }

        uint32_t running_crc = 0;
        if (fsize > 0) {
            FsFile dst_f;
            if (R_FAILED(fsFsOpenFile(save_fs, dest, FsOpenMode_Write, &dst_f))) {
                fs_log(sd, "UNPACK_DSTOPEN_FAIL dest=%s", dest);
                off += (s64)fsize;
                ok = false; break;
            }
            s64 rem = (s64)fsize, dst_off = 0, next_commit = 8 * 1024 * 1024;
            bool dst_open = true;
            while (rem > 0 && ok) {
                u64 chunk = sizeof(s_copy_buf);
                if ((s64)chunk > rem) chunk = (u64)rem;
                nr = 0;
                if (R_FAILED(fsFileRead(&in_f, off, s_copy_buf, chunk,
                        FsReadOption_None, &nr)) || nr == 0) {
                    fs_log(sd, "UNPACK_READ_FAIL off=%lld rem=%lld",
                           (long long)off, (long long)rem);
                    ok = false; break;
                }
                running_crc = crc_run(running_crc, s_copy_buf, (size_t)nr);
                if (R_FAILED(fsFileWrite(&dst_f, dst_off, s_copy_buf, nr,
                        FsWriteOption_None))) {
                    fs_log(sd, "UNPACK_WRITE_FAIL dest=%s dst_off=%lld",
                           dest, (long long)dst_off);
                    ok = false; break;
                }
                off     += (s64)nr;
                dst_off += (s64)nr;
                rem     -= (s64)nr;
                if (dst_off >= next_commit) {
                    fsFileClose(&dst_f); dst_open = false;
                    fsFsCommit(save_fs);
                    if (R_FAILED(fsFsOpenFile(save_fs, dest, FsOpenMode_Write, &dst_f))) {
                        fs_log(sd, "UNPACK_RECOMMIT_FAIL dest=%s dst_off=%lld",
                               dest, (long long)dst_off);
                        ok = false; break;
                    }
                    dst_open = true;
                    next_commit = dst_off + (8 * 1024 * 1024);
                }
            }
            if (dst_open) fsFileClose(&dst_f);
        }

        if (ok && running_crc != exp_crc) {
            fsFsDeleteFile(save_fs, dest);  // remove corrupt write before aborting
            fs_log(sd, "UNPACK_CRC_FAIL dest=%s expected=%08x got=%08x",
                   dest, exp_crc, running_crc);
            ok = false;
        }
        if (ok) fsFsCommit(save_fs);
    }

    // Read save_data_size from 8-byte EOCD comment (last 30 bytes of the ZIP).
    if (ok && save_data_size_out) {
        s64 file_size = 0;
        if (R_SUCCEEDED(fsFileGetSize(&in_f, &file_size)) && file_size >= 30) {
            uint8_t eocd[30]; uint64_t enr = 0;
            if (R_SUCCEEDED(fsFileRead(&in_f, file_size - 30, eocd, 30,
                                       FsReadOption_None, &enr))
                    && enr == 30
                    && eocd[0] == 'P' && eocd[1] == 'K'
                    && eocd[2] == 0x05 && eocd[3] == 0x06) {
                u64 sds = 0;
                for (int i = 0; i < 8; i++) sds |= (u64)eocd[22 + i] << (8 * i);
                *save_data_size_out = sds;
            }
        }
    }

    fsFileClose(&in_f);
    return ok;
}

// ── Public: zip_fingerprint ───────────────────────────────────────────────────
// Computes the same fingerprint as pack_save's fp_out by reading the Central
// Directory. Consistent across devices: same save state → same fingerprint.

bool zip_fingerprint(FsFileSystem* sd, const char* path, u32* fp_out) {
    crc_init();
    FsFile f;
    if (R_FAILED(fsFsOpenFile(sd, path, FsOpenMode_Read, &f))) return false;

    s64 file_size = 0;
    if (R_FAILED(fsFileGetSize(&f, &file_size)) || file_size < 30)
        { fsFileClose(&f); return false; }

    uint8_t eocd[30]; uint64_t nr = 0;
    if (R_FAILED(fsFileRead(&f, file_size - 30, eocd, 30, FsReadOption_None, &nr))
            || nr != 30 || eocd[0] != 'P' || eocd[1] != 'K'
            || eocd[2] != 0x05 || eocd[3] != 0x06)
        { fsFileClose(&f); return false; }

    s64 cd_off = (s64)(uint32_t)(eocd[16] | (eocd[17]<<8) | (eocd[18]<<16) | (eocd[19]<<24));
    uint32_t fp = 0;

    while (true) {
        uint8_t cdh[46]; nr = 0;
        if (R_FAILED(fsFileRead(&f, cd_off, cdh, 46, FsReadOption_None, &nr)) || nr != 46
                || cdh[0] != 'P' || cdh[1] != 'K' || cdh[2] != 0x01 || cdh[3] != 0x02)
            break;
        uint32_t file_crc = (uint32_t)(cdh[16] | (cdh[17]<<8) | (cdh[18]<<16) | (cdh[19]<<24));
        uint32_t file_sz  = (uint32_t)(cdh[20] | (cdh[21]<<8) | (cdh[22]<<16) | (cdh[23]<<24));
        uint16_t nlen     = (uint16_t)(cdh[28] | (cdh[29]<<8));
        uint16_t xlen     = (uint16_t)(cdh[30] | (cdh[31]<<8));
        uint16_t clen     = (uint16_t)(cdh[32] | (cdh[33]<<8));
        cd_off += 46;

        if (nlen > 0 && nlen < FS_MAX_PATH) {
            char name[FS_MAX_PATH]; nr = 0;
            if (R_FAILED(fsFileRead(&f, cd_off, name, nlen, FsReadOption_None, &nr))
                    || nr != nlen) break;
            fp = crc_run(fp, name, nlen);
        }
        fp = crc_run(fp, &file_sz,  sizeof(uint32_t));
        fp = crc_run(fp, &file_crc, sizeof(uint32_t));
        cd_off += nlen + xlen + clen;
    }

    fsFileClose(&f);
    *fp_out = fp;
    return true;
}
