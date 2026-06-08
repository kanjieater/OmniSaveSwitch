#include <string.h>
#include <stdio.h>
#include "omnisave.h"

#pragma GCC diagnostic ignored "-Wformat-truncation"

// Purge all entries in a directory, logging each one.
static void purge_dir(FsFileSystem* sd, const char* dir_path, const char* tag) {
    FsDir dir;
    if (R_FAILED(fsFsOpenDirectory(sd, dir_path,
            FsDirOpenMode_ReadFiles | FsDirOpenMode_ReadDirs, &dir)))
        return;

    FsDirectoryEntry ents[ENTRY_BATCH];
    s64 count = 0;
    while (R_SUCCEEDED(fsDirRead(&dir, &count, ENTRY_BATCH, ents)) && count > 0) {
        for (s64 i = 0; i < count; i++) {
            char full[FS_MAX_PATH];
            path_join(full, sizeof(full), dir_path, ents[i].name);
            fs_log(sd, "SWEEP_PURGE [%s] %s", tag, ents[i].name);
            if (ents[i].type == FsDirEntryType_Dir)
                fsFsDeleteDirectoryRecursively(sd, full);
            else
                fsFsDeleteFile(sd, full);
        }
    }
    fsDirClose(&dir);
}

// Validate that outbound entries are well-formed; discard corrupted ones.
static void validate_outbound(FsFileSystem* sd) {
    FsDir dir;
    if (R_FAILED(fsFsOpenDirectory(sd, OMNI_ROOT "/outbound",
            FsDirOpenMode_ReadDirs, &dir)))
        return;

    FsDirectoryEntry ents[ENTRY_BATCH];
    s64 count = 0;
    while (R_SUCCEEDED(fsDirRead(&dir, &count, ENTRY_BATCH, ents)) && count > 0) {
        for (s64 i = 0; i < count; i++) {
            char full[FS_MAX_PATH];
            path_join(full, sizeof(full), OMNI_ROOT "/outbound", ents[i].name);
            size_t nlen = strlen(ents[i].name);

            bool discard = (nlen != SNAP_KEY_LEN);

            // Outbound dirs must contain save.zip (ZIP packed by save_extract).
            if (!discard) {
                char packed[FS_MAX_PATH];
                snprintf(packed, sizeof(packed), "%s/save.zip", full);
                FsFile f;
                if (R_SUCCEEDED(fsFsOpenFile(sd, packed, FsOpenMode_Read, &f))) {
                    fsFileClose(&f);
                } else {
                    discard = true;
                }
            }

            if (discard) {
                fs_log(sd, "SWEEP_DISCARD outbound %s", ents[i].name);
                fsFsDeleteDirectoryRecursively(sd, full);
            }
        }
    }
    fsDirClose(&dir);
}

// Validate inbound entries; enforce outbound-wins rule.
static void validate_inbound(FsFileSystem* sd) {
    FsDir odir;
    bool has_outbound = false;
    if (R_SUCCEEDED(fsFsOpenDirectory(sd, OMNI_ROOT "/outbound",
            FsDirOpenMode_ReadDirs, &odir))) {
        FsDirectoryEntry ents[ENTRY_BATCH]; s64 count = 0;
        if (R_SUCCEEDED(fsDirRead(&odir, &count, ENTRY_BATCH, ents)) && count > 0)
            has_outbound = true;
        fsDirClose(&odir);
    }

    FsDir idir;
    if (R_FAILED(fsFsOpenDirectory(sd, OMNI_ROOT "/inbound",
            FsDirOpenMode_ReadDirs, &idir)))
        return;

    FsDirectoryEntry ents[ENTRY_BATCH];
    s64 count = 0;
    while (R_SUCCEEDED(fsDirRead(&idir, &count, ENTRY_BATCH, ents)) && count > 0) {
        for (s64 i = 0; i < count; i++) {
            char full[FS_MAX_PATH];
            path_join(full, sizeof(full), OMNI_ROOT "/inbound", ents[i].name);
            size_t nlen = strlen(ents[i].name);

            // Discard bad keys or any inbound when outbound exists (spec §2.4 crash rule).
            if (nlen != SNAP_KEY_LEN || has_outbound) {
                fs_log(sd, "SWEEP_DISCARD inbound %s (outbound_wins=%d)",
                       ents[i].name, (int)has_outbound);
                if (ents[i].type == FsDirEntryType_Dir)
                    fsFsDeleteDirectoryRecursively(sd, full);
                else
                    fsFsDeleteFile(sd, full);
            }
        }
    }
    fsDirClose(&idir);
}

