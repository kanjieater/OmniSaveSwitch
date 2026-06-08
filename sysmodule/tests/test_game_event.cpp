#include <catch2/catch_test_macros.hpp>
#include "omnisave.h"
#include "game_event.h"

// Helper: reset shared state between scenarios.
static void reset_event_slot() {
    game_event_drain();  // clear any leftover
}

// ── drain semantics ───────────────────────────────────────────────────────────

SCENARIO("drain returns 0 when nothing is pending", "[game_event]") {
    GIVEN("an empty event slot") {
        reset_event_slot();
        WHEN("drain is called") {
            uint64_t result = game_event_drain();
            THEN("returns 0") { REQUIRE(result == 0); }
        }
    }
}

SCENARIO("drain atomically reads and clears the pending title", "[game_event]") {
    GIVEN("a pending close event for title 0xDEADBEEFCAFEBABE") {
        reset_event_slot();
        bool ok = game_event_signal(0xDEADBEEFCAFEBABEULL);
        REQUIRE(ok);

        WHEN("drain is called once") {
            uint64_t first = game_event_drain();
            THEN("the title ID is returned") {
                REQUIRE(first == 0xDEADBEEFCAFEBABEULL);
            }
            AND_WHEN("drain is called a second time") {
                uint64_t second = game_event_drain();
                THEN("slot is now empty — returns 0") {
                    REQUIRE(second == 0);
                }
            }
        }
    }
}

// ── signal semantics (CAS) ────────────────────────────────────────────────────

SCENARIO("signal writes only when slot is empty", "[game_event]") {
    GIVEN("title A is already pending") {
        reset_event_slot();
        REQUIRE(game_event_signal(0xAAAAAAAAAAAAAAAAULL));

        WHEN("signal is called again for title B") {
            bool accepted = game_event_signal(0xBBBBBBBBBBBBBBBBULL);
            THEN("second signal is rejected") {
                REQUIRE_FALSE(accepted);
            }
            AND_THEN("original title A is still in the slot") {
                REQUIRE(game_event_drain() == 0xAAAAAAAAAAAAAAAAULL);
            }
        }
    }
}

SCENARIO("signal succeeds after drain clears the slot", "[game_event]") {
    GIVEN("title A is drained") {
        reset_event_slot();
        REQUIRE(game_event_signal(0x1111111111111111ULL));
        game_event_drain();

        WHEN("signal is called for title B") {
            bool accepted = game_event_signal(0x2222222222222222ULL);
            THEN("signal is accepted") { REQUIRE(accepted); }
            AND_THEN("drain returns title B") {
                REQUIRE(game_event_drain() == 0x2222222222222222ULL);
            }
        }
    }
}

// ── back-to-back close events ─────────────────────────────────────────────────

SCENARIO("sequential game closes are both observed when drained between signals", "[game_event]") {
    GIVEN("no pending event") {
        reset_event_slot();

        WHEN("title A closes, is drained, then title B closes and is drained") {
            bool sig_a   = game_event_signal(0x1000000000000001ULL);
            uint64_t a   = game_event_drain();
            bool sig_b   = game_event_signal(0x1000000000000002ULL);
            uint64_t b   = game_event_drain();

            THEN("both events are fully received in order") {
                REQUIRE(sig_a);
                REQUIRE(a == 0x1000000000000001ULL);
                REQUIRE(sig_b);
                REQUIRE(b == 0x1000000000000002ULL);
            }
        }
    }
}

SCENARIO("second close while first still pending is not injected", "[game_event]") {
    GIVEN("title A is pending and not yet drained") {
        reset_event_slot();
        REQUIRE(game_event_signal(0x1000000000000001ULL));

        WHEN("title B tries to signal before A is drained") {
            bool sig_b = game_event_signal(0x1000000000000002ULL);
            THEN("title B is rejected") { REQUIRE_FALSE(sig_b); }
            AND_THEN("drain still returns title A") {
                REQUIRE(game_event_drain() == 0x1000000000000001ULL);
            }
        }
    }
}
