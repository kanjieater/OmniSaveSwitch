#include "vfs.hpp"
#include "switch.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <cassert>

// ── VfsData helpers ────────────────────────────────────────────────────────────

static std::string norm(const std::string& p) {
    if (p.empty()) return "/";
    // Ensure leading slash, strip trailing slash.
    std::string s = (p[0] == '/') ? p : "/" + p;
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

void VfsData::put_file(const std::string& path, const std::vector<uint8_t>& data) {
    std::string n = norm(path);
    files[n] = data;
    dirs.insert("/");  // root always exists
    // Register all ancestor directories so fsFsOpenDirectory succeeds on them.
    for (size_t pos = 1; (pos = n.find('/', pos)) != std::string::npos; ++pos)
        dirs.insert(n.substr(0, pos));
}
void VfsData::put_file(const std::string& path, const std::string& text) {
    files[norm(path)] = std::vector<uint8_t>(text.begin(), text.end());
}
bool VfsData::has_file(const std::string& path) const {
    return files.count(norm(path)) > 0;
}
bool VfsData::has_dir(const std::string& path) const {
    std::string n = norm(path) + "/";
    for (auto& [k, _] : files)
        if (k.rfind(n, 0) == 0) return true;
    return dirs.count(norm(path)) > 0;
}
std::vector<uint8_t> VfsData::get_file(const std::string& path) const {
    auto it = files.find(norm(path));
    return it != files.end() ? it->second : std::vector<uint8_t>{};
}
void VfsData::make_dir(const std::string& path) { dirs.insert(norm(path)); }
void VfsData::remove_file(const std::string& path) { files.erase(norm(path)); }
void VfsData::remove_dir_recursive(const std::string& path) {
    std::string prefix = norm(path) + "/";
    for (auto it = files.begin(); it != files.end(); ) {
        if (it->first.rfind(prefix, 0) == 0) it = files.erase(it);
        else ++it;
    }
    for (auto it = dirs.begin(); it != dirs.end(); ) {
        if (it->rfind(norm(path), 0) == 0) it = dirs.erase(it);
        else ++it;
    }
}

std::vector<VfsDirEntry> VfsData::list_dir(const std::string& parent_path,
                                            unsigned mode) const {
    // Root dir: prefix is just "/" so "/foo.dat" → rest = "foo.dat".
    // Other dirs: prefix is "/dir/" so "/dir/foo.dat" → rest = "foo.dat".
    std::string np = norm(parent_path);
    std::string prefix = (np == "/") ? "/" : (np + "/");
    bool want_files = (mode & FsDirOpenMode_ReadFiles) != 0;
    bool want_dirs  = (mode & FsDirOpenMode_ReadDirs)  != 0;

    std::map<std::string, VfsDirEntry> seen;

    for (auto& [fpath, content] : files) {
        if (fpath.rfind(prefix, 0) != 0) continue;
        std::string rest = fpath.substr(prefix.size());
        auto slash = rest.find('/');
        if (slash == std::string::npos) {
            if (want_files && !seen.count(rest)) {
                seen[rest] = {rest, false, (long long)content.size()};
            }
        } else {
            std::string dirname = rest.substr(0, slash);
            if (want_dirs && !seen.count(dirname)) {
                seen[dirname] = {dirname, true, 0};
            }
        }
    }
    for (auto& dpath : dirs) {
        if (dpath.rfind(prefix, 0) != 0) continue;
        std::string rest = dpath.substr(prefix.size());
        if (rest.find('/') == std::string::npos && !rest.empty() && want_dirs)
            if (!seen.count(rest)) seen[rest] = {rest, true, 0};
    }

    std::vector<VfsDirEntry> out;
    for (auto& [_, e] : seen) out.push_back(e);
    return out;
}

// ── Dir enumeration cache ──────────────────────────────────────────────────────

static std::map<FsDir*, std::vector<VfsDirEntry>> g_dir_cache;

// ── libnx FS function implementations ─────────────────────────────────────────

extern "C" {

Result fsFsOpenDirectory(FsFileSystem* fs, const char* path, u32 mode, FsDir* out) {
    if (!fs || !fs->impl) return 1;
    auto* vfs = static_cast<VfsData*>(fs->impl);
    std::string npath = norm(path);  // collapse double-slash paths like "//journal"
    if (!vfs->has_dir(npath)) return 1;
    strncpy(out->path, npath.c_str(), FS_MAX_PATH - 1);
    out->path[FS_MAX_PATH - 1] = '\0';
    out->impl   = fs->impl;
    out->cursor = 0;
    g_dir_cache[out] = vfs->list_dir(npath, mode);
    return 0;
}

Result fsDirRead(FsDir* dir, s64* out_count, size_t max_entries,
                 FsDirectoryEntry* entries) {
    // Check fail-injection flag on the VfsData that backs this dir.
    if (dir && dir->impl) {
        auto* vfs = static_cast<VfsData*>(dir->impl);
        if (vfs->fail_next_dir_read) {
            vfs->fail_next_dir_read = false;
            *out_count = 0;
            return 1;
        }
    }
    auto it = g_dir_cache.find(dir);
    if (it == g_dir_cache.end()) { *out_count = 0; return 1; }
    auto& vec = it->second;
    s64 n = 0;
    while (dir->cursor < vec.size() && (size_t)n < max_entries) {
        auto& e = vec[dir->cursor++];
        strncpy(entries[n].name, e.name.c_str(), FS_MAX_PATH - 1);
        entries[n].name[FS_MAX_PATH - 1] = '\0';
        entries[n].type      = e.is_dir ? FsDirEntryType_Dir : FsDirEntryType_File;
        entries[n].file_size = (s64)e.file_size;
        n++;
    }
    *out_count = n;
    return 0;
}

void fsDirClose(FsDir* dir) { g_dir_cache.erase(dir); }

Result fsFsOpenFile(FsFileSystem* fs, const char* path, u32 mode, FsFile* out) {
    if (!fs || !fs->impl) return 1;
    auto* vfs = static_cast<VfsData*>(fs->impl);
    std::string npath = norm(path);
    bool is_read  = (mode & FsOpenMode_Read)  != 0;
    bool is_write = (mode & FsOpenMode_Write) != 0;
    if (is_read && !vfs->has_file(npath)) return 1;
    if (is_write && !vfs->has_file(npath)) vfs->put_file(npath, std::vector<uint8_t>{});
    strncpy(out->path, npath.c_str(), FS_MAX_PATH - 1);
    out->path[FS_MAX_PATH - 1] = '\0';
    out->impl   = fs->impl;
    out->cursor = (mode & FsOpenMode_Append) ? vfs->get_file(npath).size() : 0;
    out->mode   = (int)mode;
    return 0;
}

Result fsFileRead(FsFile* f, s64 off, void* buf, u64 size, u32, u64* nr_out) {
    auto* vfs = static_cast<VfsData*>(f->impl);
    auto& data = vfs->files[std::string(f->path)];
    if ((size_t)off >= data.size()) { *nr_out = 0; return 0; }
    u64 avail = (u64)(data.size() - (size_t)off);
    u64 to_read = (size < avail) ? size : avail;
    memcpy(buf, data.data() + off, (size_t)to_read);
    *nr_out = to_read;
    return 0;
}

Result fsFileWrite(FsFile* f, s64 off, const void* buf, u64 size, u32) {
    auto* vfs = static_cast<VfsData*>(f->impl);
    auto& data = vfs->files[std::string(f->path)];
    if ((size_t)off + size > data.size())
        data.resize((size_t)off + size);
    memcpy(data.data() + off, buf, (size_t)size);
    return 0;
}

void   fsFileClose(FsFile*) {}

Result fsFileGetSize(FsFile* f, s64* size_out) {
    if (!f || !f->impl || !size_out) return 1;
    auto* vfs = static_cast<VfsData*>(f->impl);
    auto it = vfs->files.find(std::string(f->path));
    if (it == vfs->files.end()) return 1;
    *size_out = (s64)it->second.size();
    return 0;
}

Result fsFsRenameDirectory(FsFileSystem* fs, const char* src, const char* dst) {
    auto* vfs = static_cast<VfsData*>(fs->impl);
    std::string sp = std::string(src), dp = std::string(dst);
    if (!vfs->has_dir(sp)) return 1;
    // Move all files whose paths begin with sp/
    std::string sp_prefix = sp + "/", dp_prefix = dp + "/";
    std::map<std::string, std::vector<uint8_t>> moved;
    for (auto it = vfs->files.begin(); it != vfs->files.end(); ) {
        if (it->first.rfind(sp_prefix, 0) == 0) {
            moved[dp_prefix + it->first.substr(sp_prefix.size())] = std::move(it->second);
            it = vfs->files.erase(it);
        } else { ++it; }
    }
    for (auto& [k, v] : moved) vfs->files[k] = std::move(v);
    vfs->dirs.erase(sp);
    vfs->make_dir(dp);
    return 0;
}

Result fsFsCreateFile(FsFileSystem* fs, const char* path, s64 size, u32) {
    auto* vfs = static_cast<VfsData*>(fs->impl);
    vfs->files[std::string(path)] = std::vector<uint8_t>((size_t)(size < 0 ? 0 : size));
    return 0;
}

Result fsFsDeleteFile(FsFileSystem* fs, const char* path) {
    auto* vfs = static_cast<VfsData*>(fs->impl);
    vfs->files.erase(std::string(path));
    return 0;
}

Result fsFsCreateDirectory(FsFileSystem* fs, const char* path) {
    auto* vfs = static_cast<VfsData*>(fs->impl);
    vfs->make_dir(path);
    return 0;
}

Result fsFsDeleteDirectoryRecursively(FsFileSystem* fs, const char* path) {
    auto* vfs = static_cast<VfsData*>(fs->impl);
    vfs->remove_dir_recursive(path);
    return 0;
}

Result fsFileSetSize(FsFile* f, s64 size) {
    auto* vfs = static_cast<VfsData*>(f->impl);
    auto& data = vfs->files[std::string(f->path)];
    data.resize((size_t)(size < 0 ? 0 : size));
    return 0;
}

Result fsFsRenameFile(FsFileSystem* fs, const char* src, const char* dst) {
    auto* vfs = static_cast<VfsData*>(fs->impl);
    if (vfs->rename_fail_countdown > 0 && --vfs->rename_fail_countdown == 0)
        return 1;
    auto it = vfs->files.find(std::string(src));
    if (it == vfs->files.end()) return 1;
    vfs->files[std::string(dst)] = std::move(it->second);
    vfs->files.erase(it);
    return 0;
}

Result fsFsCommit(FsFileSystem*) { return 0; }

// ── Account service stubs (all fail — save_ops hardware paths not exercised) ──

Result accountListAllUsers(AccountUid*, s32, s32* c) { if (c) *c = 0; return 1; }
Result accountGetLastOpenedUser(AccountUid*) { return 1; }
Result accountGetProfile(AccountProfile*, AccountUid) { return 1; }
Result accountProfileGet(AccountProfile*, AccountUserData*, AccountProfileBase*) { return 1; }
void   accountProfileClose(AccountProfile*) {}

// ── Save FS open / close / extend stubs ───────────────────────────────────────

Result fsOpenSaveDataFileSystem(FsFileSystem* out, FsSaveDataSpaceId, const FsSaveDataAttribute*) {
    if (out) out->impl = nullptr;
    return 1;
}
void   fsFsClose(FsFileSystem* fs) { if (fs) fs->impl = nullptr; }
Result fsExtendSaveDataFileSystem(FsSaveDataSpaceId, u64, s64, u64) { return 1; }
Result fsCreateSaveDataFileSystem(const FsSaveDataAttribute*, const FsSaveDataCreationInfo*,
                                  const FsSaveDataMetaInfo*) { return 1; }
Result fsFsGetFileTimeStampRaw(FsFileSystem*, const char*, FsTimeStampRaw* out) {
    if (out) out->is_valid = false;
    return 1;
}

} // extern "C"