// Keep only the latest outbound entry per (tid, uid) pair — older ones are
// superseded on the server regardless, so uploading them wastes bandwidth.
static void deduplicate_outbound(FsFileSystem* sd) {
    FsDir dir;
    if (R_FAILED(fsFsOpenDirectory(sd, OMNI_ROOT "/outbound",
            FsDirOpenMode_ReadDirs, &dir)))
        return;

    static char keys[64][SNAP_KEY_LEN + 2];
    int nkeys = 0;
    FsDirectoryEntry ents[ENTRY_BATCH];
    s64 count = 0;
    while (R_SUCCEEDED(fsDirRead(&dir, &count, ENTRY_BATCH, ents)) && count > 0) {
        for (s64 i = 0; i < count && nkeys < 64; i++) {
            if (strlen(ents[i].name) == SNAP_KEY_LEN)
                snprintf(keys[nkeys++], SNAP_KEY_LEN + 2, "%s", ents[i].name);
        }
    }
    fsDirClose(&dir);

    for (int i = 0; i < nkeys; i++) {
        char ts_i[20] = {0}, tid_i[17] = {0}, uid_i[17] = {0};
        if (sscanf(keys[i], "%18[^-]-%16[^-]-%16s", ts_i, tid_i, uid_i) != 3) continue;
        for (int j = 0; j < nkeys; j++) {
            if (i == j) continue;
            char ts_j[20] = {0}, tid_j[17] = {0}, uid_j[17] = {0};
            if (sscanf(keys[j], "%18[^-]-%16[^-]-%16s", ts_j, tid_j, uid_j) != 3) continue;
            if (strcmp(tid_i, tid_j) != 0 || strcmp(uid_i, uid_j) != 0) continue;
            if (strcmp(ts_j, ts_i) > 0) {
                char full[FS_MAX_PATH];
                path_join(full, sizeof(full), OMNI_ROOT "/outbound", keys[i]);
                fs_log(sd, "SWEEP_DEDUP evict %s (newer: %s)", keys[i], keys[j]);
                fsFsDeleteDirectoryRecursively(sd, full);
                break;
            }
        }
    }
}

// ── state/ orphan recovery (handles atomic_write crash remnants) ───────────────
//
// atomic_write uses a .tmp + .bak pattern. A power-loss can leave orphan files:
//   Pass 1 (.bak):  if target missing + .tmp present → rename .tmp→target, delete .bak
//                   if target missing + no .tmp       → rename .bak→target (restore)
//                   if target exists                  → delete .bak (stale)
//   Pass 2 (.tmp):  if target missing → rename .tmp→target (interrupted first write)
//                   if target exists  → delete .tmp (stale)

// Max orphan files tracked per directory (state/ usually has < 20 files).
#define MAX_ORPHANS 32

