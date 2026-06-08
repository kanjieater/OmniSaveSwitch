#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>
#include <vector>
#include "mock/vfs.hpp"
#include "mock/fsm_deps.hpp"
#include "omnisave.h"

// 52-char keys (SNAP_KEY_LEN = 52)
#define KEY_OLDER "20240101_120000_00-AABBCCDDAABBCCDD-1122334455667788"
#define KEY_NEWER "20240101_130000_00-AABBCCDDAABBCCDD-1122334455667788"
#define KEY_OTHER "20240101_120000_00-FFFFFFFFFFFFFFFF-1122334455667788"
// Invalid length key (too short)
#define KEY_BAD   "BAD_KEY"

static VfsData g_vfs;
static FsFileSystem g_fs;

static void setup_recovery() {
    g_vfs = VfsData{};
    g_fs.impl = &g_vfs;
    s_dirty = false;
    // Create required root dirs
    g_vfs.make_dir("/switch/omnisave/outbound");
    g_vfs.make_dir("/switch/omnisave/inbound");
    g_vfs.make_dir("/switch/omnisave/tmp_out");
    g_vfs.make_dir("/switch/omnisave/tmp_in");
    g_vfs.make_dir("/switch/omnisave/state");
    g_vfs.make_dir("/switch/omnisave/state/lineage");
}

static void add_outbound(const char* key) {
    std::string dir = std::string("/switch/omnisave/outbound/") + key;
    g_vfs.put_file(dir + "/save.zip", std::vector<uint8_t>{0x4F, 0x4D, 0x4E, 0x49});
}

static void add_outbound_no_savebin(const char* key) {
    std::string dir = std::string("/switch/omnisave/outbound/") + key;
    g_vfs.put_file(dir + "/other.dat", std::vector<uint8_t>{0x01});
}

static void add_inbound(const char* key) {
    std::string dir = std::string("/switch/omnisave/inbound/") + key;
    g_vfs.put_file(dir + "/save.zip", std::vector<uint8_t>{0x01});
}

// ── tmp dir purge ──────────────────────────────────────────────────────────────

SCENARIO("recovery_sweep purges tmp_out and non-key entries from tmp_in", "[recovery]") {
    GIVEN("stale files in tmp_out and tmp_in") {
        setup_recovery();
        g_vfs.put_file("/switch/omnisave/tmp_out/stale.bin", std::vector<uint8_t>{0xAA});
        g_vfs.put_file("/switch/omnisave/tmp_in/stale.bin",  std::vector<uint8_t>{0xBB});

        WHEN("recovery_sweep runs") {
            recovery_sweep(&g_fs, SWEEP_BOOT_CLEAN_ALL);

            THEN("tmp_out is empty") {
                REQUIRE_FALSE(g_vfs.has_file("/switch/omnisave/tmp_out/stale.bin"));
            }
            AND_THEN("tmp_in is empty") {
                REQUIRE_FALSE(g_vfs.has_file("/switch/omnisave/tmp_in/stale.bin"));
            }
        }
    }
}

// ── validate_outbound ──────────────────────────────────────────────────────────

SCENARIO("recovery_sweep discards outbound entries with wrong key length", "[recovery]") {
    GIVEN("an outbound entry with a malformed (too-short) key") {
        setup_recovery();
        g_vfs.put_file(std::string("/switch/omnisave/outbound/") + KEY_BAD + "/save.zip",
                       std::vector<uint8_t>{0x01});

        WHEN("recovery_sweep runs") {
            recovery_sweep(&g_fs, SWEEP_BOOT_CLEAN_ALL);

            THEN("malformed entry is removed") {
                REQUIRE_FALSE(g_vfs.has_file(
                    std::string("/switch/omnisave/outbound/") + KEY_BAD + "/save.zip"));
            }
        }
    }
}

SCENARIO("recovery_sweep discards outbound entry missing save.zip", "[recovery]") {
    GIVEN("a valid-length key directory without save.zip") {
        setup_recovery();
        add_outbound_no_savebin(KEY_OLDER);

        WHEN("recovery_sweep runs") {
            recovery_sweep(&g_fs, SWEEP_BOOT_CLEAN_ALL);

            THEN("the incomplete entry is removed") {
                REQUIRE_FALSE(g_vfs.has_file(
                    std::string("/switch/omnisave/outbound/") + KEY_OLDER + "/other.dat"));
            }
        }
    }
}

