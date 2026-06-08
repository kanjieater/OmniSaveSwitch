#pragma once
// In-memory VFS backing all libnx FS calls during host tests.
// Tests create a VfsData, populate it, then set fs.impl = &vfs.

#include <cstdint>
#include <map>
#include <set>
#include <vector>
#include <string>

struct VfsDirEntry {
    std::string    name;
    bool           is_dir;
    long long      file_size;
};

struct VfsData {
    // Full absolute paths → contents.
    std::map<std::string, std::vector<uint8_t>> files;
    // Explicitly created dirs (also inferred from file paths).
    std::set<std::string> dirs;
    // If true, the next fsDirRead call returns failure (rc=1) and resets itself.
    bool fail_next_dir_read = false;
    // If > 0, the Nth fsFsRenameFile call fails (rc=1). Decremented each call;
    // fails and resets when it reaches 1 (i.e. set to 2 to fail on 2nd rename).
    int rename_fail_countdown = 0;

    // Return immediate children of parent_path, filtered by mode
    // (FsDirOpenMode_ReadFiles | FsDirOpenMode_ReadDirs).
    std::vector<VfsDirEntry> list_dir(const std::string& parent_path,
                                      unsigned mode) const;

    // Convenience helpers used in tests.
    void put_file(const std::string& path, const std::vector<uint8_t>& data);
    void put_file(const std::string& path, const std::string& text);
    bool has_file(const std::string& path) const;
    bool has_dir(const std::string& path) const;
    std::vector<uint8_t> get_file(const std::string& path) const;

    void make_dir(const std::string& path);
    void remove_file(const std::string& path);
    void remove_dir_recursive(const std::string& path);
};

// Called by fsFsOpenDirectory; maps FsDir* to cached entry list.
// vfs.cpp owns this map internally.
