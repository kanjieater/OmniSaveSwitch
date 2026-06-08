#include <catch2/catch_test_macros.hpp>
#include <rapidcheck.h>
#include <vector>
#include "mock/catalog_mock.hpp"
#include "omnisave.h"
#include "catalog.h"

// ── helpers ───────────────────────────────────────────────────────────────────

static FsSaveDataInfo make_save(u64 tid, u8 type = FsSaveDataType_Account) {
    FsSaveDataInfo e{};
    e.application_id  = tid;
    e.save_data_type  = type;
    return e;
}

static NsApplicationRecord make_ns(u64 tid) {
    NsApplicationRecord r{};
    r.application_id = tid;
    return r;
}

static void reset_mocks() {
    g_mock_save_infos.clear();
    g_mock_save_infos_by_space.clear();
    g_mock_spaces_requested.clear();
    g_mock_ns_records.clear();
}

// ── tests ─────────────────────────────────────────────────────────────────────

SCENARIO("catalog from save data only", "[catalog]") {
    GIVEN("three games with saves, one duplicate profile") {
        reset_mocks();
        g_mock_save_infos = {
            make_save(0xAAA0000000000000ULL),
            make_save(0xBBB0000000000000ULL),
            make_save(0xAAA0000000000000ULL),  // duplicate — different profile
        };

        WHEN("catalog_enumerate is called") {
            CatalogEntry out[CATALOG_MAX];
            int n = catalog_enumerate(out, CATALOG_MAX);

            THEN("returns 2 (deduped)") { REQUIRE(n == 2); }
            THEN("output is sorted ascending") {
                REQUIRE(out[0].title_id == 0xAAA0000000000000ULL);
                REQUIRE(out[1].title_id == 0xBBB0000000000000ULL);
            }
        }
    }

    GIVEN("a non-Account save entry is present") {
        reset_mocks();
        g_mock_save_infos = {
            make_save(0xAAA0000000000000ULL, 0),  // type 0 = System, not Account
            make_save(0xBBB0000000000000ULL),
        };

        WHEN("catalog_enumerate is called") {
            CatalogEntry out[CATALOG_MAX];
            int n = catalog_enumerate(out, CATALOG_MAX);

            THEN("non-Account entry is filtered") { REQUIRE(n == 1); }
            THEN("only Account game appears") {
                REQUIRE(out[0].title_id == 0xBBB0000000000000ULL);
            }
        }
    }
}

SCENARIO("catalog from NS app list only (no saves)", "[catalog]") {
    GIVEN("two games installed, neither has saves") {
        reset_mocks();
        g_mock_ns_records = {
            make_ns(0x0100000000010000ULL),
            make_ns(0x0100000000020000ULL),
        };

        WHEN("catalog_enumerate is called") {
            CatalogEntry out[CATALOG_MAX];
            int n = catalog_enumerate(out, CATALOG_MAX);

            THEN("both installed titles appear") { REQUIRE(n == 2); }
        }
    }
}

SCENARIO("catalog merges saves and NS with dedup", "[catalog]") {
    GIVEN("one game with saves, one installed-only, one in both") {
        reset_mocks();
        const u64 TID_SAVE_ONLY     = 0x0100000000000001ULL;
        const u64 TID_INSTALLED_ONLY = 0x0100000000000002ULL;
        const u64 TID_BOTH           = 0x0100000000000003ULL;

        g_mock_save_infos = {
            make_save(TID_SAVE_ONLY),
            make_save(TID_BOTH),
        };
        g_mock_ns_records = {
            make_ns(TID_INSTALLED_ONLY),
            make_ns(TID_BOTH),  // duplicate — already in saves
        };

        WHEN("catalog_enumerate is called") {
            CatalogEntry out[CATALOG_MAX];
            int n = catalog_enumerate(out, CATALOG_MAX);

            THEN("returns 3 unique titles") { REQUIRE(n == 3); }
            THEN("all three titles present") {
                bool found1 = false, found2 = false, found3 = false;
                for (int i = 0; i < n; i++) {
                    if (out[i].title_id == TID_SAVE_ONLY)      found1 = true;
                    if (out[i].title_id == TID_INSTALLED_ONLY) found2 = true;
                    if (out[i].title_id == TID_BOTH)           found3 = true;
                }
                REQUIRE(found1);
                REQUIRE(found2);
                REQUIRE(found3);
            }
        }
    }

    GIVEN("empty saves and empty NS list") {
        reset_mocks();

        WHEN("catalog_enumerate is called") {
            CatalogEntry out[CATALOG_MAX];
            int n = catalog_enumerate(out, CATALOG_MAX);
            THEN("returns 0") { REQUIRE(n == 0); }
        }
    }
}