SCENARIO("recovery_sweep keeps valid outbound entry", "[recovery]") {
    GIVEN("a valid outbound entry with save.zip") {
        setup_recovery();
        add_outbound(KEY_OLDER);

        WHEN("recovery_sweep runs") {
            recovery_sweep(&g_fs, SWEEP_BOOT_CLEAN_ALL);

            THEN("valid entry survives") {
                REQUIRE(g_vfs.has_file(
                    std::string("/switch/omnisave/outbound/") + KEY_OLDER + "/save.zip"));
            }
            AND_THEN("dirty flag is set") { REQUIRE(s_dirty); }
        }
    }
}

// ── deduplicate_outbound ───────────────────────────────────────────────────────

SCENARIO("recovery_sweep deduplicates older key for same title+uid", "[recovery]") {
    GIVEN("two outbound entries for the same title+uid at different timestamps") {
        setup_recovery();
        add_outbound(KEY_OLDER);
        add_outbound(KEY_NEWER);

        WHEN("recovery_sweep runs") {
            recovery_sweep(&g_fs, SWEEP_BOOT_CLEAN_ALL);

            THEN("older key is evicted") {
                REQUIRE_FALSE(g_vfs.has_file(
                    std::string("/switch/omnisave/outbound/") + KEY_OLDER + "/save.zip"));
            }
            AND_THEN("newer key is retained") {
                REQUIRE(g_vfs.has_file(
                    std::string("/switch/omnisave/outbound/") + KEY_NEWER + "/save.zip"));
            }
        }
    }
}

SCENARIO("recovery_sweep keeps both entries for different titles", "[recovery]") {
    GIVEN("outbound entries for two different titles") {
        setup_recovery();
        add_outbound(KEY_OLDER);
        add_outbound(KEY_OTHER);

        WHEN("recovery_sweep runs") {
            recovery_sweep(&g_fs, SWEEP_BOOT_CLEAN_ALL);

            THEN("both entries are retained") {
                REQUIRE(g_vfs.has_file(
                    std::string("/switch/omnisave/outbound/") + KEY_OLDER + "/save.zip"));
                REQUIRE(g_vfs.has_file(
                    std::string("/switch/omnisave/outbound/") + KEY_OTHER + "/save.zip"));
            }
        }
    }
}

// ── validate_inbound (outbound-wins rule) ──────────────────────────────────────

SCENARIO("recovery_sweep discards inbound when outbound exists", "[recovery]") {
    GIVEN("outbound and inbound both present") {
        setup_recovery();
        add_outbound(KEY_NEWER);
        add_inbound(KEY_OLDER);

        WHEN("recovery_sweep runs") {
            recovery_sweep(&g_fs, SWEEP_BOOT_CLEAN_ALL);

            THEN("inbound entry is purged (outbound-wins rule §2.4)") {
                REQUIRE_FALSE(g_vfs.has_file(
                    std::string("/switch/omnisave/inbound/") + KEY_OLDER + "/save.zip"));
            }
            AND_THEN("outbound entry is retained") {
                REQUIRE(g_vfs.has_file(
                    std::string("/switch/omnisave/outbound/") + KEY_NEWER + "/save.zip"));
            }
        }
    }
}

SCENARIO("recovery_sweep keeps inbound when no outbound exists", "[recovery]") {
    GIVEN("only inbound present, no outbound") {
        setup_recovery();
        add_inbound(KEY_OLDER);

        WHEN("recovery_sweep runs") {
            recovery_sweep(&g_fs, SWEEP_BOOT_CLEAN_ALL);

            THEN("inbound entry is retained") {
                REQUIRE(g_vfs.has_file(
                    std::string("/switch/omnisave/inbound/") + KEY_OLDER + "/save.zip"));
            }
        }
    }
}

SCENARIO("recovery_sweep with no outbound dir → deduplicate_outbound returns early", "[recovery]") {
    GIVEN("only tmp dirs exist; outbound and inbound dirs are absent") {
        g_vfs = VfsData{};
        g_fs.impl = &g_vfs;
        s_dirty = false;
        g_vfs.make_dir("/switch/omnisave/tmp_out");
        g_vfs.make_dir("/switch/omnisave/tmp_in");
        // outbound dir deliberately omitted → deduplicate_outbound hits early return

        WHEN("recovery_sweep runs") {
            recovery_sweep(&g_fs, SWEEP_BOOT_CLEAN_ALL);

            THEN("dirty flag remains false (no outbound entries)") {
                REQUIRE_FALSE(s_dirty);
            }
        }
    }
}

