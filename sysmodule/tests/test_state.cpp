#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>
#include "mock/vfs.hpp"
#include "omnisave.h"

// ── Test fixtures ──────────────────────────────────────────────────────────────

static VfsData g_vfs;
static FsFileSystem g_fs;

#define TID "AABBCCDDAABBCCDD"
#define UID "1122334455667788"
#define SNAP_K1 "20240101_120000_00-AABBCCDDAABBCCDD-1122334455667788"

#define LINEAGE_PATH \
    OMNI_ROOT "/state/lineage/" TID "_" UID ".json"
#define LINEAGE_TMP_PATH  LINEAGE_PATH ".tmp"
#define LINEAGE_BAK_PATH  LINEAGE_PATH ".bak"

static void setup_state() {
    g_vfs = VfsData{};
    g_fs.impl = &g_vfs;
    g_vfs.make_dir("/switch/omnisave");
    g_vfs.make_dir("/switch/omnisave/state");
    g_vfs.make_dir("/switch/omnisave/state/lineage");
    g_vfs.make_dir("/switch/omnisave/outbound");
    g_vfs.make_dir("/switch/omnisave/inbound");
    g_vfs.make_dir("/switch/omnisave/tmp_out");
    g_vfs.make_dir("/switch/omnisave/tmp_in");
}

// ── Round-trip: verify atomic_write happy path ─────────────────────────────────

SCENARIO("state_write_lineage / state_read_lineage round-trip", "[state]") {
    GIVEN("state directories exist") {
        setup_state();

        WHEN("lineage is written then read back") {
            state_write_lineage(&g_fs, TID, UID, SNAP_K1, 5);

            int counter = -1;
            char snap[64] = {0};
            bool found = state_read_lineage(&g_fs, TID, UID, snap, sizeof(snap), &counter);

            THEN("found=true, counter=5, snap_id matches") {
                REQUIRE(found);
                REQUIRE(counter == 5);
                REQUIRE(strcmp(snap, SNAP_K1) == 0);
            }
            AND_THEN("no orphan .tmp or .bak files remain") {
                REQUIRE_FALSE(g_vfs.has_file(LINEAGE_TMP_PATH));
                REQUIRE_FALSE(g_vfs.has_file(LINEAGE_BAK_PATH));
            }
        }
    }
}

// ── P0-3 FAILING TEST: crash window between delete and rename ──────────────────
//
// OLD atomic_write pattern: write .tmp → delete original → rename .tmp → original
// If crash occurs AFTER delete but BEFORE rename, the original file is permanently lost.
//
// We simulate this by manually placing the post-crash state on disk:
//   • LINEAGE_TMP_PATH exists with counter=6 (the stranded new data)
//   • LINEAGE_PATH does NOT exist (was deleted before crash)
//   • No .bak file (old code had no .bak)
//
// BEFORE FIX: recovery_sweep does not handle state/ .tmp files,
//             so state_read_lineage() returns counter=0 → data is lost → FAIL
// AFTER FIX:  recovery_sweep detects the orphan .tmp and renames it to the
//             target path → state_read_lineage() returns counter=6 → PASS

SCENARIO("P0-3: old crash pattern — orphan .tmp recovered on boot", "[state][p0-3]") {
    GIVEN("crash state: .tmp has counter=6, original deleted, no .bak") {
        setup_state();
        const char* lineage_json =
            "{\n"
            "  \"base_snapshot_id\": \"" SNAP_K1 "\",\n"
            "  \"snapshot_counter\": 6,\n"
            "  \"last_synced_at\": \"2024-01-01 12:00:00\"\n"
            "}\n";
        g_vfs.put_file(LINEAGE_TMP_PATH, lineage_json);
        // Original file does NOT exist (was deleted by old atomic_write before crash)

        WHEN("recovery_sweep runs on boot") {
            recovery_sweep(&g_fs, SWEEP_BOOT_CLEAN_ALL);

            THEN("lineage counter is recovered (not lost)") {
                int counter = 0;
                state_read_lineage(&g_fs, TID, UID, nullptr, 0, &counter);
                // BEFORE FIX: counter==0 (FAIL — data lost)
                // AFTER FIX:  counter==6 (PASS — recovered from .tmp)
                REQUIRE(counter == 6);
            }
            AND_THEN("orphan .tmp is consumed — no files left behind") {
                REQUIRE_FALSE(g_vfs.has_file(LINEAGE_TMP_PATH));
            }
        }
    }
}

// ── P0-3 FAILING TEST: crash after bak-rename, before tmp-rename ──────────────
//
// NEW atomic_write pattern (after partial fix): write .tmp → commit → rename
// original → .bak → rename .tmp → original → delete .bak
//
// If crash after "rename original → .bak" but before "rename .tmp → original":
//   • LINEAGE_TMP_PATH exists with counter=6 (new write, stranded)
//   • LINEAGE_BAK_PATH exists with counter=5 (original, backed up)
//   • LINEAGE_PATH does NOT exist
//
// BEFORE FIX: recovery_sweep does not handle .bak/.tmp in state/
//             → state_read_lineage() returns counter=0 → FAIL
// AFTER FIX:  recovery_sweep sees .bak exists, target missing, .tmp present
//             → prefers newer .tmp, renames to target → counter=6 → PASS