static void recover_state_orphans(FsFileSystem* sd) {
    const char* const scan_dirs[2] = {
        OMNI_ROOT "/state",
        OMNI_ROOT "/state/lineage",
    };

    for (int di = 0; di < 2; di++) {
        const char* sdir = scan_dirs[di];

        // Enumerate all files in this directory with one open/read/close.
        static char bak_names[MAX_ORPHANS][FS_MAX_PATH];
        static char tmp_names[MAX_ORPHANS][FS_MAX_PATH];
        int n_bak = 0, n_tmp = 0;

        {
            FsDir dir;
            if (R_FAILED(fsFsOpenDirectory(sd, sdir, FsDirOpenMode_ReadFiles, &dir)))
                continue;
            FsDirectoryEntry ents[ENTRY_BATCH]; s64 count = 0;
            while (R_SUCCEEDED(fsDirRead(&dir, &count, ENTRY_BATCH, ents)) && count > 0) {
                for (s64 i = 0; i < count; i++) {
                    size_t nlen = strlen(ents[i].name);
                    if (nlen > 4 && strcmp(ents[i].name + nlen - 4, ".bak") == 0
                            && n_bak < MAX_ORPHANS)
                        snprintf(bak_names[n_bak++], FS_MAX_PATH, "%s", ents[i].name);
                    else if (nlen > 4 && strcmp(ents[i].name + nlen - 4, ".tmp") == 0
                            && n_tmp < MAX_ORPHANS)
                        snprintf(tmp_names[n_tmp++], FS_MAX_PATH, "%s", ents[i].name);
                }
            }
            fsDirClose(&dir);
        }

        // Pass 1: resolve .bak files (may consume matching .tmp files too).
        for (int b = 0; b < n_bak; b++) {
            size_t nlen = strlen(bak_names[b]);
            char tname[FS_MAX_PATH];
            memcpy(tname, bak_names[b], nlen - 4);
            tname[nlen - 4] = '\0';

            char bak[FS_MAX_PATH], tgt[FS_MAX_PATH], tmp[FS_MAX_PATH];
            path_join(bak, sizeof(bak), sdir, bak_names[b]);
            path_join(tgt, sizeof(tgt), sdir, tname);
            snprintf(tmp, sizeof(tmp), "%s.tmp", tgt);

            FsFile tf;
            if (R_SUCCEEDED(fsFsOpenFile(sd, tgt, FsOpenMode_Read, &tf))) {
                fsFileClose(&tf);
                fsFsDeleteFile(sd, bak);
                fs_log(sd, "SWEEP_STATE_BAK_STALE %s", bak_names[b]);
            } else {
                FsFile tmpf;
                if (R_SUCCEEDED(fsFsOpenFile(sd, tmp, FsOpenMode_Read, &tmpf))) {
                    fsFileClose(&tmpf);
                    // Both .tmp and .bak present: .tmp is the newer write — prefer it.
                    fsFsRenameFile(sd, tmp, tgt);
                    fsFsDeleteFile(sd, bak);
                    fs_log(sd, "SWEEP_STATE_TMP_PROMOTE %s", tname);
                } else {
                    fsFsRenameFile(sd, bak, tgt);
                    fs_log(sd, "SWEEP_STATE_BAK_RESTORE %s", tname);
                }
            }
        }

        // Pass 2: resolve .tmp files not already handled by pass 1.
        for (int t = 0; t < n_tmp; t++) {
            size_t nlen = strlen(tmp_names[t]);
            char tname[FS_MAX_PATH];
            memcpy(tname, tmp_names[t], nlen - 4);
            tname[nlen - 4] = '\0';

            char tmp_path[FS_MAX_PATH], tgt[FS_MAX_PATH];
            path_join(tmp_path, sizeof(tmp_path), sdir, tmp_names[t]);
            path_join(tgt, sizeof(tgt), sdir, tname);

            FsFile tf;
            if (R_SUCCEEDED(fsFsOpenFile(sd, tgt, FsOpenMode_Read, &tf))) {
                fsFileClose(&tf);
                fsFsDeleteFile(sd, tmp_path);
                fs_log(sd, "SWEEP_STATE_TMP_STALE %s", tmp_names[t]);
            } else {
                fsFsRenameFile(sd, tmp_path, tgt);
                fs_log(sd, "SWEEP_STATE_TMP_RESTORE %s", tname);
            }
        }

        fsFsCommit(sd);
    }
}

// ── Public API ─────────────────────────────────────────────────────────────────

