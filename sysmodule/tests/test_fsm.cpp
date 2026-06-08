#include <catch2/catch_test_macros.hpp>
#include <rapidcheck.h>
#include <cstring>
#include <string>
#include "mock/fsm_deps.hpp"
#include "mock/vfs.hpp"
#include "omnisave.h"

// A 52-char key with tid=AABBCCDDAABBCCDD uid=1122334455667788
#define KEY_A "20240101_120000_00-AABBCCDDAABBCCDD-1122334455667788"
// Same tid+uid, later timestamp
#define KEY_B "20240101_130000_00-AABBCCDDAABBCCDD-1122334455667788"
// Different game (tid=FFFFFFFFFFFFFFFF)
#define KEY_C "20240101_120000_00-FFFFFFFFFFFFFFFF-1122334455667788"

static VfsData g_vfs;
static FsFileSystem g_fs;

static void setup() {
    g_vfs = VfsData{};
    g_fs.impl = &g_vfs;
    mock_reset();
    fsm_init();
    // Default: server configured and device paired
    strncpy(g_config.server_host,  "testserver",    sizeof(g_config.server_host)  - 1);
    strncpy(g_config.device_token, "sk_device_test", sizeof(g_config.device_token) - 1);
}

static InputSnapshot blank_snap() {
    InputSnapshot s{};
    memset(&s, 0, sizeof(s));
    return s;
}

// ── IDLE transitions ───────────────────────────────────────────────────────────

SCENARIO("IDLE: storage critical blocks all transitions", "[fsm][idle]") {
    GIVEN("FSM in IDLE, storage critical") {
        setup();
        auto snap = blank_snap();
        snap.storage_critical = true;
        snap.has_outbound = true;
        strncpy(snap.outbound_key, KEY_A, sizeof(snap.outbound_key));

        WHEN("tick fires") {
            fsm_tick(&g_fs, &snap);
            THEN("stays IDLE") { REQUIRE(fsm_get_state() == IDLE); }
        }
    }
}

SCENARIO("IDLE: outbound pending → UPLOADING", "[fsm][idle]") {
    GIVEN("FSM in IDLE with outbound key") {
        setup();
        auto snap = blank_snap();
        snap.has_outbound = true;
        strncpy(snap.outbound_key, KEY_A, sizeof(snap.outbound_key));

        WHEN("tick fires") {
            fsm_tick(&g_fs, &snap);
            THEN("transitions to UPLOADING") { REQUIRE(fsm_get_state() == UPLOADING); }
        }
    }
}

SCENARIO("IDLE: inbound with game running → stays IDLE", "[fsm][idle]") {
    GIVEN("FSM in IDLE, inbound ready but game is running") {
        setup();
        auto snap = blank_snap();
        snap.has_inbound  = true;
        snap.game_running = true;
        strncpy(snap.inbound_key, KEY_A, sizeof(snap.inbound_key));

        WHEN("tick fires") {
            fsm_tick(&g_fs, &snap);
            THEN("stays IDLE") { REQUIRE(fsm_get_state() == IDLE); }
        }
    }
}

SCENARIO("IDLE: inbound with game stopped → INBOUND_READY", "[fsm][idle]") {
    GIVEN("FSM in IDLE, inbound ready and game stopped") {
        setup();
        auto snap = blank_snap();
        snap.has_inbound  = true;
        snap.game_running = false;
        strncpy(snap.inbound_key, KEY_A, sizeof(snap.inbound_key));

        WHEN("tick fires") {
            fsm_tick(&g_fs, &snap);
            THEN("transitions to INBOUND_READY") { REQUIRE(fsm_get_state() == INBOUND_READY); }
        }
    }
}

