#include <string.h>
#include <stdio.h>
#include "omnisave.h"
#include "catalog.h"

u64  s_catalog_hash = 0;
bool s_catalog_dirty = false;
CatalogEntry s_catalog_entries[CATALOG_MAX];
int  s_catalog_count = 0;

static bool _has_title(const CatalogEntry* entries, int count, u64 tid) {
    for (int i = 0; i < count; i++)
        if (entries[i].title_id == tid) return true;
    return false;
}

// Second pass: merge all installed applications (including those without saves).
static void _merge_ns_titles(CatalogEntry* out, int* n, int max) {
    NsApplicationRecord batch[64];
    s32 offset = 0;
    s32 got = 0;
    while (R_SUCCEEDED(nsListApplicationRecord(batch, 64, offset, &got)) && got > 0) {
        for (s32 i = 0; i < got && *n < max; i++) {
            u64 tid = batch[i].application_id;
            if (tid == 0) continue;
            if (!_has_title(out, *n, tid))
                out[(*n)++].title_id = tid;
        }
        offset += got;
    }
}

// Enumerate save-data metadata (metadata read only, no save FS open), then
// merge all NS-installed titles so games without saves are also reported.
// Deduplicates title_id across profiles. Returns count.
// Iterates User and SdUser spaces explicitly — FsSaveDataSpaceId_All does not
// reliably enumerate all spaces (documented in JKSV source).
int catalog_enumerate(CatalogEntry* out, int max) {
    static const FsSaveDataSpaceId SPACES[] = {
        FsSaveDataSpaceId_User,
        FsSaveDataSpaceId_SdUser,
    };

    int n = 0;
    for (int si = 0; si < 2 && n < max; si++) {
        FsSaveDataInfoReader reader;
        if (R_FAILED(fsOpenSaveDataInfoReader(&reader, SPACES[si]))) continue;

        FsSaveDataInfo entries[ENTRY_BATCH];
        s64 count = 0;
        while (R_SUCCEEDED(fsSaveDataInfoReaderRead(&reader, entries, ENTRY_BATCH, &count))
               && count > 0 && n < max) {
            for (s64 i = 0; i < count && n < max; i++) {
                if (entries[i].save_data_type != FsSaveDataType_Account) continue;
                u64 tid = entries[i].application_id;
                if (tid == 0) continue;

                // Deduplicate — same title_id may appear across spaces or profiles.
                if (!_has_title(out, n, tid))
                    out[n++].title_id = tid;
            }
        }
        fsSaveDataInfoReaderClose(&reader);
    }

    _merge_ns_titles(out, &n, max);

    // Sort ascending for deterministic hash.
    for (int i = 1; i < n; i++) {
        CatalogEntry key = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].title_id > key.title_id) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }

    return n;
}

// FNV-1a hash of sorted entries.
u64 catalog_hash(const CatalogEntry* entries, int count) {
    u64 h = 14695981039346656037ULL;
    for (int i = 0; i < count; i++) {
        const u8* p = (const u8*)&entries[i].title_id;
        for (int b = 0; b < 8; b++) {
            h ^= p[b];
            h *= 1099511628211ULL;
        }
    }
    return h;
}

// Build JSON array of hex title_ids for the device-config payload.
int catalog_build_json(const CatalogEntry* entries, int count, char* out, int out_sz) {
    int pos = 0;
    pos += snprintf(out + pos, out_sz - pos, "[");
    for (int i = 0; i < count && pos < out_sz - 25; i++) {
        pos += snprintf(out + pos, out_sz - pos,
                        "%s\"%016llX\"",
                        (i > 0 ? "," : ""),
                        (unsigned long long)entries[i].title_id);
    }
    pos += snprintf(out + pos, out_sz - pos, "]");
    return pos;
}
