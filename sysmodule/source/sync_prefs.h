#pragma once
#include "omnisave.h"

// Per-device game sync preferences.  Server is authoritative; this module caches
// the last-seen prefs from the /queue heartbeat response.
//
// Memory model: fixed-size static array (no heap alloc, suitable for gameplay loops).
// Overflow policy: titles beyond SYNC_PREFS_MAX are default-allowed (never silently blocked).

#define SYNC_PREFS_MAX 256   // 256 × 9 bytes = 2304 bytes;  budget: well within inner heap

typedef struct {
    u64  title_id;
    bool enabled;
} SyncPrefEntry;

// Load preferences from SD card at boot.  No-op if file absent.
void sync_prefs_load(FsFileSystem* sd);

// Apply preferences from a queue-response JSON body.
// Replaces the in-memory map entirely, then persists to SD card.
void sync_prefs_apply_from_queue(FsFileSystem* sd, const char* queue_json);

// Returns true if the title should be synced.  Unknown titles default to true.
bool sync_prefs_is_enabled(u64 title_id);

// Number of entries currently loaded (visible for tests).
int sync_prefs_count(void);

// Reset to empty (all titles default-allow).  Used in tests and at boot before load.
void sync_prefs_reset(void);

// ── Game name cache (populated from queue response) ───────────────────────────

// Parse game_names field from queue response and cache it in memory.
void sync_prefs_apply_game_names(const char* queue_json);

// Look up a server-provided game name for display in notifications.
// Returns true and fills out[0..sz-1] if found; false if unknown.
bool sync_prefs_get_game_name(u64 title_id, char* out, size_t sz);