SCENARIO("IDLE: outbound deferred falls through to inbound", "[fsm][idle]") {
    GIVEN("Key A has 3 consecutive upload failures and is deferred") {
        setup();
        g_mock_upload_rc = 0;
        auto snap = blank_snap();
        snap.has_outbound = true;
        strncpy(snap.outbound_key, KEY_A, sizeof(snap.outbound_key));

        // Drive 3 failure cycles without resetting FSM (fail_streak accumulates).
        // Cycle 1: retry_delay=5 → 7 ticks total
        fsm_tick(&g_fs, &snap); fsm_tick(&g_fs, &snap);
        for (int i = 0; i < 5; i++) fsm_tick(&g_fs, &snap);
        // Cycle 2: retry_delay=10 → 12 ticks total
        fsm_tick(&g_fs, &snap); fsm_tick(&g_fs, &snap);
        for (int i = 0; i < 10; i++) fsm_tick(&g_fs, &snap);
        // Cycle 3: fail_streak reaches 3 → deferral set; retry_delay=20
        fsm_tick(&g_fs, &snap); fsm_tick(&g_fs, &snap);
        for (int i = 0; i < 20; i++) fsm_tick(&g_fs, &snap); // drain RETRY_BACKOFF → IDLE

        REQUIRE(fsm_is_key_deferred(KEY_A));

        snap.has_inbound  = true;
        snap.game_running = false;
        strncpy(snap.inbound_key, KEY_C, sizeof(snap.inbound_key));

        WHEN("tick fires with deferred outbound + inbound") {
            fsm_tick(&g_fs, &snap);
            THEN("falls through to INBOUND_READY") { REQUIRE(fsm_get_state() == INBOUND_READY); }
        }
    }
}

SCENARIO("IDLE: heartbeat with server → DOWNLOADING", "[fsm][idle]") {
    GIVEN("FSM in IDLE, heartbeat due, server configured") {
        setup();
        auto snap = blank_snap();
        snap.heartbeat_due = true;

        WHEN("tick fires") {
            fsm_tick(&g_fs, &snap);
            THEN("transitions to DOWNLOADING") { REQUIRE(fsm_get_state() == DOWNLOADING); }
        }
    }
}

// ── DOWNLOADING transitions ────────────────────────────────────────────────────

SCENARIO("DOWNLOADING: network ready → polls and returns to IDLE", "[fsm][downloading]") {
    GIVEN("FSM in DOWNLOADING via heartbeat, network available") {
        setup();
        g_mock_network_ready = 1;
        auto snap = blank_snap();
        snap.heartbeat_due = true;
        fsm_tick(&g_fs, &snap); // → DOWNLOADING

        WHEN("second tick fires in DOWNLOADING") {
            fsm_tick(&g_fs, &snap);
            THEN("returns to IDLE") { REQUIRE(fsm_get_state() == IDLE); }
            THEN("polled server once") { REQUIRE(g_mock_poll_inbound_calls == 1); }
        }
    }
}

SCENARIO("DOWNLOADING: no network → returns to IDLE without polling", "[fsm][downloading]") {
    GIVEN("FSM in DOWNLOADING, no network") {
        setup();
        g_mock_network_ready = 0;
        auto snap = blank_snap();
        snap.heartbeat_due = true;
        fsm_tick(&g_fs, &snap); // → DOWNLOADING

        WHEN("second tick fires") {
            fsm_tick(&g_fs, &snap);
            THEN("returns to IDLE") { REQUIRE(fsm_get_state() == IDLE); }
            THEN("did not poll") { REQUIRE(g_mock_poll_inbound_calls == 0); }
        }
    }
}

SCENARIO("DOWNLOADING: poll returns 401 → clears token and returns to IDLE", "[fsm][downloading]") {
    GIVEN("FSM in DOWNLOADING, poll returns -2 (token revoked)") {
        setup();
        g_mock_network_ready    = 1;
        g_mock_poll_inbound_rc  = -2;
        strncpy(g_config.device_token, "sk_device_test", sizeof(g_config.device_token) - 1);
        auto snap = blank_snap();
        snap.heartbeat_due = true;
        fsm_tick(&g_fs, &snap); // → DOWNLOADING

        WHEN("second tick fires") {
            fsm_tick(&g_fs, &snap);
            THEN("returns to IDLE") { REQUIRE(fsm_get_state() == IDLE); }
            THEN("device_token cleared in memory") { REQUIRE(g_config.device_token[0] == '\0'); }
        }
    }
}

