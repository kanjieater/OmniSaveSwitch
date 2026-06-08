#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include "mock/vfs.hpp"
#include "omnisave.h"
#include "sync_prefs.h"

// Title IDs used in tests (matching FSM test key format)
#define TID_A  0xAABBCCDDAABBCCDDULL
#define TID_B  0xFFFFFFFFFFFFFFFFULL
#define TID_A_STR "AABBCCDDAABBCCDD"
#define TID_B_STR "FFFFFFFFFFFFFFFF"

// Outbound key with TID_A embedded
#define KEY_A "20240101_120000_00-AABBCCDDAABBCCDD-1122334455667788"
// Inbound key with TID_A embedded (format: <15hex>-<title_id>-0000...)
#define KEY_A_INBOUND "abcdef1234567-AABBCCDDAABBCCDD-0000000000000000"

static VfsData  g_vfs;
static FsFileSystem g_fs;

static void setup() {
    g_vfs = VfsData{};
    g_fs.impl = &g_vfs;
    sync_prefs_reset();
}

// ── Unit: is_enabled logic ────────────────────────────────────────────────────

SCENARIO("sync_prefs: unknown title defaults to enabled", "[sync_prefs]") {
    GIVEN("empty prefs map") {
        setup();
        THEN("unknown title returns true") {
            REQUIRE(sync_prefs_is_enabled(TID_A) == true);
        }
    }
}

SCENARIO("sync_prefs: disabled title returns false", "[sync_prefs]") {
    GIVEN("prefs applied with TID_A disabled") {
        setup();
        const char* json = "{\"pending\":[],\"hint\":null,"
                           "\"sync_prefs\":{\"" TID_A_STR "\":false}}";
        sync_prefs_apply_from_queue(&g_fs, json);
        THEN("TID_A is disabled") {
            REQUIRE(sync_prefs_is_enabled(TID_A) == false);
        }
        THEN("TID_B (unknown) is still enabled") {
            REQUIRE(sync_prefs_is_enabled(TID_B) == true);
        }
    }
}

SCENARIO("sync_prefs: re-enable restores default-allow", "[sync_prefs]") {
    GIVEN("TID_A disabled then re-enabled via queue response") {
        setup();
        const char* off = "{\"pending\":[],\"sync_prefs\":{\"" TID_A_STR "\":false}}";
        sync_prefs_apply_from_queue(&g_fs, off);
        REQUIRE(sync_prefs_is_enabled(TID_A) == false);

        const char* on = "{\"pending\":[],\"sync_prefs\":{\"" TID_A_STR "\":true}}";
        sync_prefs_apply_from_queue(&g_fs, on);
        THEN("TID_A is enabled again") {
            REQUIRE(sync_prefs_is_enabled(TID_A) == true);
        }
    }
}

// ── Unit: apply_from_queue replaces map entirely ─────────────────────────────

SCENARIO("sync_prefs: apply replaces entire map", "[sync_prefs]") {
    GIVEN("TID_A disabled via first response") {
        setup();
        const char* first = "{\"sync_prefs\":{\"" TID_A_STR "\":false}}";
        sync_prefs_apply_from_queue(&g_fs, first);
        REQUIRE(sync_prefs_count() == 1);

        WHEN("second response omits TID_A entirely") {
            const char* second = "{\"sync_prefs\":{\"" TID_B_STR "\":false}}";
            sync_prefs_apply_from_queue(&g_fs, second);
            THEN("map has one entry (TID_B)") {
                REQUIRE(sync_prefs_count() == 1);
            }
            THEN("TID_A is now unknown (default-allow)") {
                REQUIRE(sync_prefs_is_enabled(TID_A) == true);
            }
            THEN("TID_B is disabled") {
                REQUIRE(sync_prefs_is_enabled(TID_B) == false);
            }
        }
    }
}

// ── Unit: missing sync_prefs field is a no-op ─────────────────────────────────

SCENARIO("sync_prefs: missing field in queue response leaves map unchanged", "[sync_prefs]") {
    GIVEN("TID_A disabled") {
        setup();
        const char* setup_json = "{\"sync_prefs\":{\"" TID_A_STR "\":false}}";
        sync_prefs_apply_from_queue(&g_fs, setup_json);

        WHEN("response has no sync_prefs key") {
            const char* no_prefs = "{\"pending\":[],\"hint\":null}";
            sync_prefs_apply_from_queue(&g_fs, no_prefs);
            THEN("map unchanged — TID_A still disabled") {
                REQUIRE(sync_prefs_is_enabled(TID_A) == false);
            }
        }
    }
}

