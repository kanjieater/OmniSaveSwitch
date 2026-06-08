#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "omnisave.h"
#include "save_clear.h"

#pragma GCC diagnostic ignored "-Wformat-truncation"

// Monotonic per-process counter embedded in every outbound key to prevent
// same-second collisions when game-close extraction and delivery preservation
// both run in the same tick for the same title+uid.
u32 s_key_seq = 0;

// ── open_save_fs ───────────────────────────────────────────────────────────────

static const FsSaveDataSpaceId SAVE_SPACES[] = {
    FsSaveDataSpaceId_User,
    FsSaveDataSpaceId_SdUser,
};

Result open_save_fs(u64 title_id, const char* uid_hex, FsFileSystem* out) {
    u64 target_uid0 = (uid_hex && uid_hex[0]) ?
                      (u64)strtoull(uid_hex, NULL, 16) : 0;
    Result rc = 0x202;
    for (int si = 0; si < 2 && R_FAILED(rc); si++) {
        FsSaveDataInfoReader reader;
        if (R_FAILED(fsOpenSaveDataInfoReader(&reader, SAVE_SPACES[si]))) continue;
        FsSaveDataInfo entries[ENTRY_BATCH];
        s64 count = 0;
        bool found = false;
        while (!found &&
               R_SUCCEEDED(fsSaveDataInfoReaderRead(&reader, entries, ENTRY_BATCH, &count)) &&
               count > 0) {
            for (s64 i = 0; i < count; i++) {
                if (entries[i].application_id != title_id) continue;
                if (entries[i].save_data_type  != FsSaveDataType_Account) continue;
                // If a specific UID is requested, skip non-matching accounts.
                if (target_uid0 != 0 && entries[i].uid.uid[0] != target_uid0) continue;
                FsSaveDataAttribute attr;
                memset(&attr, 0, sizeof(attr));
                attr.application_id = entries[i].application_id;
                attr.uid            = entries[i].uid;
                attr.save_data_type = entries[i].save_data_type;
                rc = fsOpenSaveDataFileSystem(out,
                    (FsSaveDataSpaceId)entries[i].save_data_space_id, &attr);
                if (R_SUCCEEDED(rc)) {
                    snprintf(s_uid_hex, sizeof(s_uid_hex), "%016llX",
                             (unsigned long long)entries[i].uid.uid[0]);
                    s_save_data_size = entries[i].size;
                    s_save_data_id   = entries[i].save_data_id;
                    s_save_space_id  = entries[i].save_data_space_id;
                    s_account_nickname[0] = '\0';
                    AccountProfile profile;
                    if (R_SUCCEEDED(accountGetProfile(&profile, entries[i].uid))) {
                        AccountProfileBase base;
                        if (R_SUCCEEDED(accountProfileGet(&profile, NULL, &base))) {
                            strncpy(s_account_nickname, base.nickname,
                                    sizeof(s_account_nickname) - 1);
                            s_account_nickname[sizeof(s_account_nickname) - 1] = '\0';
                            for (char* p = s_account_nickname; *p; p++)
                                if (*p == '"' || *p == '\\' || *p == '\n' ||
                                    *p == '\r' || *p == '\t') *p = ' ';
                        }
                        accountProfileClose(&profile);
                    }
                    found = true; break;
                }
            }
        }
        fsSaveDataInfoReaderClose(&reader);
    }
    return rc;
}

// ── save_enum_title_uids ────────────────────────────────────────────────────────

int save_enum_title_uids(u64 title_id, char uid_hexes[][17], int max_uids) {
    int count = 0;
    for (int si = 0; si < 2 && count < max_uids; si++) {
        FsSaveDataInfoReader reader;
        if (R_FAILED(fsOpenSaveDataInfoReader(&reader, SAVE_SPACES[si]))) continue;
        FsSaveDataInfo entries[ENTRY_BATCH];
        s64 n = 0;
        while (count < max_uids &&
               R_SUCCEEDED(fsSaveDataInfoReaderRead(&reader, entries, ENTRY_BATCH, &n)) &&
               n > 0) {
            for (s64 i = 0; i < n && count < max_uids; i++) {
                if (entries[i].application_id != title_id) continue;
                if (entries[i].save_data_type  != FsSaveDataType_Account) continue;
                snprintf(uid_hexes[count], 17, "%016llX",
                         (unsigned long long)entries[i].uid.uid[0]);
                count++;
            }
        }
        fsSaveDataInfoReaderClose(&reader);
    }
    return count;
}

