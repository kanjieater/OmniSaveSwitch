#pragma once
#include <cstddef>

struct StatusViewModel {
    char label[64];      // primary text, e.g. "Up to Date"
    char secondary[64];  // subtitle or game name; empty if none
    char progress[16];   // e.g. "42%"; empty when not in-transfer
};

// Parse status.json content into a view model.
// resolve_name: optional callback (nullptr OK) for title_id → display name.
void parse_status(
    const char* json,
    bool offline,
    StatusViewModel& out,
    void (*resolve_name)(const char* tid, char* name, size_t sz));