// ── Unit: load/save round-trip ────────────────────────────────────────────────

SCENARIO("sync_prefs: persist to SD and reload", "[sync_prefs]") {
    GIVEN("prefs applied and saved") {
        setup();
        const char* json = "{\"sync_prefs\":{\"" TID_A_STR "\":false,\"" TID_B_STR "\":true}}";
        sync_prefs_apply_from_queue(&g_fs, json);
        REQUIRE(sync_prefs_count() == 2);
        REQUIRE(sync_prefs_is_enabled(TID_A) == false);

        WHEN("map is reset and reloaded from SD") {
            sync_prefs_reset();
            REQUIRE(sync_prefs_count() == 0);
            sync_prefs_load(&g_fs);
            THEN("TID_A is still disabled") {
                REQUIRE(sync_prefs_is_enabled(TID_A) == false);
            }
            THEN("TID_B is still enabled") {
                REQUIRE(sync_prefs_is_enabled(TID_B) == true);
            }
        }
    }
}

SCENARIO("sync_prefs: load from absent file leaves map empty", "[sync_prefs]") {
    GIVEN("no prefs file on SD") {
        setup();
        sync_prefs_load(&g_fs);
        THEN("count is 0") { REQUIRE(sync_prefs_count() == 0); }
        THEN("any title is default-allowed") {
            REQUIRE(sync_prefs_is_enabled(TID_A) == true);
        }
    }
}

// ── Game name cache ───────────────────────────────────────────────────────────

SCENARIO("sync_prefs: game name cache populated from queue response", "[sync_prefs]") {
    GIVEN("queue response with game_names field") {
        setup();
        const char* json = "{\"pending\":[],\"sync_prefs\":{},"
                           "\"game_names\":{\"" TID_A_STR "\":\"New Pokemon Snap\","
                           "\"" TID_B_STR "\":\"Zelda: Link's Awakening\"}}";
        sync_prefs_apply_game_names(json);

        THEN("TID_A name found") {
            char out[48] = {0};
            REQUIRE(sync_prefs_get_game_name(TID_A, out, sizeof(out)) == true);
            REQUIRE(std::string(out) == "New Pokemon Snap");
        }
        THEN("TID_B name found") {
            char out[48] = {0};
            REQUIRE(sync_prefs_get_game_name(TID_B, out, sizeof(out)) == true);
        }
        THEN("unknown title returns false") {
            char out[48] = {0};
            REQUIRE(sync_prefs_get_game_name(0xDEADBEEFDEADBEEFULL, out, sizeof(out)) == false);
        }
    }
}

SCENARIO("sync_prefs: missing game_names field is a no-op", "[sync_prefs]") {
    GIVEN("queue with no game_names key") {
        setup();
        // First set some names
        const char* with_names = "{\"game_names\":{\"" TID_A_STR "\":\"Name\"}}";
        sync_prefs_apply_game_names(with_names);
        REQUIRE(sync_prefs_get_game_name(TID_A, nullptr, 0) == true);

        // Queue without game_names — should be no-op (names persist)
        const char* without = "{\"pending\":[],\"hint\":null}";
        sync_prefs_apply_game_names(without);
        THEN("cache still has names from previous call") {
            char out[48] = {0};
            REQUIRE(sync_prefs_get_game_name(TID_A, out, sizeof(out)) == true);
        }
    }
}

SCENARIO("sync_prefs: empty game_names object clears old names", "[sync_prefs]") {
    GIVEN("names cached then overwritten with empty object") {
        setup();
        const char* with_names = "{\"game_names\":{\"" TID_A_STR "\":\"Name\"}}";
        sync_prefs_apply_game_names(with_names);

        const char* empty = "{\"game_names\":{}}";
        sync_prefs_apply_game_names(empty);
        THEN("TID_A name no longer found") {
            char out[48] = {0};
            REQUIRE(sync_prefs_get_game_name(TID_A, out, sizeof(out)) == false);
        }
    }
}

// ── FSM: upload suppressed when title disabled ────────────────────────────────

#include "mock/fsm_deps.hpp"

