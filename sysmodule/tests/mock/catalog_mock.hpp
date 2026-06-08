#pragma once
#include <map>
#include <vector>
#include <switch.h>

extern std::vector<FsSaveDataInfo>      g_mock_save_infos;
extern std::vector<NsApplicationRecord> g_mock_ns_records;

// Space-keyed save lists: when non-empty for a given space, overrides g_mock_save_infos
// for that space. Allows tests to put different saves in User vs SdUser.
extern std::map<FsSaveDataSpaceId, std::vector<FsSaveDataInfo>> g_mock_save_infos_by_space;

// Records every space ID passed to fsOpenSaveDataInfoReader, in order of call.
extern std::vector<FsSaveDataSpaceId> g_mock_spaces_requested;