// ── stat_save_dir ─────────────────────────────────────────────────────────────
// Recursively walks the save filesystem collecting file count, total bytes,
// and newest mtime. Used as a cheap preflight before committing to pack_save.

static void stat_save_dir(FsFileSystem* fs, const char* dir_path,
                          SaveSignature* sig, int depth) {
    if (depth > 8) return;
    FsDir dir;
    if (R_FAILED(fsFsOpenDirectory(fs, dir_path,
            FsDirOpenMode_ReadFiles | FsDirOpenMode_ReadDirs, &dir)))
        return;
    FsDirectoryEntry ents[ENTRY_BATCH];
    s64 count = 0;
    while (R_SUCCEEDED(fsDirRead(&dir, &count, ENTRY_BATCH, ents)) && count > 0) {
        for (s64 i = 0; i < count; i++) {
            char child[FS_MAX_PATH];
            if (dir_path[1] == '\0')
                snprintf(child, sizeof(child), "/%s", ents[i].name);
            else
                snprintf(child, sizeof(child), "%s/%s", dir_path, ents[i].name);
            if (ents[i].type == FsDirEntryType_File) {
                sig->file_count++;
                sig->total_bytes += (u64)ents[i].file_size;
                FsTimeStampRaw ts;
                if (R_SUCCEEDED(fsFsGetFileTimeStampRaw(fs, child, &ts)) && ts.is_valid) {
                    if (ts.modified > sig->newest_mtime)
                        sig->newest_mtime = ts.modified;
                }
            } else {
                stat_save_dir(fs, child, sig, depth + 1);
            }
        }
    }
    fsDirClose(&dir);
}

// ── save_extract ───────────────────────────────────────────────────────────────