SCENARIO("catalog is capped at max", "[catalog]") {
    GIVEN("more NS entries than CATALOG_MAX") {
        reset_mocks();
        for (int i = 0; i < CATALOG_MAX + 10; i++)
            g_mock_ns_records.push_back(make_ns((u64)(i + 1) << 32));

        WHEN("catalog_enumerate is called with limit CATALOG_MAX") {
            CatalogEntry out[CATALOG_MAX];
            int n = catalog_enumerate(out, CATALOG_MAX);
            THEN("count does not exceed CATALOG_MAX") { REQUIRE(n <= CATALOG_MAX); }
        }
    }
}

SCENARIO("catalog hash changes on install/uninstall", "[catalog]") {
    GIVEN("initial catalog with one game") {
        reset_mocks();
        g_mock_ns_records = { make_ns(0x0100000000000001ULL) };
        CatalogEntry out[CATALOG_MAX];
        int n1 = catalog_enumerate(out, CATALOG_MAX);
        u64 h1 = catalog_hash(out, n1);

        WHEN("a second game is installed") {
            reset_mocks();
            g_mock_ns_records = {
                make_ns(0x0100000000000001ULL),
                make_ns(0x0100000000000002ULL),
            };
            int n2 = catalog_enumerate(out, CATALOG_MAX);
            u64 h2 = catalog_hash(out, n2);

            THEN("hash differs") { REQUIRE(h1 != h2); }
        }

        WHEN("the game is uninstalled") {
            reset_mocks();
            int n2 = catalog_enumerate(out, CATALOG_MAX);
            u64 h2 = catalog_hash(out, n2);

            THEN("hash differs") { REQUIRE(h1 != h2); }
            THEN("count is 0") { REQUIRE(n2 == 0); }
        }
    }
}

SCENARIO("catalog_build_json produces valid array", "[catalog]") {
    GIVEN("two entries") {
        CatalogEntry entries[2];
        entries[0].title_id = 0x0100F2C0115B6000ULL;
        entries[1].title_id = 0x0100ABE01031C000ULL;

        WHEN("catalog_build_json is called") {
            char buf[256];
            int len = catalog_build_json(entries, 2, buf, (int)sizeof(buf));

            THEN("starts with [") { REQUIRE(buf[0] == '['); }
            THEN("ends with ]") { REQUIRE(buf[len - 1] == ']'); }
            THEN("contains first title in uppercase hex") {
                REQUIRE(std::string(buf).find("0100F2C0115B6000") != std::string::npos);
            }
            THEN("contains second title in uppercase hex") {
                REQUIRE(std::string(buf).find("0100ABE01031C000") != std::string::npos);
            }
        }
    }
}

SCENARIO("NS entries with application_id=0 are filtered", "[catalog]") {
    GIVEN("one valid NS entry and one zero-id entry") {
        reset_mocks();
        g_mock_ns_records = {
            make_ns(0),
            make_ns(0x0100000000000001ULL),
        };

        WHEN("catalog_enumerate is called") {
            CatalogEntry out[CATALOG_MAX];
            int n = catalog_enumerate(out, CATALOG_MAX);
            THEN("zero-id entry is filtered") { REQUIRE(n == 1); }
        }
    }
}