SCENARIO("dirty flag not set when outbound queue is empty after sweep", "[recovery]") {
    GIVEN("no outbound entries") {
        setup_recovery();
        s_dirty = false;

        WHEN("recovery_sweep runs") {
            recovery_sweep(&g_fs, SWEEP_BOOT_CLEAN_ALL);

            THEN("dirty flag remains false") { REQUIRE_FALSE(s_dirty); }
        }
    }
}

// ── tmp_in partial-download keep/purge ─────────────────────────────────────────

SCENARIO("sweep keeps tmp_in dir with committed download progress", "[recovery]") {
    GIVEN("a valid-key tmp_in dir with a non-empty save.zip") {
        setup_recovery();
        g_vfs.put_file("/switch/omnisave/tmp_in/" KEY_OLDER "/save.zip",
                       std::vector<uint8_t>(4 * 1024 * 1024, 0xAB));

        WHEN("recovery_sweep runs on wake") {
            recovery_sweep(&g_fs, SWEEP_WAKE_SAFE_ONLY);

            THEN("the partial download dir is not deleted") {
                REQUIRE(g_vfs.has_file("/switch/omnisave/tmp_in/" KEY_OLDER "/save.zip"));
            }
        }
    }
}

SCENARIO("sweep purges tmp_in dir with empty save.zip", "[recovery]") {
    GIVEN("a valid-key tmp_in dir where save.zip has 0 bytes (no committed window)") {
        setup_recovery();
        g_vfs.put_file("/switch/omnisave/tmp_in/" KEY_OLDER "/save.zip",
                       std::vector<uint8_t>{});

        WHEN("recovery_sweep runs") {
            recovery_sweep(&g_fs, SWEEP_BOOT_CLEAN_ALL);

            THEN("the zero-progress dir is purged") {
                REQUIRE_FALSE(g_vfs.has_dir("/switch/omnisave/tmp_in/" KEY_OLDER));
            }
        }
    }
}

SCENARIO("sweep purges tmp_in dir with no save.zip at all", "[recovery]") {
    GIVEN("a valid-key tmp_in dir that has no save.zip") {
        setup_recovery();
        g_vfs.make_dir("/switch/omnisave/tmp_in/" KEY_OLDER);

        WHEN("recovery_sweep runs") {
            recovery_sweep(&g_fs, SWEEP_BOOT_CLEAN_ALL);

            THEN("the empty dir is purged") {
                REQUIRE_FALSE(g_vfs.has_dir("/switch/omnisave/tmp_in/" KEY_OLDER));
            }
        }
    }
}

SCENARIO("sweep purges tmp_in dir with wrong key length regardless of save.zip", "[recovery]") {
    GIVEN("a dir named 'bad_key' (not SNAP_KEY_LEN) with a large save.zip") {
        setup_recovery();
        g_vfs.put_file("/switch/omnisave/tmp_in/" KEY_BAD "/save.zip",
                       std::vector<uint8_t>(4 * 1024 * 1024, 0xAB));

        WHEN("recovery_sweep runs") {
            recovery_sweep(&g_fs, SWEEP_BOOT_CLEAN_ALL);

            THEN("the malformed-key dir is purged regardless of save.zip size") {
                REQUIRE_FALSE(g_vfs.has_dir("/switch/omnisave/tmp_in/" KEY_BAD));
            }
        }
    }
}

// ── state/ orphan recovery (P0-3: atomic_write crash safety) ──────────────────

#define STATE_FILE    "/switch/omnisave/state/lineage/AABB_1122.json"
#define STATE_TMP     STATE_FILE ".tmp"
#define STATE_BAK     STATE_FILE ".bak"

static const char* LINEAGE_JSON = "{\"base_snapshot_id\":\"K\",\"snapshot_counter\":5}\n";
static const char* LINEAGE_JSON6 = "{\"base_snapshot_id\":\"K\",\"snapshot_counter\":6}\n";

