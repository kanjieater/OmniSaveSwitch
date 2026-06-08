#include <stdio.h>
#include <string.h>
#include "save_clear.h"
#include "omnisave.h"

bool clear_save_root(FsFileSystem* save_fs) {
    FsDir dir;
    if (R_FAILED(fsFsOpenDirectory(save_fs, "/",
            FsDirOpenMode_ReadFiles | FsDirOpenMode_ReadDirs, &dir)))
        return false;
    FsDirectoryEntry ents[ENTRY_BATCH];
    s64 count = 0;
    if (R_FAILED(fsDirRead(&dir, &count, ENTRY_BATCH, ents))) {
        fsDirClose(&dir);
        return false;
    }
    fsDirClose(&dir);
    bool ok = true;
    for (s64 i = 0; i < count; i++) {
        char path[FS_MAX_PATH];
        path[0] = '/';
        strncpy(path + 1, ents[i].name, sizeof(path) - 2);
        path[sizeof(path) - 1] = '\0';
        if (ents[i].type == FsDirEntryType_Dir) {
            if (R_FAILED(fsFsDeleteDirectoryRecursively(save_fs, path))) ok = false;
        } else {
            if (R_FAILED(fsFsDeleteFile(save_fs, path))) ok = false;
        }
    }
    return ok;
}
