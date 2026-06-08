#pragma once
#include <cstddef>

struct HistoryViewModel {
    // Home mode (show_name=true):  line1=name, line2=timestamp, line3="Save #N"
    // In-game mode (show_name=false): line1=timestamp, line2="Save #N", line3=""
    char line1[64];
    char line2[64];
    char line3[64];
    bool valid;  // false if json was null/empty (file missing)
};

// Parse a backup or restore JSON string into a view model.
// json: file contents (may be null or empty → valid=false, all lines empty).
// show_name: true = home mode (name/ts/snap), false = in-game mode (ts/snap/-).
// resolve_name: optional callback for title_id → display name; nullptr OK.
void parse_history(
    const char* json,
    bool show_name,
    HistoryViewModel& out,
    void (*resolve_name)(const char* tid, char* name, size_t sz));