SCENARIO("sweep recovers state orphan: .tmp exists, original missing (no .bak)", "[recovery][p0-3]") {
    GIVEN("crash after write(.tmp) and delete(original), no .bak — old pattern") {
        setup_recovery();
        g_vfs.put_file(STATE_TMP, LINEAGE_JSON6);
        // Original does NOT exist

        WHEN("recovery_sweep runs") {
            recovery_sweep(&g_fs, SWEEP_BOOT_CLEAN_ALL);

            THEN("original is restored from .tmp") {
                REQUIRE(g_vfs.has_file(STATE_FILE));
            }
            AND_THEN(".tmp orphan is consumed") {
                REQUIRE_FALSE(g_vfs.has_file(STATE_TMP));
            }
        }
    }
}

SCENARIO("sweep recovers state orphan: .bak + .tmp present, original missing — prefer .tmp", "[recovery][p0-3]") {
    GIVEN("crash after rename(original→.bak), before rename(.tmp→original)") {
        setup_recovery();
        g_vfs.put_file(STATE_TMP, LINEAGE_JSON6);  // newer
        g_vfs.put_file(STATE_BAK, LINEAGE_JSON);   // older backup
        // Original does NOT exist

        WHEN("recovery_sweep runs") {
            recovery_sweep(&g_fs, SWEEP_BOOT_CLEAN_ALL);

            THEN("original is restored from newer .tmp") {
                REQUIRE(g_vfs.has_file(STATE_FILE));
                std::vector<uint8_t> content = g_vfs.get_file(STATE_FILE);
                std::string s(content.begin(), content.end());
                REQUIRE(s.find("\"snapshot_counter\":6") != std::string::npos);
            }
            AND_THEN("no orphans remain") {
                REQUIRE_FALSE(g_vfs.has_file(STATE_TMP));
                REQUIRE_FALSE(g_vfs.has_file(STATE_BAK));
            }
        }
    }
}

SCENARIO("sweep recovers state orphan: only .bak present, original missing — restore .bak", "[recovery][p0-3]") {
    GIVEN("crash after rename(original→.bak), .tmp gone (first write)") {
        setup_recovery();
        g_vfs.put_file(STATE_BAK, LINEAGE_JSON);
        // No .tmp, no original

        WHEN("recovery_sweep runs") {
            recovery_sweep(&g_fs, SWEEP_BOOT_CLEAN_ALL);

            THEN("original is restored from .bak") {
                REQUIRE(g_vfs.has_file(STATE_FILE));
            }
            AND_THEN(".bak is consumed") {
                REQUIRE_FALSE(g_vfs.has_file(STATE_BAK));
            }
        }
    }
}

SCENARIO("sweep cleans stale .bak when original already exists", "[recovery][p0-3]") {
    GIVEN("crash after rename(.tmp→original), before delete(.bak)") {
        setup_recovery();
        g_vfs.put_file(STATE_FILE, LINEAGE_JSON6);  // good target
        g_vfs.put_file(STATE_BAK,  LINEAGE_JSON);   // stale

        WHEN("recovery_sweep runs") {
            recovery_sweep(&g_fs, SWEEP_BOOT_CLEAN_ALL);

            THEN("original is unchanged") {
                std::vector<uint8_t> content = g_vfs.get_file(STATE_FILE);
                std::string s(content.begin(), content.end());
                REQUIRE(s.find("\"snapshot_counter\":6") != std::string::npos);
            }
            AND_THEN("stale .bak is removed") {
                REQUIRE_FALSE(g_vfs.has_file(STATE_BAK));
            }
        }
    }
}

SCENARIO("sweep cleans stale .tmp when original already exists", "[recovery][p0-3]") {
    GIVEN("stale .tmp alongside a valid original") {
        setup_recovery();
        g_vfs.put_file(STATE_FILE, LINEAGE_JSON6);
        g_vfs.put_file(STATE_TMP,  LINEAGE_JSON);   // stale

        WHEN("recovery_sweep runs") {
            recovery_sweep(&g_fs, SWEEP_BOOT_CLEAN_ALL);

            THEN("original is unchanged") {
                std::vector<uint8_t> content = g_vfs.get_file(STATE_FILE);
                std::string s(content.begin(), content.end());
                REQUIRE(s.find("\"snapshot_counter\":6") != std::string::npos);
            }
            AND_THEN("stale .tmp is removed") {
                REQUIRE_FALSE(g_vfs.has_file(STATE_TMP));
            }
        }
    }
}