SCENARIO("P0-3: crash after bak-rename, before tmp-rename — data not lost", "[state][p0-3]") {
    GIVEN("crash state: .tmp=counter6, .bak=counter5, original missing") {
        setup_state();
        const char* json5 =
            "{\n"
            "  \"base_snapshot_id\": \"" SNAP_K1 "\",\n"
            "  \"snapshot_counter\": 5,\n"
            "  \"last_synced_at\": \"2024-01-01 11:00:00\"\n"
            "}\n";
        const char* json6 =
            "{\n"
            "  \"base_snapshot_id\": \"" SNAP_K1 "\",\n"
            "  \"snapshot_counter\": 6,\n"
            "  \"last_synced_at\": \"2024-01-01 12:00:00\"\n"
            "}\n";
        g_vfs.put_file(LINEAGE_TMP_PATH, json6);
        g_vfs.put_file(LINEAGE_BAK_PATH, json5);
        // Target does NOT exist (crash mid-rename)

        WHEN("recovery_sweep runs on boot") {
            recovery_sweep(&g_fs, SWEEP_BOOT_CLEAN_ALL);

            THEN("lineage counter is not zero — data not lost") {
                int counter = 0;
                state_read_lineage(&g_fs, TID, UID, nullptr, 0, &counter);
                // BEFORE FIX: counter==0 (FAIL)
                // AFTER FIX:  counter==6 (PASS — preferred newer .tmp)
                REQUIRE(counter > 0);
            }
            AND_THEN("no .tmp or .bak orphans remain") {
                REQUIRE_FALSE(g_vfs.has_file(LINEAGE_TMP_PATH));
                REQUIRE_FALSE(g_vfs.has_file(LINEAGE_BAK_PATH));
            }
        }
    }
}

// ── Stale .bak cleanup (successful write left .bak behind) ────────────────────
//
// If crash after "rename .tmp → original" but before "delete .bak":
//   • Target exists with good content
//   • .bak is stale (from the previous value)
// Recovery should delete the stale .bak without touching the target.

SCENARIO("P0-3: stale .bak deleted when target already exists", "[state][p0-3]") {
    GIVEN("target exists, stale .bak also present") {
        setup_state();
        const char* json5 =
            "{\n"
            "  \"base_snapshot_id\": \"" SNAP_K1 "\",\n"
            "  \"snapshot_counter\": 5,\n"
            "  \"last_synced_at\": \"2024-01-01 11:00:00\"\n"
            "}\n";
        const char* json6 =
            "{\n"
            "  \"base_snapshot_id\": \"" SNAP_K1 "\",\n"
            "  \"snapshot_counter\": 6,\n"
            "  \"last_synced_at\": \"2024-01-01 12:00:00\"\n"
            "}\n";
        g_vfs.put_file(LINEAGE_PATH, json6);  // target is good
        g_vfs.put_file(LINEAGE_BAK_PATH, json5);  // stale .bak

        WHEN("recovery_sweep runs on boot") {
            recovery_sweep(&g_fs, SWEEP_BOOT_CLEAN_ALL);

            THEN("target is unchanged at counter=6") {
                int counter = 0;
                state_read_lineage(&g_fs, TID, UID, nullptr, 0, &counter);
                REQUIRE(counter == 6);
            }
            AND_THEN("stale .bak is removed") {
                REQUIRE_FALSE(g_vfs.has_file(LINEAGE_BAK_PATH));
            }
        }
    }
}

// ── state_atomic_write: public wrapper (used by transport_upload for session.json) ──

SCENARIO("state_atomic_write round-trip", "[state][p0-4]") {
    GIVEN("state directories exist") {
        setup_state();
        const char* path = OMNI_ROOT "/state/session.json";
        const char* content = "{\"session_id\":\"abc\",\"total_bytes\":1024}\n";

        WHEN("state_atomic_write writes and reads back") {
            state_atomic_write(&g_fs, path, content);

            char buf[256] = {0};
            bool ok = fs_read_text_file(&g_fs, path, buf, sizeof(buf));

            THEN("file contains the written content") {
                REQUIRE(ok);
                REQUIRE(strstr(buf, "abc") != nullptr);
            }
            AND_THEN("no .tmp or .bak orphans remain") {
                REQUIRE_FALSE(g_vfs.has_file(std::string(path) + ".tmp"));
                REQUIRE_FALSE(g_vfs.has_file(std::string(path) + ".bak"));
            }
        }
    }
}

// ── state_append_event ring-buffer wrap ───────────────────────────────────────

SCENARIO("state_append_event ring buffer wraps at MAX_EVENTS", "[state]") {
    GIVEN("state directories exist") {
        setup_state();

        WHEN("11 distinct events are appended") {
            for (int i = 1; i <= 11; i++) {
                char msg[32];
                snprintf(msg, sizeof(msg), "evt-%02d", i);
                state_append_event(&g_fs, "info", msg);
            }

            THEN("newest event (evt-11) is present in events.json") {
                char buf[4096] = {0};
                bool ok = fs_read_text_file(&g_fs, OMNI_ROOT "/state/events.json",
                                            buf, sizeof(buf));
                REQUIRE(ok);
                REQUIRE(strstr(buf, "evt-11") != nullptr);
            }
            AND_THEN("oldest event (evt-01) is no longer present") {
                char buf[4096] = {0};
                fs_read_text_file(&g_fs, OMNI_ROOT "/state/events.json",
                                  buf, sizeof(buf));
                // "evt-01" is distinct enough (evt-10, evt-11 won't match this prefix)
                REQUIRE(strstr(buf, "\"evt-01\"") == nullptr);
            }
        }
    }
}
