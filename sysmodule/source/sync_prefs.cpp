#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "omnisave.h"
#include "sync_prefs.h"

#define PREFS_PATH OMNI_ROOT "/sync_prefs.json"

// ── Storage ───────────────────────────────────────────────────────────────────

static SyncPrefEntry s_prefs[SYNC_PREFS_MAX];
static int           s_count = 0;

// ── JSON parser: {"<hex-title-id>": bool, ...} ────────────────────────────────

static int _parse_obj(const char* json, SyncPrefEntry* out, int max) {
    const char* p = strchr(json, '{');
    if (!p) return 0;
    p++;
    int count = 0;
    while (count < max) {
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r' || *p == '\t') p++;
        if (*p == '}' || *p == '\0') break;
        if (*p != '"') { p++; continue; }
        p++;
        char key[17] = {0};
        int k = 0;
        while (*p && *p != '"' && k < 16) key[k++] = *p++;
        if (*p != '"') continue;
        p++;
        while (*p == ' ') p++;
        if (*p != ':') continue;
        p++;
        while (*p == ' ') p++;
        bool enabled;
        if (strncmp(p, "false", 5) == 0) { enabled = false; p += 5; }
        else if (strncmp(p, "true",  4) == 0) { enabled = true;  p += 4; }
        else { p++; continue; }
        char* end;
        u64 tid = (u64)strtoull(key, &end, 16);
        if (end > key) {  // at least one valid hex digit parsed
            out[count].title_id = tid;
            out[count].enabled  = enabled;
            count++;
        }
    }
    return count;
}

// ── Public API ────────────────────────────────────────────────────────────────

void sync_prefs_reset(void) {
    s_count = 0;
    memset(s_prefs, 0, sizeof(s_prefs));
}

void sync_prefs_load(FsFileSystem* sd) {
    sync_prefs_reset();
    char buf[SYNC_PREFS_MAX * 32 + 16];
    if (!fs_read_text_file(sd, PREFS_PATH, buf, sizeof(buf))) return;
    s_count = _parse_obj(buf, s_prefs, SYNC_PREFS_MAX);
    fs_log(sd, "PREFS_LOAD count=%d", s_count);
}

static void _save(FsFileSystem* sd) {
    char buf[SYNC_PREFS_MAX * 32 + 16];
    int pos = 0;
    buf[pos++] = '{';
    for (int i = 0; i < s_count; i++) {
        if (i) buf[pos++] = ',';
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "\"%016llX\":%s",
                        (unsigned long long)s_prefs[i].title_id,
                        s_prefs[i].enabled ? "true" : "false");
    }
    buf[pos++] = '}';
    buf[pos++] = '\n';
    buf[pos]   = '\0';
    fs_write_text_file(sd, PREFS_PATH, buf);
}

void sync_prefs_apply_from_queue(FsFileSystem* sd, const char* queue_json) {
    // Find "sync_prefs": in the queue body.
    const char* key = strstr(queue_json, "\"sync_prefs\":");
    if (!key) return;
    const char* obj_start = strchr(key, '{');
    if (!obj_start) return;

    SyncPrefEntry tmp[SYNC_PREFS_MAX];
    int n = _parse_obj(obj_start, tmp, SYNC_PREFS_MAX);
    if (n == SYNC_PREFS_MAX) {
        fs_log(sd, "PREFS_CAP_WARN cap=%d reached; excess titles default-allowed", SYNC_PREFS_MAX);
    }
    s_count = n;
    memcpy(s_prefs, tmp, (size_t)n * sizeof(SyncPrefEntry));
    _save(sd);
}

bool sync_prefs_is_enabled(u64 title_id) {
    for (int i = 0; i < s_count; i++) {
        if (s_prefs[i].title_id == title_id)
            return s_prefs[i].enabled;
    }
    if (s_count >= SYNC_PREFS_MAX) {
        // Cap reached; unknown title cannot be stored — default allow, never block.
        return true;
    }
    return true;  // default allow
}

int sync_prefs_count(void) {
    return s_count;
}

// ── Game name cache ───────────────────────────────────────────────────────────

#define GAME_NAME_MAX 8  // only pending items need names; queue returns at most 4

typedef struct { u64 title_id; char name[48]; } GameNameEntry;
static GameNameEntry s_game_names[GAME_NAME_MAX];
static int           s_game_names_count = 0;

// Parse {"0100XXXX": "Name", ...} object into out array.
static int _parse_names(const char* json, GameNameEntry* out, int max) {
    const char* p = strchr(json, '{');
    if (!p) return 0;
    p++;
    int count = 0;
    while (count < max) {
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r' || *p == '\t') p++;
        if (*p == '}' || *p == '\0') break;
        if (*p != '"') { p++; continue; }
        p++;
        char key[17] = {0};
        int k = 0;
        while (*p && *p != '"' && k < 16) key[k++] = *p++;
        if (*p != '"') continue;
        p++;
        while (*p == ' ') p++;
        if (*p != ':') continue;
        p++;
        while (*p == ' ') p++;
        if (*p != '"') continue;
        p++;
        char name[48] = {0};
        int n = 0;
        while (*p && *p != '"' && n < 47) name[n++] = *p++;
        if (*p != '"') continue;
        p++;
        char* end;
        u64 tid = (u64)strtoull(key, &end, 16);
        if (end > key) {
            out[count].title_id = tid;
            snprintf(out[count].name, sizeof(out[count].name), "%s", name);
            count++;
        }
    }
    return count;
}

void sync_prefs_apply_game_names(const char* queue_json) {
    const char* key = strstr(queue_json, "\"game_names\":");
    if (!key) return;
    const char* obj_start = strchr(key, '{');
    if (!obj_start) return;
    s_game_names_count = _parse_names(obj_start, s_game_names, GAME_NAME_MAX);
}

bool sync_prefs_get_game_name(u64 title_id, char* out, size_t sz) {
    for (int i = 0; i < s_game_names_count; i++) {
        if (s_game_names[i].title_id == title_id) {
            snprintf(out, sz, "%s", s_game_names[i].name);
            return true;
        }
    }
    return false;
}