// Returns: 1=extracted (new outbound staged), 0=skip (save unchanged or no save slot), -1=failure
int save_extract(u64 title_id, const char* uid_hex, FsFileSystem* sd,
                 char* out_key, size_t key_sz) {
    FsFileSystem save_fs;
    Result open_rc = open_save_fs(title_id, uid_hex, &save_fs);
    if (R_FAILED(open_rc)) {
        if (open_rc == 0x202) {
            // No save slot exists — game never launched on this device.
            // Nothing to preserve; preservation is vacuously satisfied.
            fs_log(sd, "EXTRACT_SKIP_NO_SAVE title=%016llX uid=%s",
                   (unsigned long long)title_id, uid_hex ? uid_hex : "");
            return 0;
        }
        return -1;
    }

    // Phase 1: stat pass — compare file count, total bytes, newest mtime.
    // Only short-circuits if a fingerprint file also exists, confirming a prior
    // successful upload. Without has_fp the stat signature alone cannot prove
    // the content is unchanged (e.g. lost outbound, same-size in-place edit).
    //
    // stored_fp is read once here and reused in Phase 3. This is safe because
    // save_extract() runs synchronously within a single FSM tick (single-threaded
    // tick loop). snap_hash_write() is only called from do_uploading() and the
    // delivery preservation path — both in different FSM states that cannot
    // execute concurrently with this function. If the file is partially written
    // (< 4 bytes), snap_hash_read() returns false and has_fp = false, which
    // causes both Phase 1 and Phase 3 to skip the FP comparison safely.
    SaveSignature cur_sig = {0, 0, 0};
    stat_save_dir(&save_fs, "/", &cur_sig, 0);
    u32 stored_fp = 0;
    bool has_fp = snap_hash_read(sd, title_id, s_uid_hex, &stored_fp);
    {
        SaveSignature stored_sig;
        if (has_fp && sig_read(sd, title_id, s_uid_hex, &stored_sig)) {
            bool mtime_ok = (stored_sig.newest_mtime != 0) && (cur_sig.newest_mtime != 0) &&
                            (stored_sig.newest_mtime == cur_sig.newest_mtime);
            if (stored_sig.file_count == cur_sig.file_count &&
                stored_sig.total_bytes == cur_sig.total_bytes && mtime_ok) {
                fsFsClose(&save_fs);
                fs_log(sd, "EXTRACT_DEDUP_STAT title=%016llX fc=%u tb=%llu",
                       (unsigned long long)title_id, cur_sig.file_count,
                       (unsigned long long)cur_sig.total_bytes);
                return 0;
            }
        }
    }

    // Phase 2: signature changed — pack the save.
    u64 posix = get_posix_utc();
    char ts_folder[32];
    make_ts_folder(ts_folder, sizeof(ts_folder), posix);
    snprintf(out_key, key_sz, "%.15s_%02u-%016llX-%.16s",
             ts_folder, s_key_seq++ % 100,
             (unsigned long long)title_id, s_uid_hex);

    char tmp_path[FS_MAX_PATH], tmp_packed[FS_MAX_PATH], out_path[FS_MAX_PATH];
    snprintf(tmp_path,   sizeof(tmp_path),   OMNI_ROOT "/tmp_out/%s",          out_key);
    snprintf(tmp_packed, sizeof(tmp_packed), OMNI_ROOT "/tmp_out/%s/save.zip", out_key);
    snprintf(out_path,   sizeof(out_path),   OMNI_ROOT "/outbound/%s",         out_key);

    fsFsDeleteDirectoryRecursively(sd, tmp_path);
    fsFsCreateDirectory(sd, tmp_path);

    // Marker: recovery sweep skips tmp_out while extraction is in progress.
    fsFsCreateFile(sd, OMNI_ROOT "/tmp_out/.active", 0, 0);
    fsFsCommit(sd);

    int file_count = 0;
    u32 fp = 0;
    bool ok = pack_save(&save_fs, "/", sd, tmp_packed, s_save_data_size, &fp, &file_count);
    fsFsClose(&save_fs);

    if (!ok) {
        fsFsDeleteFile(sd, OMNI_ROOT "/tmp_out/.active");
        fsFsDeleteDirectoryRecursively(sd, tmp_path);
        return -1;
    }

    // Zero-entry ZIP: title is installed but this account has no save data yet.
    if (file_count == 0) {
        fs_log(sd, "EXTRACT_SKIP_EMPTY key=%s", out_key);
        fsFsDeleteFile(sd, OMNI_ROOT "/tmp_out/.active");
        fsFsDeleteDirectoryRecursively(sd, tmp_path);
        fsFsCommit(sd);
        sig_write(sd, title_id, s_uid_hex, &cur_sig);
        return 0;
    }

    // Secondary dedup: CRC32 confirms content unchanged despite stat difference.
    // stored_fp and has_fp were read before Phase 1 above; no second read needed.
    if (has_fp && stored_fp == fp) {
        fsFsDeleteFile(sd, OMNI_ROOT "/tmp_out/.active");
        fsFsDeleteDirectoryRecursively(sd, tmp_path);
        fsFsCommit(sd);
        sig_write(sd, title_id, s_uid_hex, &cur_sig);
        fs_log(sd, "EXTRACT_DEDUP_FP title=%016llX fp=%08x", (unsigned long long)title_id, fp);
        return 0;
    }

    fsFsCommit(sd);
    Result rc = fsFsRenameDirectory(sd, tmp_path, out_path);
    if (R_FAILED(rc)) {
        fsFsDeleteFile(sd, OMNI_ROOT "/tmp_out/.active");
        fsFsDeleteDirectoryRecursively(sd, tmp_path);
        return -1;
    }
    fsFsDeleteFile(sd, OMNI_ROOT "/tmp_out/.active");
    fsFsCommit(sd);
    sig_write(sd, title_id, s_uid_hex, &cur_sig);
    fs_log(sd, "EXTRACT_OK key=%s", out_key);
    return 1;
}