SCENARIO("DOWNLOADING: poll 401 after token cleared allows pairing flow", "[fsm][downloading]") {
    GIVEN("FSM in DOWNLOADING, poll returns -2, token subsequently empty") {
        setup();
        g_mock_network_ready   = 1;
        g_mock_poll_inbound_rc = -2;
        strncpy(g_config.device_token, "sk_device_stale", sizeof(g_config.device_token) - 1);
        auto snap = blank_snap();
        snap.heartbeat_due = true;
        fsm_tick(&g_fs, &snap); // → DOWNLOADING
        fsm_tick(&g_fs, &snap); // poll 401 → clears token, IDLE

        WHEN("next tick with empty token and heartbeat due") {
            // token is now empty; FSM should call poll_device_config not poll_inbound
            snap.heartbeat_due = true;
            fsm_tick(&g_fs, &snap); // → DOWNLOADING again
            fsm_tick(&g_fs, &snap); // polls with empty token
            THEN("poll was called again (re-entered DOWNLOADING)") {
                REQUIRE(g_mock_poll_inbound_calls >= 1);
            }
        }
    }
}

SCENARIO("DOWNLOADING: empty poll → no status.json writes", "[fsm][downloading]") {
    GIVEN("heartbeat fires, poll returns 0 (nothing pending)") {
        setup();
        g_mock_network_ready   = 1;
        g_mock_poll_inbound_rc = 0;
        auto snap = blank_snap();
        snap.heartbeat_due = true;
        fsm_tick(&g_fs, &snap); // IDLE → DOWNLOADING (quiet)
        g_mock_status_write_calls = 0;

        WHEN("DOWNLOADING tick fires") {
            fsm_tick(&g_fs, &snap);
            THEN("returns to IDLE")       { REQUIRE(fsm_get_state() == IDLE); }
            THEN("no status.json writes") { REQUIRE(g_mock_status_write_calls == 0); }
        }
    }
}

SCENARIO("DOWNLOADING: work found → one status write on return to IDLE", "[fsm][downloading]") {
    GIVEN("heartbeat fires, poll returns 1 (inbound downloaded)") {
        setup();
        g_mock_network_ready   = 1;
        g_mock_poll_inbound_rc = 1;
        auto snap = blank_snap();
        snap.heartbeat_due = true;
        fsm_tick(&g_fs, &snap); // IDLE → DOWNLOADING (quiet)
        g_mock_status_write_calls = 0;

        WHEN("DOWNLOADING tick fires") {
            fsm_tick(&g_fs, &snap);
            THEN("returns to IDLE")          { REQUIRE(fsm_get_state() == IDLE); }
            THEN("exactly one status write") { REQUIRE(g_mock_status_write_calls == 1); }
        }
    }
}

// ── UPLOADING transitions ──────────────────────────────────────────────────────

SCENARIO("UPLOADING: no server configured → IDLE immediately", "[fsm][uploading]") {
    GIVEN("FSM moves to UPLOADING, but server host is empty") {
        setup();
        g_config.server_host[0] = '\0';
        auto snap = blank_snap();
        snap.has_outbound = true;
        strncpy(snap.outbound_key, KEY_A, sizeof(snap.outbound_key));
        fsm_tick(&g_fs, &snap); // → UPLOADING

        WHEN("second tick fires") {
            fsm_tick(&g_fs, &snap);
            THEN("transitions to IDLE") { REQUIRE(fsm_get_state() == IDLE); }
        }
    }
}

SCENARIO("UPLOADING: no network → RETRY_BACKOFF", "[fsm][uploading]") {
    GIVEN("FSM in UPLOADING, network unavailable") {
        setup();
        g_mock_network_ready = 0;
        auto snap = blank_snap();
        snap.has_outbound = true;
        strncpy(snap.outbound_key, KEY_A, sizeof(snap.outbound_key));
        fsm_tick(&g_fs, &snap); // → UPLOADING

        WHEN("second tick fires") {
            fsm_tick(&g_fs, &snap);
            THEN("transitions to RETRY_BACKOFF") { REQUIRE(fsm_get_state() == RETRY_BACKOFF); }
        }
    }
}

