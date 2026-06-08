#pragma once
#include <switch.h>

// Maximum title_ids enumerated in one catalog sweep.
#define CATALOG_MAX 512

typedef struct {
    u64 title_id;
} CatalogEntry;

// Enumerate save-data metadata (no save FS open — metadata read only).
// Deduplicates title_id across profiles. Returns count written into out[].
int  catalog_enumerate(CatalogEntry* out, int max);

// FNV-1a hash of sorted entries. Compare to s_catalog_hash to detect changes.
u64  catalog_hash(const CatalogEntry* entries, int count);

// Build the JSON array string for installed_titles in device-config payload.
// out must be at least 24 * count + 4 bytes. Returns bytes written (excl. NUL).
int  catalog_build_json(const CatalogEntry* entries, int count, char* out, int out_sz);

// Persistent hash of the last successfully reported catalog (0 = never reported).
extern u64 s_catalog_hash;
// True when catalog has changed and needs to be included in the next device-config POST.
extern bool s_catalog_dirty;
extern CatalogEntry s_catalog_entries[CATALOG_MAX];
extern int  s_catalog_count;