// ── clear_save_root ────────────────────────────────────────────────────────────

// clear_save_root is in save_clear.cpp (extracted for testability).

// ── peek_save_data_size ────────────────────────────────────────────────────────

static u64 peek_save_data_size(FsFileSystem* sd, const char* path) {
    FsFile f;
    if (R_FAILED(fsFsOpenFile(sd, path, FsOpenMode_Read, &f))) return 0;
    s64 file_size = 0;
    if (R_FAILED(fsFileGetSize(&f, &file_size)) || file_size < 30)
        { fsFileClose(&f); return 0; }
    u8 buf[30]; u64 nr = 0;
    if (R_FAILED(fsFileRead(&f, file_size - 30, buf, 30, FsReadOption_None, &nr)) || nr != 30)
        { fsFileClose(&f); return 0; }
    fsFileClose(&f);
    if (buf[0] != 'P' || buf[1] != 'K' || buf[2] != 0x05 || buf[3] != 0x06) return 0;
    if ((u16)(buf[20] | (buf[21] << 8)) != 8) return 0;
    u64 sds = 0;
    for (int i = 0; i < 8; i++) sds |= ((u64)buf[22 + i] << (8 * i));
    return sds;
}

// ── save_inject ────────────────────────────────────────────────────────────────

int save_inject(const char* key, FsFileSystem* sd, const char* target_user_key) {
    char packed_path[FS_MAX_PATH];
    snprintf(packed_path, sizeof(packed_path), OMNI_ROOT "/inbound/%s/save.zip", key);

    char tid[17] = {0}, uid_hex[17] = {0}, ts[20] = {0};
    if (sscanf(key, "%18[^-]-%16[^-]-%16s", ts, tid, uid_hex) != 3) return -1;
    u64 title_id = (u64)strtoull(tid, NULL, 16);
    if (title_id == 0) return -1;

    // Resolve the target account UID. Priority:
    // 1. Exact match of target_user_key against local accounts.
    // 2. accountGetLastOpenedUser() — fallback, logged as USER_CONTEXT_FALLBACK.
    // 3. First available account from accountListAllUsers().
    AccountUid inject_uid;
    memset(&inject_uid, 0, sizeof(inject_uid));
    bool using_fallback = true;

    if (target_user_key && target_user_key[0]) {
        u64 target_uid0 = (u64)strtoull(target_user_key, NULL, 16);
        AccountUid all_uids[8];
        s32 all_count = 0;
        if (R_SUCCEEDED(accountListAllUsers(all_uids, 8, &all_count))) {
            for (s32 i = 0; i < all_count; i++) {
                if (all_uids[i].uid[0] == target_uid0) {
                    inject_uid = all_uids[i];
                    using_fallback = false;
                    break;
                }
            }
        }
        if (using_fallback)
            fs_log(sd, "USER_CONTEXT_FALLBACK key=%s target=%s used=last_known_user",
                   key, target_user_key);
    }

    if (using_fallback) {
        if (R_FAILED(accountGetLastOpenedUser(&inject_uid))) {
            s32 user_count = 0;
            if (R_FAILED(accountListAllUsers(&inject_uid, 1, &user_count))
                    || user_count == 0)
                return -1;
        }
    }

    // Enumerate save data for this title across User and SdUser to determine the
    // correct space. Priority: exact uid match > any account match > NAND user default.
    // FsSaveDataSpaceId_All does not reliably enumerate all spaces (JKSV source).
    FsSaveDataSpaceId inject_space = FsSaveDataSpaceId_User;  // NAND user — safe default
    bool existing_save = false;
    {
        bool space_inferred = false;
        for (int si = 0; si < 2 && !existing_save; si++) {
            FsSaveDataInfoReader rdr;
            if (R_FAILED(fsOpenSaveDataInfoReader(&rdr, SAVE_SPACES[si]))) continue;
            FsSaveDataInfo batch[ENTRY_BATCH];
            s64 n = 0;
            while (!existing_save &&
                   R_SUCCEEDED(fsSaveDataInfoReaderRead(&rdr, batch, ENTRY_BATCH, &n)) &&
                   n > 0) {
                for (s64 i = 0; i < n; i++) {
                    if (batch[i].application_id != title_id) continue;
                    if (batch[i].save_data_type  != FsSaveDataType_Account) continue;
                    if (!space_inferred) {
                        // Any account's save for this title tells us the correct space.
                        inject_space = (FsSaveDataSpaceId)batch[i].save_data_space_id;
                        space_inferred = true;
                    }
                    if (batch[i].uid.uid[0] == inject_uid.uid[0]) {
                        inject_space = (FsSaveDataSpaceId)batch[i].save_data_space_id;
                        existing_save = true;
                        break;
                    }
                }
            }
            fsSaveDataInfoReaderClose(&rdr);
        }
    }

    FsFileSystem save_fs;
    FsSaveDataAttribute attr_open;
    memset(&attr_open, 0, sizeof(attr_open));
    attr_open.application_id = title_id;
    attr_open.uid            = inject_uid;
    attr_open.save_data_type = FsSaveDataType_Account;

    if (existing_save) {
        Result rc = fsOpenSaveDataFileSystem(&save_fs, inject_space, &attr_open);
        if (R_FAILED(rc)) {
            fs_log(sd, "INJECT_OPEN_FAIL title=%s rc=%08x space=%d",
                   tid, (unsigned)rc, (int)inject_space);
            return 0;  // transient — space came from enumeration so should be valid
        }
    } else {
        // Enumeration confirmed no save exists — go straight to creation.
        u64 inbound_size = peek_save_data_size(sd, packed_path);
        if (inbound_size == 0) return -1;

        FsSaveDataAttribute attr; memset(&attr, 0, sizeof(attr));
        attr.application_id = title_id;
        attr.uid            = inject_uid;
        attr.save_data_type = FsSaveDataType_Account;

        FsSaveDataCreationInfo cinfo; memset(&cinfo, 0, sizeof(cinfo));
        cinfo.save_data_size     = (s64)inbound_size;
        cinfo.journal_size       = (s64)inbound_size;
        cinfo.available_size     = 0x4000;
        cinfo.owner_id           = title_id;
        cinfo.save_data_space_id = inject_space;

        FsSaveDataMetaInfo meta; memset(&meta, 0, sizeof(meta));
        Result crc = fsCreateSaveDataFileSystem(&attr, &cinfo, &meta);
        fs_log(sd, "SAVE_CREATE title=%s rc=%08x space=%d",
               tid, (unsigned)crc, (int)inject_space);
        if (R_FAILED(crc)) return -1;

        Result rc = fsOpenSaveDataFileSystem(&save_fs, inject_space, &attr_open);
        if (R_FAILED(rc)) return -1;

        // Populate globals so snap_hash_update_from_archive works correctly.
        s_save_data_size = inbound_size;
        s_save_data_id   = 0;
        s_save_space_id  = inject_space;
        snprintf(s_uid_hex, sizeof(s_uid_hex), "%016llX",
                 (unsigned long long)inject_uid.uid[0]);
    }

    u64 inbound_size = peek_save_data_size(sd, packed_path);
    if (inbound_size > s_save_data_size) {
        s64 delta = (s64)(inbound_size - s_save_data_size);
        Result ext_rc = fsExtendSaveDataFileSystem(
            (FsSaveDataSpaceId)s_save_space_id, s_save_data_id, delta, 0);
        fs_log(sd, "EXTEND_SAVE title=%s delta=%lld rc=%08x",
               tid, (long long)delta, (unsigned)ext_rc);
    }

    bool ok = clear_save_root(&save_fs);
    if (!ok) fs_log(sd, "INJECT_CLEAR_FAIL title=%s", tid);
    if (ok) ok = unpack_save(sd, packed_path, &save_fs, NULL);
    if (!ok) fs_log(sd, "INJECT_UNPACK_FAIL title=%s", tid);
    if (ok) {
        fsFsCommit(&save_fs);
        snap_hash_update_from_archive(sd, key, packed_path);
    }
    fsFsClose(&save_fs);
    return ok ? 1 : 0;
}
