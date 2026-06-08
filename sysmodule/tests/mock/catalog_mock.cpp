// Controllable mock for save-data reader and NS application listing.
// Tests populate g_mock_save_infos / g_mock_ns_records before calling
// catalog_enumerate(); the mock functions drain them in order.
// For space-specific tests, populate g_mock_save_infos_by_space instead.
#include "catalog_mock.hpp"

std::vector<FsSaveDataInfo>      g_mock_save_infos;
std::vector<NsApplicationRecord> g_mock_ns_records;
std::map<FsSaveDataSpaceId, std::vector<FsSaveDataInfo>> g_mock_save_infos_by_space;
std::vector<FsSaveDataSpaceId>   g_mock_spaces_requested;

extern "C" {

Result fsOpenSaveDataInfoReader(FsSaveDataInfoReader* out, FsSaveDataSpaceId space) {
    g_mock_spaces_requested.push_back(space);
    out->cursor = 0;
    out->space  = space;
    return 0;
}

Result fsSaveDataInfoReaderRead(FsSaveDataInfoReader* r, FsSaveDataInfo* out,
                                s64 max, s64* count) {
    // Use space-specific list when populated; fall back to legacy list.
    auto it = g_mock_save_infos_by_space.find(r->space);
    const auto& list = (it != g_mock_save_infos_by_space.end())
                       ? it->second
                       : g_mock_save_infos;
    s64 c = 0;
    while (r->cursor < (s64)list.size() && c < max)
        out[c++] = list[r->cursor++];
    *count = c;
    return 0;
}

void fsSaveDataInfoReaderClose(FsSaveDataInfoReader*) {}

Result nsListApplicationRecord(NsApplicationRecord* out, s32 max, s32 offset, s32* count) {
    s32 c = 0;
    while (offset + c < (s32)g_mock_ns_records.size() && c < max) {
        out[c] = g_mock_ns_records[offset + c];
        c++;
    }
    *count = c;
    return 0;
}

} // extern "C"
