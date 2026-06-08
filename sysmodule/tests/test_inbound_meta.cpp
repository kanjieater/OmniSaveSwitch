/**
 * Regression tests for the buf[64] inbound-stuck bug.
 *
 * Root cause: build_snapshot() declared `buf[64]` for the inbound meta JSON
 * but the meta JSON written by download_transaction / DL_SKIP_EXISTS fast-path
 * is ~72 bytes.  fs_read_text_file checks `size >= buf_sz` and returns false,
 * so has_inbound was never set — saves stuck in inbound/ forever.
 *
 * These tests verify fs_read_text_file returns the correct result for files
 * exactly at and around the old limit so regressions are caught immediately.
 */

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>
#include "mock/vfs.hpp"
#include "omnisave.h"

static VfsData    g_vfs;
static FsFileSystem g_fs;

static void setup() {
    g_vfs = VfsData{};
    g_fs.impl = &g_vfs;
}

// ── fs_read_text_file buffer boundary ─────────────────────────────────────────

// A realistic meta JSON as written by download_transaction / DL_SKIP_EXISTS:
// {"transaction_id":"550e8400-e29b-41d4-a716-446655440000","snap_id":0}
// That is 72 bytes (not counting the trailing newline; with newline: 73 bytes).
static const char META_JSON[] =
    "{\"transaction_id\":\"550e8400-e29b-41d4-a716-446655440000\",\"snap_id\":0}\n";

SCENARIO("fs_read_text_file: 72-byte meta JSON is readable with buf[128]", "[inbound_meta]") {
    GIVEN("meta JSON file of 72 bytes in VFS") {
        setup();
        g_vfs.put_file("/switch/omnisave/inbound/test.json", std::string(META_JSON));

        WHEN("reading with buf[128]") {
            char buf[128];
            bool ok = fs_read_text_file(&g_fs, "/switch/omnisave/inbound/test.json",
                                        buf, sizeof(buf));
            THEN("returns true") { REQUIRE(ok == true); }
            THEN("snap_id is parseable") {
                const char* p = strstr(buf, "\"snap_id\":");
                REQUIRE(p != nullptr);
            }
        }
    }
}

SCENARIO("fs_read_text_file: 72-byte meta JSON silently fails with old buf[64]", "[inbound_meta]") {
    GIVEN("meta JSON file of 72 bytes in VFS") {
        setup();
        g_vfs.put_file("/switch/omnisave/inbound/test.json", std::string(META_JSON));

        WHEN("reading with buf[64] (old, buggy size)") {
            char buf[64];
            bool ok = fs_read_text_file(&g_fs, "/switch/omnisave/inbound/test.json",
                                        buf, sizeof(buf));
            THEN("returns false — this is the bug that caused stuck saves") {
                REQUIRE(ok == false);
            }
        }
    }
}

SCENARIO("fs_read_text_file: file exactly at buf_sz - 1 is readable", "[inbound_meta]") {
    GIVEN("a 127-byte file with buf[128]") {
        setup();
        // 127 printable chars + null = 128; size=127 < buf_sz=128 → ok
        char content[128];
        memset(content, 'x', 127);
        content[127] = '\0';
        g_vfs.put_file("/switch/omnisave/inbound/big.json", std::string(content));

        WHEN("reading with buf[128]") {
            char buf[128];
            bool ok = fs_read_text_file(&g_fs, "/switch/omnisave/inbound/big.json",
                                        buf, sizeof(buf));
            THEN("returns true") { REQUIRE(ok == true); }
        }
    }
}

SCENARIO("fs_read_text_file: missing file returns false", "[inbound_meta]") {
    GIVEN("no file in VFS") {
        setup();
        WHEN("reading non-existent path") {
            char buf[128];
            bool ok = fs_read_text_file(&g_fs, "/switch/omnisave/inbound/missing.json",
                                        buf, sizeof(buf));
            THEN("returns false") { REQUIRE(ok == false); }
        }
    }
}

// ── write_inplace pattern: file never absent during repeated overwrites ──────
// Tests the core invariant of write_inplace (used in state_write_status):
// open-write-setsize never removes the file, unlike the old delete+rename path.

SCENARIO("write_inplace pattern: file always present during sequential overwrites", "[inbound_meta]") {
    GIVEN("status.json pre-created") {
        setup();
        // Simulate the first-time create path (no existing file).
        const char* path = "/switch/omnisave/state/status.json";
        const char* text1 = "{\"tick\":1,\"fsm_state\":\"IDLE\"}\n";
        const char* text2 = "{\"tick\":2,\"fsm_state\":\"DOWNLOADING\"}\n";
        const char* text3 = "{\"tick\":3,\"fsm_state\":\"IDLE\"}\n";

        // First write (create).
        {
            s64 len = (s64)strlen(text1);
            fsFsCreateFile(&g_fs, path, len, 0);
            FsFile f;
            fsFsOpenFile(&g_fs, path, FsOpenMode_Write, &f);
            fsFileSetSize(&f, len);
            fsFileWrite(&f, 0, text1, (u64)len, FsWriteOption_None);
            fsFileClose(&f);
        }
        REQUIRE(g_vfs.has_file(path));

        WHEN("subsequent overwrites using write_inplace pattern (open+setsize+write)") {
            for (const char* text : {text2, text3, text1, text2}) {
                s64 len = (s64)strlen(text);
                FsFile f;
                // Open existing file for write — does NOT delete it.
                if (R_FAILED(fsFsOpenFile(&g_fs, path, FsOpenMode_Write, &f))) {
                    fsFsCreateFile(&g_fs, path, len, 0);
                    fsFsOpenFile(&g_fs, path, FsOpenMode_Write, &f);
                }
                fsFileSetSize(&f, len);
                fsFileWrite(&f, 0, text, (u64)len, FsWriteOption_None);
                fsFileClose(&f);
                // File must ALWAYS be accessible — no delete gap.
                REQUIRE(g_vfs.has_file(path));
            }
            THEN("file has last written content") {
                char buf[128] = {};
                fs_read_text_file(&g_fs, path, buf, sizeof(buf));
                REQUIRE(std::string(buf).find("DOWNLOADING") != std::string::npos);
            }
        }
    }
}
