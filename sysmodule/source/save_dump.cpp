#include <string.h>
#include <stdio.h>
#include "omnisave.h"

#pragma GCC diagnostic ignored "-Wformat-truncation"

// FsSaveDataSpaceId_All does not reliably enumerate all spaces (documented in
// JKSV source). Iterate User and SdUser explicitly to capture all account saves.
static const FsSaveDataSpaceId DUMP_SPACES[] = {
    FsSaveDataSpaceId_User,
    FsSaveDataSpaceId_SdUser,
};

void dump_all_saves(void) {
    FsFileSystem sd;
    if (R_FAILED(fsOpenSdCardFileSystem(&sd))) return;

    u64 posix = get_posix_utc();
    char ts_folder[32];
    make_ts_folder(ts_folder, sizeof(ts_folder), posix);

    int count_queued = 0;
    bool aborted = false;

    for (int si = 0; si < 2 && !aborted; si++) {
        FsSaveDataInfoReader reader;
        if (R_FAILED(fsOpenSaveDataInfoReader(&reader, DUMP_SPACES[si]))) continue;

        FsSaveDataInfo entries[ENTRY_BATCH];
        s64 count = 0;
        while (!aborted &&
               R_SUCCEEDED(fsSaveDataInfoReaderRead(&reader, entries, ENTRY_BATCH, &count))
               && count > 0) {
            for (s64 i = 0; i < count && !aborted; i++) {
                if (s_running_pid != 0) { aborted = true; break; }
                if (entries[i].save_data_type != FsSaveDataType_Account) continue;
                if (entries[i].application_id == 0) continue;
                if (entries[i].application_id == s_running_title_id) continue;

                u64 title_id = entries[i].application_id;
                FsSaveDataAttribute attr;
                memset(&attr, 0, sizeof(attr));
                attr.application_id = entries[i].application_id;
                attr.uid            = entries[i].uid;
                attr.save_data_type = entries[i].save_data_type;

                FsFileSystem save_fs;
                if (R_FAILED(fsOpenSaveDataFileSystem(&save_fs,
                        (FsSaveDataSpaceId)entries[i].save_data_space_id, &attr)))
                    continue;

                snprintf(s_uid_hex, sizeof(s_uid_hex), "%016llX",
                         (unsigned long long)entries[i].uid.uid[0]);
                s_save_data_size = entries[i].size;
                s_save_data_id   = entries[i].save_data_id;
                s_save_space_id  = entries[i].save_data_space_id;

                char key[SNAP_KEY_LEN + 2];
                snprintf(key, sizeof(key), "%.15s_%02u-%016llX-%.16s",
                         ts_folder, s_key_seq++ % 100,
                         (unsigned long long)title_id, s_uid_hex);

                char tmp_path[FS_MAX_PATH], tmp_packed[FS_MAX_PATH], out_path[FS_MAX_PATH];
                snprintf(tmp_path,   sizeof(tmp_path),   OMNI_ROOT "/tmp_out/%s",          key);
                snprintf(tmp_packed, sizeof(tmp_packed), OMNI_ROOT "/tmp_out/%s/save.zip", key);
                snprintf(out_path,   sizeof(out_path),   OMNI_ROOT "/outbound/%s",         key);

                fsFsDeleteDirectoryRecursively(&sd, tmp_path);
                fsFsCreateDirectory(&sd, tmp_path);

                bool ok = pack_save(&save_fs, "/", &sd, tmp_packed, s_save_data_size, NULL, NULL);
                fsFsClose(&save_fs);
                if (!ok) { fsFsDeleteDirectoryRecursively(&sd, tmp_path); continue; }

                fsFsCommit(&sd);
                if (R_SUCCEEDED(fsFsRenameDirectory(&sd, tmp_path, out_path))) {
                    fsFsCommit(&sd);
                    count_queued++;
                } else {
                    fsFsDeleteDirectoryRecursively(&sd, tmp_path);
                }
            }
        }
        fsSaveDataInfoReaderClose(&reader);
    }

    fs_log(&sd, "DUMP_ALL queued=%d", count_queued);
    fsFsCommit(&sd);
    fsFsClose(&sd);
}