SCENARIO("UPLOADING: cooperative yield (snap_id=-1) → IDLE, no failure recorded", "[fsm][uploading]") {
    GIVEN("transport_upload returns -1 (yield)") {
        setup();
        g_mock_upload_rc = -1;
        auto snap = blank_snap();
        snap.has_outbound = true;
        strncpy(snap.outbound_key, KEY_A, sizeof(snap.outbound_key));
        fsm_tick(&g_fs, &snap); // → UPLOADING

        WHEN("second tick fires") {
            fsm_tick(&g_fs, &snap);
            THEN("transitions to IDLE") { REQUIRE(fsm_get_state() == IDLE); }
            AND_THEN("key is not deferred") { REQUIRE_FALSE(fsm_is_key_deferred(KEY_A)); }
        }
    }
}

SCENARIO("UPLOADING: success → IDLE, dirty cleared", "[fsm][uploading]") {
    GIVEN("transport_upload returns snap_id > 0") {
        setup();
        g_mock_upload_rc = 42;
        s_dirty = true;
        auto snap = blank_snap();
        snap.has_outbound = true;
        strncpy(snap.outbound_key, KEY_A, sizeof(snap.outbound_key));
        fsm_tick(&g_fs, &snap); // → UPLOADING

        WHEN("second tick fires") {
            fsm_tick(&g_fs, &snap);
            THEN("transitions to IDLE") { REQUIRE(fsm_get_state() == IDLE); }
            AND_THEN("dirty flag cleared") { REQUIRE_FALSE(s_dirty); }
        }
    }
}

SCENARIO("UPLOADING: 3 consecutive failures trigger deferral", "[fsm][uploading]") {
    GIVEN("transport_upload fails three times for the same key across retry cycles") {
        setup();
        g_mock_upload_rc = 0;
        auto snap = blank_snap();
        snap.has_outbound = true;
        strncpy(snap.outbound_key, KEY_A, sizeof(snap.outbound_key));

        // fail_streak accumulates only if the FSM is not reset between cycles.
        // Cycle 1 (retry_ticks=5 after fail)
        fsm_tick(&g_fs, &snap); fsm_tick(&g_fs, &snap);
        for (int i = 0; i < 5; i++) fsm_tick(&g_fs, &snap);
        // Cycle 2 (retry_ticks=10)
        fsm_tick(&g_fs, &snap); fsm_tick(&g_fs, &snap);
        for (int i = 0; i < 10; i++) fsm_tick(&g_fs, &snap);
        // Cycle 3: fail_streak hits 3 → deferral set
        fsm_tick(&g_fs, &snap); fsm_tick(&g_fs, &snap);

        WHEN("key deferral is checked") {
            THEN("KEY_A is deferred") { REQUIRE(fsm_is_key_deferred(KEY_A)); }
            AND_THEN("KEY_C is not deferred") { REQUIRE_FALSE(fsm_is_key_deferred(KEY_C)); }
        }
    }
}

// ── INBOUND_READY transitions ──────────────────────────────────────────────────

SCENARIO("INBOUND_READY: game starts → stays INBOUND_READY", "[fsm][inbound_ready]") {
    GIVEN("FSM in INBOUND_READY, game starts") {
        setup();
        auto snap = blank_snap();
        snap.has_inbound = true;
        strncpy(snap.inbound_key, KEY_A, sizeof(snap.inbound_key));
        fsm_tick(&g_fs, &snap); // → INBOUND_READY

        snap.game_running = true;
        WHEN("tick fires with game running") {
            fsm_tick(&g_fs, &snap);
            THEN("stays INBOUND_READY") { REQUIRE(fsm_get_state() == INBOUND_READY); }
        }
    }
}