SCENARIO("FSM: upload skipped when title disabled", "[sync_prefs][fsm]") {
    GIVEN("FSM IDLE, outbound pending, TID_A disabled") {
        setup();
        g_fs.impl = &g_vfs;
        strncpy(g_config.server_host,  "testserver",    sizeof(g_config.server_host)  - 1);
        strncpy(g_config.device_token, "sk_device_test", sizeof(g_config.device_token) - 1);
        mock_reset();
        fsm_init();

        const char* json = "{\"sync_prefs\":{\"" TID_A_STR "\":false}}";
        sync_prefs_apply_from_queue(&g_fs, json);

        // Stage the outbound directory so build_snapshot would see it.
        g_vfs.make_dir("/switch/omnisave/outbound/" KEY_A);
        g_vfs.put_file("/switch/omnisave/outbound/" KEY_A "/save.zip", "data");

        InputSnapshot snap{};
        snap.has_outbound = true;
        strncpy(snap.outbound_key, KEY_A, sizeof(snap.outbound_key));

        WHEN("tick fires") {
            fsm_tick(&g_fs, &snap);
            THEN("FSM stays IDLE (upload suppressed)") {
                REQUIRE(fsm_get_state() == IDLE);
            }
            THEN("outbound directory is deleted") {
                REQUIRE(!g_vfs.has_dir("/switch/omnisave/outbound/" KEY_A));
            }
        }
    }
}

SCENARIO("FSM: upload proceeds when title enabled", "[sync_prefs][fsm]") {
    GIVEN("FSM IDLE, outbound pending, TID_A enabled (default)") {
        setup();
        g_fs.impl = &g_vfs;
        strncpy(g_config.server_host,  "testserver",    sizeof(g_config.server_host)  - 1);
        strncpy(g_config.device_token, "sk_device_test", sizeof(g_config.device_token) - 1);
        mock_reset();
        fsm_init();

        InputSnapshot snap{};
        snap.has_outbound = true;
        strncpy(snap.outbound_key, KEY_A, sizeof(snap.outbound_key));

        WHEN("tick fires") {
            fsm_tick(&g_fs, &snap);
            THEN("FSM transitions to UPLOADING") {
                REQUIRE(fsm_get_state() == UPLOADING);
            }
        }
    }
}

// ── FSM: inject suppressed when title disabled ────────────────────────────────

SCENARIO("FSM: inject skipped when title disabled", "[sync_prefs][fsm]") {
    GIVEN("FSM IDLE, inbound ready, game stopped, TID_A disabled") {
        setup();
        g_fs.impl = &g_vfs;
        strncpy(g_config.server_host,  "testserver",    sizeof(g_config.server_host)  - 1);
        strncpy(g_config.device_token, "sk_device_test", sizeof(g_config.device_token) - 1);
        mock_reset();
        fsm_init();

        const char* json = "{\"sync_prefs\":{\"" TID_A_STR "\":false}}";
        sync_prefs_apply_from_queue(&g_fs, json);

        InputSnapshot snap{};
        snap.has_inbound   = true;
        snap.game_running  = false;
        strncpy(snap.inbound_key, KEY_A_INBOUND, sizeof(snap.inbound_key));

        WHEN("tick fires") {
            fsm_tick(&g_fs, &snap);
            THEN("FSM stays IDLE (inject suppressed)") {
                REQUIRE(fsm_get_state() == IDLE);
            }
        }
    }
}

SCENARIO("FSM: inject proceeds when title enabled", "[sync_prefs][fsm]") {
    GIVEN("FSM IDLE, inbound ready, game stopped, TID_A enabled (default)") {
        setup();
        g_fs.impl = &g_vfs;
        strncpy(g_config.server_host,  "testserver",    sizeof(g_config.server_host)  - 1);
        strncpy(g_config.device_token, "sk_device_test", sizeof(g_config.device_token) - 1);
        mock_reset();
        fsm_init();

        InputSnapshot snap{};
        snap.has_inbound   = true;
        snap.game_running  = false;
        strncpy(snap.inbound_key, KEY_A_INBOUND, sizeof(snap.inbound_key));

        WHEN("tick fires") {
            fsm_tick(&g_fs, &snap);
            THEN("FSM transitions to INBOUND_READY") {
                REQUIRE(fsm_get_state() == INBOUND_READY);
            }
        }
    }
}
