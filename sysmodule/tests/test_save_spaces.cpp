#include <catch2/catch_test_macros.hpp>
#include <vector>
#include "mock/catalog_mock.hpp"
#include "omnisave.h"
#include "catalog.h"

// ── helpers ───────────────────────────────────────────────────────────────────

static FsSaveDataInfo make_save_in_space(u64 tid, FsSaveDataSpaceId space) {
    FsSaveDataInfo e{};
    e.application_id     = tid;
    e.save_data_type     = FsSaveDataType_Account;
    e.save_data_space_id = (u8)space;
    return e;
}

static void reset_space_mocks() {
    g_mock_save_infos.clear();
    g_mock_save_infos_by_space.clear();
    g_mock_spaces_requested.clear();
    g_mock_ns_records.clear();
}

static bool spaces_requested_include(FsSaveDataSpaceId s) {
    for (auto r : g_mock_spaces_requested)
        if (r == s) return true;
    return false;
}

// ── space request verification ────────────────────────────────────────────────

SCENARIO("catalog_enumerate requests User and SdUser spaces, not All", "[spaces]") {
    GIVEN("an empty save list") {
        reset_space_mocks();

        WHEN("catalog_enumerate is called") {
            CatalogEntry out[CATALOG_MAX];
            catalog_enumerate(out, CATALOG_MAX);

            THEN("FsSaveDataSpaceId_User was requested") {
                REQUIRE(spaces_requested_include(FsSaveDataSpaceId_User));
            }
            AND_THEN("FsSaveDataSpaceId_SdUser was requested") {
                REQUIRE(spaces_requested_include(FsSaveDataSpaceId_SdUser));
            }
            AND_THEN("FsSaveDataSpaceId_All was NOT requested") {
                REQUIRE_FALSE(spaces_requested_include(FsSaveDataSpaceId_All));
            }
        }
    }
}

// ── SdUser saves are found ────────────────────────────────────────────────────

SCENARIO("catalog_enumerate finds saves in SdUser space", "[spaces]") {
    GIVEN("a save exists only in SdUser space") {
        reset_space_mocks();
        const u64 TID = 0x0100000000001000ULL;
        g_mock_save_infos_by_space[FsSaveDataSpaceId_SdUser] = {
            make_save_in_space(TID, FsSaveDataSpaceId_SdUser),
        };

        WHEN("catalog_enumerate is called") {
            CatalogEntry out[CATALOG_MAX];
            int n = catalog_enumerate(out, CATALOG_MAX);

            THEN("the SdUser save appears in catalog") {
                REQUIRE(n == 1);
                REQUIRE(out[0].title_id == TID);
            }
        }
    }
}

SCENARIO("catalog_enumerate finds saves in User space", "[spaces]") {
    GIVEN("a save exists only in User space") {
        reset_space_mocks();
        const u64 TID = 0x0100000000002000ULL;
        g_mock_save_infos_by_space[FsSaveDataSpaceId_User] = {
            make_save_in_space(TID, FsSaveDataSpaceId_User),
        };

        WHEN("catalog_enumerate is called") {
            CatalogEntry out[CATALOG_MAX];
            int n = catalog_enumerate(out, CATALOG_MAX);

            THEN("the User save appears in catalog") {
                REQUIRE(n == 1);
                REQUIRE(out[0].title_id == TID);
            }
        }
    }
}

SCENARIO("catalog_enumerate finds saves across both User and SdUser", "[spaces]") {
    GIVEN("distinct saves in User and SdUser spaces") {
        reset_space_mocks();
        const u64 TID_USER   = 0x0100000000003000ULL;
        const u64 TID_SDUSER = 0x0100000000004000ULL;
        g_mock_save_infos_by_space[FsSaveDataSpaceId_User] = {
            make_save_in_space(TID_USER, FsSaveDataSpaceId_User),
        };
        g_mock_save_infos_by_space[FsSaveDataSpaceId_SdUser] = {
            make_save_in_space(TID_SDUSER, FsSaveDataSpaceId_SdUser),
        };

        WHEN("catalog_enumerate is called") {
            CatalogEntry out[CATALOG_MAX];
            int n = catalog_enumerate(out, CATALOG_MAX);

            THEN("both titles appear") { REQUIRE(n == 2); }
            THEN("User title is present") {
                bool found = false;
                for (int i = 0; i < n; i++) found |= (out[i].title_id == TID_USER);
                REQUIRE(found);
            }
            AND_THEN("SdUser title is present") {
                bool found = false;
                for (int i = 0; i < n; i++) found |= (out[i].title_id == TID_SDUSER);
                REQUIRE(found);
            }
        }
    }
}

SCENARIO("catalog_enumerate deduplicates a title present in both User and SdUser", "[spaces]") {
    GIVEN("the same title in both spaces (e.g. save migrated between spaces)") {
        reset_space_mocks();
        const u64 TID = 0x0100000000005000ULL;
        g_mock_save_infos_by_space[FsSaveDataSpaceId_User] = {
            make_save_in_space(TID, FsSaveDataSpaceId_User),
        };
        g_mock_save_infos_by_space[FsSaveDataSpaceId_SdUser] = {
            make_save_in_space(TID, FsSaveDataSpaceId_SdUser),
        };

        WHEN("catalog_enumerate is called") {
            CatalogEntry out[CATALOG_MAX];
            int n = catalog_enumerate(out, CATALOG_MAX);

            THEN("title appears exactly once") { REQUIRE(n == 1); }
            THEN("correct title_id") { REQUIRE(out[0].title_id == TID); }
        }
    }
}