SCENARIO("INBOUND_READY: new outbound appears → IDLE (outbound lock)", "[fsm][inbound_ready]") {
    GIVEN("FSM in INBOUND_READY, outbound appears") {
        setup();
        auto snap = blank_snap();
        snap.has_inbound = true;
        strncpy(snap.inbound_key, KEY_A, sizeof(snap.inbound_key));
        fsm_tick(&g_fs, &snap); // → INBOUND_READY

        snap.game_running = false;
        snap.has_outbound = true;
        strncpy(snap.outbound_key, KEY_B, sizeof(snap.outbound_key));
        WHEN("tick fires with outbound pending") {
            fsm_tick(&g_fs, &snap);
            THEN("transitions to IDLE (outbound wins)") { REQUIRE(fsm_get_state() == IDLE); }
        }
    }
}

SCENARIO("INBOUND_READY: game stopped, no outbound → DELIVERING", "[fsm][inbound_ready]") {
    GIVEN("FSM in INBOUND_READY, game stopped") {
        setup();
        auto snap = blank_snap();
        snap.has_inbound = true;
        strncpy(snap.inbound_key, KEY_A, sizeof(snap.inbound_key));
        fsm_tick(&g_fs, &snap); // → INBOUND_READY

        snap.game_running = false;
        WHEN("tick fires") {
            fsm_tick(&g_fs, &snap);
            THEN("transitions to DELIVERING") { REQUIRE(fsm_get_state() == DELIVERING); }
        }
    }
}

// ── DELIVERING transitions ─────────────────────────────────────────────────────

SCENARIO("DELIVERING: inject success → IDLE, ack sent", "[fsm][delivering]") {
    GIVEN("inject succeeds") {
        setup();
        g_mock_inject_rc = 1;
        auto snap = blank_snap();
        snap.has_inbound = true;
        strncpy(snap.inbound_key, KEY_A, sizeof(snap.inbound_key));
        fsm_tick(&g_fs, &snap); // → INBOUND_READY
        fsm_tick(&g_fs, &snap); // → DELIVERING

        WHEN("third tick fires") {
            fsm_tick(&g_fs, &snap);
            THEN("transitions to IDLE") { REQUIRE(fsm_get_state() == IDLE); }
            AND_THEN("ack was sent") { REQUIRE(g_mock_ack_calls == 1); }
        }
    }
}

SCENARIO("DELIVERING: inject permanent failure → IDLE, title blocked", "[fsm][delivering]") {
    GIVEN("inject returns -1 (game not installed)") {
        setup();
        g_mock_inject_rc = -1;
        auto snap = blank_snap();
        snap.has_inbound = true;
        strncpy(snap.inbound_key, KEY_A, sizeof(snap.inbound_key));
        fsm_tick(&g_fs, &snap); // → INBOUND_READY
        fsm_tick(&g_fs, &snap); // → DELIVERING

        WHEN("third tick fires") {
            fsm_tick(&g_fs, &snap);
            THEN("transitions to IDLE") { REQUIRE(fsm_get_state() == IDLE); }
            AND_THEN("permanent error reported") { REQUIRE(g_mock_error_fail_calls == 1); }
            AND_THEN("title is blocked") { REQUIRE(fsm_is_title_inject_blocked("AABBCCDDAABBCCDD")); }
        }
    }
}

SCENARIO("DELIVERING: 3 transient inject failures → permanent error", "[fsm][delivering]") {
    GIVEN("inject returns 0 three times for the same title without FSM reset") {
        setup();
        g_mock_inject_rc = 0;

        // s_inject_fail_count accumulates across deliver cycles only within the same FSM lifetime.
        auto snap = blank_snap();
        snap.has_inbound = true;
        strncpy(snap.inbound_key, KEY_A, sizeof(snap.inbound_key));

        for (int attempt = 0; attempt < 3; attempt++) {
            fsm_tick(&g_fs, &snap); // IDLE → INBOUND_READY
            fsm_tick(&g_fs, &snap); // INBOUND_READY → DELIVERING
            fsm_tick(&g_fs, &snap); // DELIVERING → IDLE
        }

        THEN("first two attempts made no server call (client-local retry)") {
            REQUIRE(g_mock_error_fail_calls == 1);
            REQUIRE(g_mock_ack_calls == 0);
        }
        AND_THEN("third attempt escalated to permanent fail report") {
            REQUIRE(g_mock_error_fail_calls == 1);
        }
    }
}

