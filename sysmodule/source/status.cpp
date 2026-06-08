#include <stdio.h>
#include <string.h>
#include "omnisave.h"

// status.cpp — thin shims kept for legacy call-site compat.
// Real state is owned by state.cpp / state_write_status().
// These are no longer called from the new FSM; retained so the linker is happy
// if any stale object file references them.

void status_write_upload(FsFileSystem* sd, const char* key, int snap_id) {
    (void)key; (void)snap_id;
    state_write_status(sd, fsm_get_state(), key, 0, 0);
}

void status_write_inject(FsFileSystem* sd, const char* key, bool ok) {
    (void)key; (void)ok;
    state_write_status(sd, fsm_get_state(), key, 0, 0);
}

void status_write_current(FsFileSystem* sd, u64 title_id) {
    char json[64];
    if (title_id == 0)
        snprintf(json, sizeof(json), "{\"title_id\":\"\"}\n");
    else
        snprintf(json, sizeof(json), "{\"title_id\":\"%016llX\"}\n",
                 (unsigned long long)title_id);
    fs_write_text_file(sd, OMNI_ROOT "/state/current.json", json);
    fsFsCommit(sd);
}