void recovery_sweep(FsFileSystem* sd, RecoverySweepMode mode) {
    fs_log(sd, "SWEEP_START");

    // Recover any atomic_write orphans in state/ before touching the queues.
    if (mode == SWEEP_BOOT_CLEAN_ALL)
        recover_state_orphans(sd);

    // tmp_out: always purge on boot. On wake, check .active marker — if present,
    // save_extract is mid-pack; leave tmp_out alone and let it finish.
    bool skip_tmp_purge = false;
    if (mode == SWEEP_WAKE_SAFE_ONLY) {
        FsFile mf;
        if (R_SUCCEEDED(fsFsOpenFile(sd, OMNI_ROOT "/tmp_out/.active",
                                     FsOpenMode_Read, &mf))) {
            fsFileClose(&mf);
            skip_tmp_purge = true;
        }
    }
    if (!skip_tmp_purge)
        purge_dir(sd, OMNI_ROOT "/tmp_out", "tmp_out");

    // tmp_in: keep dirs that have committed download progress (save.zip size > 0).
    // Purge invalid entries and dirs with no committed data (stale or zero-progress).
    {
        FsDir tdir;
        if (R_SUCCEEDED(fsFsOpenDirectory(sd, OMNI_ROOT "/tmp_in",
                FsDirOpenMode_ReadFiles | FsDirOpenMode_ReadDirs, &tdir))) {
            FsDirectoryEntry ents[ENTRY_BATCH];
            s64 count = 0;
            while (R_SUCCEEDED(fsDirRead(&tdir, &count, ENTRY_BATCH, ents)) && count > 0) {
                for (s64 i = 0; i < count; i++) {
                    size_t nlen = strlen(ents[i].name);
                    char full[FS_MAX_PATH];
                    path_join(full, sizeof(full), OMNI_ROOT "/tmp_in", ents[i].name);

                    if (ents[i].type != FsDirEntryType_Dir || nlen != SNAP_KEY_LEN) {
                        fs_log(sd, "SWEEP_PURGE [tmp_in_invalid] %s", ents[i].name);
                        if (ents[i].type == FsDirEntryType_Dir)
                            fsFsDeleteDirectoryRecursively(sd, full);
                        else
                            fsFsDeleteFile(sd, full);
                        continue;
                    }

                    char zip_path[FS_MAX_PATH];
                    snprintf(zip_path, sizeof(zip_path), "%s/save.zip", full);
                    s64 zip_sz = 0;
                    FsFile zf;
                    if (R_SUCCEEDED(fsFsOpenFile(sd, zip_path, FsOpenMode_Read, &zf))) {
                        fsFileGetSize(&zf, &zip_sz);
                        fsFileClose(&zf);
                    }

                    if (zip_sz > 0) {
                        fs_log(sd, "SWEEP_KEEP [tmp_in] %s sz=%lld",
                               ents[i].name, (long long)zip_sz);
                    } else {
                        fs_log(sd, "SWEEP_PURGE [tmp_in_stale] %s", ents[i].name);
                        fsFsDeleteDirectoryRecursively(sd, full);
                    }
                }
            }
            fsDirClose(&tdir);
        }
    }
    fsFsCommit(sd);

    // Validate durable queues.
    validate_outbound(sd);
    deduplicate_outbound(sd);
    validate_inbound(sd);
    fsFsCommit(sd);

    // Check if there's work so dirty flag is set before first tick.
    FsDir odir;
    if (R_SUCCEEDED(fsFsOpenDirectory(sd, OMNI_ROOT "/outbound",
            FsDirOpenMode_ReadDirs, &odir))) {
        FsDirectoryEntry ents[ENTRY_BATCH]; s64 count = 0;
        if (R_SUCCEEDED(fsDirRead(&odir, &count, ENTRY_BATCH, ents)) && count > 0)
            s_dirty = true;
        fsDirClose(&odir);
    }

    // Network poll deliberately excluded here — it can block for minutes and
    // would prevent pmdmnt from detecting game open/close during that time.
    // The heartbeat in do_idle handles server polling on the main loop thread.

    fs_log(sd, "SWEEP_DONE dirty=%d", (int)s_dirty);
}