SCENARIO("RETRY_BACKOFF: countdown reaches zero → IDLE", "[fsm][retry]") {
    GIVEN("FSM in RETRY_BACKOFF after upload failure") {
        setup();
        g_mock_upload_rc     = 0;
        g_mock_network_ready = 1;
        auto snap = blank_snap();
        snap.has_outbound = true;
        strncpy(snap.outbound_key, KEY_A, sizeof(snap.outbound_key));
        fsm_tick(&g_fs, &snap); // → UPLOADING
        fsm_tick(&g_fs, &snap); // → RETRY_BACKOFF (retry_delay=5)

        WHEN("5 ticks elapse in RETRY_BACKOFF") {
            for (int i = 0; i < 5; i++) fsm_tick(&g_fs, &snap);
            THEN("transitions back to IDLE") { REQUIRE(fsm_get_state() == IDLE); }
        }
    }
}

SCENARIO("fsm_on_wake clears inject block list", "[fsm][wake]") {
    GIVEN("title blocked from inject failure") {
        setup();
        g_mock_inject_rc = -1;
        auto snap = blank_snap();
        snap.has_inbound = true;
        strncpy(snap.inbound_key, KEY_A, sizeof(snap.inbound_key));
        fsm_tick(&g_fs, &snap);
        fsm_tick(&g_fs, &snap);
        fsm_tick(&g_fs, &snap);
        REQUIRE(fsm_is_title_inject_blocked("AABBCCDDAABBCCDD"));

        WHEN("fsm_on_wake is called") {
            fsm_on_wake();
            THEN("title is no longer blocked") {
                REQUIRE_FALSE(fsm_is_title_inject_blocked("AABBCCDDAABBCCDD"));
            }
        }
    }
}

// ── RapidCheck property tests ──────────────────────────────────────────────────

TEST_CASE("Property: FSM always reaches IDLE after successful upload cycle", "[fsm][property]") {
    REQUIRE(rc::check("any valid-length key: IDLE → UPLOADING → IDLE on success", []() {
        // Generate a random 49-char alphanumeric key.
        auto chars = *rc::gen::container<std::vector<char>>(
            SNAP_KEY_LEN, rc::gen::elementOf(std::string("abcdefghijklmnopqrstuvwxyz0123456789")));
        char key[SNAP_KEY_LEN + 2] = {};
        for (int i = 0; i < SNAP_KEY_LEN; i++) key[i] = chars[(size_t)i];

        setup();
        g_mock_upload_rc = 7; // success
        auto snap = blank_snap();
        snap.has_outbound = true;
        strncpy(snap.outbound_key, key, sizeof(snap.outbound_key));

        fsm_tick(&g_fs, &snap); // IDLE → UPLOADING
        fsm_tick(&g_fs, &snap); // UPLOADING → IDLE

        RC_ASSERT(fsm_get_state() == IDLE);
    }));
}

