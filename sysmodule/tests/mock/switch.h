#pragma once
// Host-side stub for <switch.h> — replaces libnx for x86_64 test builds.
// Include path must list this directory before system includes.

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;
typedef u32      Result;

#define R_SUCCEEDED(rc)  ((rc) == 0u)
#define R_FAILED(rc)     ((rc) != 0u)
#define FS_MAX_PATH      1024

#define FsDirOpenMode_ReadFiles  (1u << 0)
#define FsDirOpenMode_ReadDirs   (1u << 1)

typedef enum { FsDirEntryType_Dir = 0, FsDirEntryType_File = 1 } FsDirEntryType;
typedef enum { FsOpenMode_Read = 1, FsOpenMode_Write = 2, FsOpenMode_Append = 4 } FsOpenMode;
typedef enum { FsWriteOption_None = 0, FsWriteOption_Flush = 1 } FsWriteOption;
typedef enum { FsReadOption_None  = 0 } FsReadOption;

typedef struct {
    char           name[FS_MAX_PATH];
    FsDirEntryType type;
    s64            file_size;
} FsDirectoryEntry;

// All three carry a void* into the in-memory VFS so that the implementations
// in vfs.cpp can recover the correct VfsData without any global state.
typedef struct { void* impl; } FsFileSystem;
typedef struct { void* impl; size_t cursor; char path[FS_MAX_PATH]; } FsDir;
typedef struct { void* impl; size_t cursor; char path[FS_MAX_PATH]; int mode; } FsFile;

// ── Account types ──────────────────────────────────────────────────────────────
typedef struct { u64 uid[2]; } AccountUid;
typedef struct { u8 _opaque[0x80]; } AccountProfile;
typedef struct { char nickname[33]; u8 _pad[0x27]; } AccountProfileBase;
typedef struct { u8 _opaque[0x40]; } AccountUserData;

// ── Save-data enumeration types (catalog.cpp / save_dump.cpp) ─────────────────
typedef enum {
    FsSaveDataType_Account = 1,
} FsSaveDataType;

typedef enum {
    FsSaveDataSpaceId_System  = 0,
    FsSaveDataSpaceId_User    = 1,
    FsSaveDataSpaceId_SdUser  = 4,
    FsSaveDataSpaceId_All     = 7,
} FsSaveDataSpaceId;

typedef struct {
    u8         save_data_space_id;
    u8         save_data_type;
    u8         _pad[6];
    AccountUid uid;
    u64        system_save_data_id;
    u64        application_id;
    u64        size;           // save data size (libnx field name is .size, not .save_data_size)
    u64        journal_size;
    u64        available_size;
    u64        owner_id;
    u64        save_data_id;
    u64        _unk;
} FsSaveDataInfo;

typedef struct { s64 cursor; FsSaveDataSpaceId space; } FsSaveDataInfoReader;

typedef struct {
    u64        application_id;
    AccountUid uid;
    u8         save_data_type;
    u8         _pad[7];
} FsSaveDataAttribute;

// ── NS application record types (catalog.cpp installed-title enumeration) ──────
typedef struct {
    u64 application_id;
    u8  type;
    u8  _pad[7];
} NsApplicationRecord;

#ifdef __cplusplus
extern "C" {
#endif

Result fsFsOpenDirectory(FsFileSystem* fs, const char* path, u32 mode, FsDir* out);
Result fsDirRead(FsDir* dir, s64* out_count, size_t max_entries, FsDirectoryEntry* entries);
void   fsDirClose(FsDir* dir);

Result fsFsOpenFile(FsFileSystem* fs, const char* path, u32 mode, FsFile* out);
Result fsFileRead(FsFile* f, s64 off, void* buf, u64 size, u32 opt, u64* nr_out);
Result fsFileWrite(FsFile* f, s64 off, const void* buf, u64 size, u32 opt);
void   fsFileClose(FsFile* f);

Result fsOpenSaveDataInfoReader(FsSaveDataInfoReader* out, FsSaveDataSpaceId space);
Result fsSaveDataInfoReaderRead(FsSaveDataInfoReader* r, FsSaveDataInfo* out, s64 max, s64* count);
void   fsSaveDataInfoReaderClose(FsSaveDataInfoReader* r);

Result nsListApplicationRecord(NsApplicationRecord* out, s32 max, s32 offset, s32* count);

// ── Account service stubs ──────────────────────────────────────────────────────
Result accountListAllUsers(AccountUid* uids, s32 max_uids, s32* count_out);
Result accountGetLastOpenedUser(AccountUid* out);
Result accountGetProfile(AccountProfile* out, AccountUid uid);
Result accountProfileGet(AccountProfile* profile, AccountUserData* userdata_out,
                         AccountProfileBase* base_out);
void   accountProfileClose(AccountProfile* profile);

// ── Save FS creation types ─────────────────────────────────────────────────────
typedef struct {
    s64 save_data_size;
    s64 journal_size;
    s64 available_size;
    u64 owner_id;
    u8  save_data_space_id;
    u8  _pad[7];
} FsSaveDataCreationInfo;

typedef struct { u8 _opaque[0x10]; } FsSaveDataMetaInfo;

// ── Time types ────────────────────────────────────────────────────────────────

typedef struct {
    s32 year; s8 month; s8 day;
    s8  hour; s8 minute; s8 second;
    s8  _pad[2];
} TimeCalendarTime;

// ── File timestamp ─────────────────────────────────────────────────────────────
typedef struct {
    bool is_valid;
    u8   _pad[7];
    u64  created;
    u64  modified;
    u64  accessed;
} FsTimeStampRaw;

// ── Save FS open / close / extend / create ────────────────────────────────────
Result fsOpenSaveDataFileSystem(FsFileSystem* out, FsSaveDataSpaceId space,
                                const FsSaveDataAttribute* attr);
void   fsFsClose(FsFileSystem* fs);
Result fsExtendSaveDataFileSystem(FsSaveDataSpaceId space, u64 save_data_id,
                                  s64 data_size_delta, u64 journal_size_delta);
Result fsCreateSaveDataFileSystem(const FsSaveDataAttribute* attr,
                                  const FsSaveDataCreationInfo* cinfo,
                                  const FsSaveDataMetaInfo* meta);
Result fsFsGetFileTimeStampRaw(FsFileSystem* fs, const char* path, FsTimeStampRaw* out);

Result fsFsCreateFile(FsFileSystem* fs, const char* path, s64 size, u32 opt);
Result fsFsDeleteFile(FsFileSystem* fs, const char* path);
Result fsFsCreateDirectory(FsFileSystem* fs, const char* path);
Result fsFsDeleteDirectoryRecursively(FsFileSystem* fs, const char* path);
Result fsFsRenameDirectory(FsFileSystem* fs, const char* src, const char* dst);
Result fsFsRenameFile(FsFileSystem* fs, const char* src, const char* dst);
Result fsFsCommit(FsFileSystem* fs);
Result fsFileGetSize(FsFile* f, s64* size_out);
Result fsFileSetSize(FsFile* f, s64 size);

Result timeToCalendarTimeWithMyRule(u64 posix, TimeCalendarTime* out, void* info);

#ifdef __cplusplus
}
#endif
