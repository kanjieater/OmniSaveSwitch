#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <vector>
#include "omnisave.h"
#include "save_clear.h"
#include "mock/vfs.hpp"

static VfsData g_vfs;
static FsFileSystem g_fs;

static void setup_save_fs() {
    g_vfs = VfsData{};
    g_fs.impl = &g_vfs;
    // Create the root directory so fsFsOpenDirectory("/", ...) succeeds.
    g_vfs.make_dir("/");
}

// ── clear_save_root: fsDirRead failure ────────────────────────────────────────

SCENARIO("clear_save_root returns false when fsDirRead fails", "[save_ops]") {
    GIVEN("a save filesystem root with some files") {
        setup_save_fs();
        g_vfs.put_file("/data.bin",   std::vector<uint8_t>{0x01, 0x02});
        g_vfs.put_file("/config.dat", std::vector<uint8_t>{0xFF});
        g_vfs.fail_next_dir_read = true;   // inject fsDirRead failure

        WHEN("clear_save_root is called") {
            bool result = clear_save_root(&g_fs);
            THEN("returns false (did not silently succeed)") {
                REQUIRE_FALSE(result);
            }
            AND_THEN("files are NOT deleted (no partial cleanup)") {
                REQUIRE(g_vfs.has_file("/data.bin"));
                REQUIRE(g_vfs.has_file("/config.dat"));
            }
        }
    }
}

// ── clear_save_root: success path ─────────────────────────────────────────────

SCENARIO("clear_save_root returns true and deletes all root entries", "[save_ops]") {
    GIVEN("a save filesystem with files and a subdirectory") {
        setup_save_fs();
        g_vfs.put_file("/save1.dat", std::vector<uint8_t>{0xAA});
        g_vfs.put_file("/subdir/nested.dat", std::vector<uint8_t>{0xBB});
        g_vfs.fail_next_dir_read = false;

        WHEN("clear_save_root is called") {
            bool result = clear_save_root(&g_fs);
            THEN("returns true") { REQUIRE(result); }
            AND_THEN("root-level file is gone") {
                REQUIRE_FALSE(g_vfs.has_file("/save1.dat"));
            }
            AND_THEN("subdirectory is gone") {
                REQUIRE_FALSE(g_vfs.has_dir("/subdir"));
            }
        }
    }
}

// ── clear_save_root: fsFsOpenDirectory failure ────────────────────────────────

SCENARIO("clear_save_root returns false when root directory cannot be opened", "[save_ops]") {
    GIVEN("a save filesystem where root dir does not exist") {
        g_vfs = VfsData{};   // no make_dir("/") — root missing
        g_fs.impl = &g_vfs;

        WHEN("clear_save_root is called") {
            bool result = clear_save_root(&g_fs);
            THEN("returns false") { REQUIRE_FALSE(result); }
        }
    }
}