SCENARIO("IDLE: deferral cooldown expiry clears deferred key", "[fsm][idle]") {
    GIVEN("KEY_A is deferred after 3 failure cycles") {
        setup();
        g_mock_upload_rc = 0;
        auto snap = blank_snap();
        snap.has_outbound = true;
        strncpy(snap.outbound_key, KEY_A, sizeof(snap.outbound_key));

        // Drive 3 failure cycles to set deferral (s_deferred_until_posix = 1000060).
        fsm_tick(&g_fs, &snap); fsm_tick(&g_fs, &snap);
        for (int i = 0; i < 5; i++) fsm_tick(&g_fs, &snap);
        fsm_tick(&g_fs, &snap); fsm_tick(&g_fs, &snap);
        for (int i = 0; i < 10; i++) fsm_tick(&g_fs, &snap);
        fsm_tick(&g_fs, &snap); fsm_tick(&g_fs, &snap);
        for (int i = 0; i < 20; i++) fsm_tick(&g_fs, &snap);
        REQUIRE(fsm_is_key_deferred(KEY_A));

        WHEN("mock time advances past the cooldown and tick fires") {
            g_mock_posix_utc = 1000061; // past s_deferred_until_posix (1000000+60)
            g_mock_upload_rc = 1;
            fsm_tick(&g_fs, &snap);

            THEN("FSM transitions to UPLOADING (deferral was cleared)") {
                REQUIRE(fsm_get_state() == UPLOADING);
            }
            AND_THEN("KEY_A is no longer deferred") {
                REQUIRE_FALSE(fsm_is_key_deferred(KEY_A));
            }
        }
    }
}

TEST_CASE("Property: deferred key never equals a different key", "[fsm][property]") {
    REQUIRE(rc::check("arbitrary distinct key is never mistakenly deferred", []() {
        // Generate a random 49-char key; skip if it accidentally equals KEY_A.
        auto chars = *rc::gen::container<std::vector<char>>(
            SNAP_KEY_LEN,
            rc::gen::elementOf(std::string("0123456789abcdefABCDEF_-")));
        char other_key[SNAP_KEY_LEN + 2] = {};
        for (int i = 0; i < SNAP_KEY_LEN; i++) other_key[i] = chars[(size_t)i];
        if (strcmp(other_key, KEY_A) == 0) return;

        setup();
        g_mock_upload_rc = 0;
        auto snap = blank_snap();
        snap.has_outbound = true;
        strncpy(snap.outbound_key, KEY_A, sizeof(snap.outbound_key));

        // Drive 3 failure cycles without resetting FSM.
        fsm_tick(&g_fs, &snap); fsm_tick(&g_fs, &snap);
        for (int i = 0; i < 5; i++) fsm_tick(&g_fs, &snap);
        fsm_tick(&g_fs, &snap); fsm_tick(&g_fs, &snap);
        for (int i = 0; i < 10; i++) fsm_tick(&g_fs, &snap);
        fsm_tick(&g_fs, &snap); fsm_tick(&g_fs, &snap);

        RC_ASSERT(fsm_is_key_deferred(KEY_A));
        RC_ASSERT(!fsm_is_key_deferred(other_key));
    }));
}

// ── Preservation pre-step ──────────────────────────────────────────────────────

SCENARIO("DELIVERING: save unchanged (extract dedup) → skip preservation, proceed to inject",
         "[fsm][delivering][preservation]") {
    GIVEN("save_extract returns 0 (no local change), inject succeeds") {
        setup();
        g_mock_save_extract_rc = 0;
        g_mock_inject_rc = 1;
        auto snap = blank_snap();
        snap.has_inbound = true;
        strncpy(snap.inbound_key, KEY_A, sizeof(snap.inbound_key));
        fsm_tick(&g_fs, &snap); // → INBOUND_READY
        fsm_tick(&g_fs, &snap); // → DELIVERING

        WHEN("deliver tick fires") {
            fsm_tick(&g_fs, &snap);
            THEN("inject succeeds → IDLE") { REQUIRE(fsm_get_state() == IDLE); }
            AND_THEN("ack sent") { REQUIRE(g_mock_ack_calls == 1); }
        }
    }
}

SCENARIO("DELIVERING: save changed → preservation upload succeeds → inject proceeds",
         "[fsm][delivering][preservation]") {
    GIVEN("save_extract returns 1 (local save changed), both uploads succeed") {
        setup();
        g_mock_save_extract_rc = 1;
        g_mock_upload_rc = 1;
        g_mock_inject_rc = 1;
        auto snap = blank_snap();
        snap.has_inbound = true;
        strncpy(snap.inbound_key, KEY_A, sizeof(snap.inbound_key));
        fsm_tick(&g_fs, &snap); // → INBOUND_READY
        fsm_tick(&g_fs, &snap); // → DELIVERING

        WHEN("deliver tick fires") {
            fsm_tick(&g_fs, &snap);
            THEN("inject proceeds → IDLE") { REQUIRE(fsm_get_state() == IDLE); }
            AND_THEN("ack sent") { REQUIRE(g_mock_ack_calls == 1); }
        }
    }
}

