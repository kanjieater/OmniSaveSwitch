#include <stdio.h>
#include <string.h>
#include "omnisave.h"

bool g_verbose_notif = false;

// Ultrahand reads .notify files from this directory and displays them as toasts.
// Files are auto-deleted by Ultrahand after display.
#define NOTIF_DIR "/config/ultrahand/notifications"
#define PROGRAM_ID "0100000000000001"

static void ensure_notif_dirs(FsFileSystem* sd) {
    fsFsCreateDirectory(sd, "/config");
    fsFsCreateDirectory(sd, "/config/ultrahand");
    fsFsCreateDirectory(sd, NOTIF_DIR);
}

void notif_verbose(const char* event_slug, const char* message) {
    if (g_verbose_notif) notif_push(event_slug, message);
}

static bool notif_slug_pending(FsFileSystem* sd, const char* slug) {
    FsDir dir;
    if (R_FAILED(fsFsOpenDirectory(sd, NOTIF_DIR, FsDirOpenMode_ReadFiles, &dir)))
        return false;
    char suffix[128];
    snprintf(suffix, sizeof(suffix), "-%s.notify", slug);
    size_t slen = strlen(suffix);
    FsDirectoryEntry ents[1];
    s64 count = 0;
    bool found = false;
    while (!found &&
           R_SUCCEEDED(fsDirRead(&dir, &count, 1, ents)) && count > 0) {
        for (s64 i = 0; i < count && !found; i++) {
            size_t nlen = strlen(ents[i].name);
            if (nlen > slen && strcmp(ents[i].name + nlen - slen, suffix) == 0)
                found = true;
        }
    }
    fsDirClose(&dir);
    return found;
}

void notif_push(const char* event_slug, const char* message) {
    u64 ts = 0;
    timeGetCurrentTime(TimeType_UserSystemClock, &ts);

    char path[192];
    snprintf(path, sizeof(path),
             NOTIF_DIR "/" PROGRAM_ID "-%014llu-%s.notify",
             (unsigned long long)ts, event_slug);

    char body[256];
    snprintf(body, sizeof(body),
             "{\"text\": \"%s\", \"font_size\": 20, \"priority\": 10}",
             message);

    FsFileSystem sd;
    if (R_FAILED(fsOpenSdCardFileSystem(&sd))) return;
    ensure_notif_dirs(&sd);
    if (!notif_slug_pending(&sd, event_slug))
        fs_write_text_file(&sd, path, body);
    fsFsClose(&sd);
}