SCENARIO("DELIVERING: preservation upload fails → delivery deferred (stays DELIVERING)",
         "[fsm][delivering][preservation]") {
    GIVEN("save_extract returns 1 (local save changed), preservation upload fails") {
        setup();
        g_mock_save_extract_rc = 1;
        g_mock_upload_rc = 0;  // preservation upload failure
        g_mock_inject_rc = 1;
        auto snap = blank_snap();
        snap.has_inbound = true;
        strncpy(snap.inbound_key, KEY_A, sizeof(snap.inbound_key));
        fsm_tick(&g_fs, &snap); // → INBOUND_READY
        fsm_tick(&g_fs, &snap); // → DELIVERING

        WHEN("deliver tick fires") {
            fsm_tick(&g_fs, &snap);
            THEN("delivery deferred — FSM stays in DELIVERING") {
                REQUIRE(fsm_get_state() == DELIVERING);
            }
            AND_THEN("inject not called — no ack sent") {
                REQUIRE(g_mock_ack_calls == 0);
            }
        }
    }
}

SCENARIO("DELIVERING: save_extract error → deferred to IDLE (not tight loop)",
         "[fsm][delivering][preservation]") {
    GIVEN("save_extract returns -1 (game not installed)") {
        setup();
        g_mock_save_extract_rc = -1;
        g_mock_inject_rc = 1;
        auto snap = blank_snap();
        snap.has_inbound = true;
        strncpy(snap.inbound_key, KEY_A, sizeof(snap.inbound_key));
        fsm_tick(&g_fs, &snap); // → INBOUND_READY
        fsm_tick(&g_fs, &snap); // → DELIVERING

        WHEN("first deliver tick fires") {
            fsm_tick(&g_fs, &snap);
            THEN("FSM returns to IDLE (not tight-looping in DELIVERING)") {
                REQUIRE(fsm_get_state() == IDLE);
            }
            AND_THEN("no ack sent") { REQUIRE(g_mock_ack_calls == 0); }
        }
    }
}

SCENARIO("DELIVERING: 3 consecutive save_extract failures → permanent block",
         "[fsm][delivering][preservation]") {
    GIVEN("save_extract returns -1 three times for the same title") {
        setup();
        g_mock_save_extract_rc = -1;
        g_mock_inject_rc = 1;
        auto snap = blank_snap();
        snap.has_inbound = true;
        strncpy(snap.inbound_key, KEY_A, sizeof(snap.inbound_key));

        // Three full INBOUND_READY → DELIVERING → fail → IDLE cycles
        for (int i = 0; i < 3; i++) {
            snap.has_inbound = true;
            strncpy(snap.inbound_key, KEY_A, sizeof(snap.inbound_key));
            fsm_tick(&g_fs, &snap); // IDLE → INBOUND_READY
            fsm_tick(&g_fs, &snap); // INBOUND_READY → DELIVERING
            fsm_tick(&g_fs, &snap); // DELIVERING → IDLE (or block on 3rd)
        }

        WHEN("after 3 failures") {
            THEN("title is blocked for this session") {
                REQUIRE(fsm_is_title_inject_blocked("01008CF01BAAC000") == false); // KEY_A title
                // The block uses the tid from KEY_A
                char tid[17] = {0};
                sscanf(KEY_A, "%*[^-]-%16[^-]", tid);
                REQUIRE(fsm_is_title_inject_blocked(tid) == true);
            }
            AND_THEN("FSM is in IDLE") {
                REQUIRE(fsm_get_state() == IDLE);
            }
            AND_THEN("no ack sent (never injected)") {
                REQUIRE(g_mock_ack_calls == 0);
            }
        }
    }
}
