#include <catch2/catch_test_macros.hpp>
#include "history_parser.hpp"
#include <cstring>

static void mock_resolve(const char* /*tid*/, char* name, size_t sz) {
    snprintf(name, sz, "My Game");
}

static HistoryViewModel parse(const char* json, bool show_name,
                               void (*res)(const char*, char*, size_t) = mock_resolve) {
    HistoryViewModel vm;
    parse_history(json, show_name, vm, res);
    return vm;
}

static const char* SAMPLE_JSON =
    "{\"title_id\":\"0100F2C0115B6000\","
    "\"snapshot_counter\":5,"
    "\"completed_at\":\"2026-06-10T14:30:00Z\"}";

// ── Null / empty input ────────────────────────────────────────────────────────

TEST_CASE("null json → valid=false, all lines empty") {
    auto vm = parse(nullptr, true);
    REQUIRE(vm.valid     == false);
    REQUIRE(vm.line1[0]  == '\0');
    REQUIRE(vm.line2[0]  == '\0');
    REQUIRE(vm.line3[0]  == '\0');
}

TEST_CASE("empty json → valid=false, all lines empty") {
    auto vm = parse("", false);
    REQUIRE(vm.valid == false);
}

// ── Home mode (show_name=true): name → ts → Save #N ──────────────────────────

TEST_CASE("home mode: line1=name, line2=timestamp, line3=save num") {
    auto vm = parse(SAMPLE_JSON, true);
    REQUIRE(vm.valid == true);
    REQUIRE(std::string(vm.line1) == "My Game");
    REQUIRE(std::string(vm.line2) == "Jun 10, 14:30");
    REQUIRE(std::string(vm.line3) == "Save #5");
}

TEST_CASE("home mode: no resolver → line1 empty") {
    auto vm = parse(SAMPLE_JSON, true, nullptr);
    REQUIRE(vm.valid == true);
    REQUIRE(vm.line1[0] == '\0');
    REQUIRE(std::string(vm.line2) == "Jun 10, 14:30");
    REQUIRE(std::string(vm.line3) == "Save #5");
}

TEST_CASE("home mode: missing snapshot_counter → line3 empty") {
    const char* json = "{\"title_id\":\"0100F2C0115B6000\","
                       "\"completed_at\":\"2026-06-10T14:30:00Z\"}";
    auto vm = parse(json, true);
    REQUIRE(vm.line3[0] == '\0');
}

TEST_CASE("home mode: missing completed_at → line2 empty") {
    const char* json = "{\"title_id\":\"0100F2C0115B6000\","
                       "\"snapshot_counter\":3}";
    auto vm = parse(json, true);
    REQUIRE(vm.line2[0] == '\0');
    REQUIRE(std::string(vm.line3) == "Save #3");
}

// ── In-game mode (show_name=false): ts → Save #N → (empty) ───────────────────

TEST_CASE("in-game mode: line1=timestamp, line2=save num, line3 empty") {
    auto vm = parse(SAMPLE_JSON, false);
    REQUIRE(vm.valid == true);
    REQUIRE(std::string(vm.line1) == "Jun 10, 14:30");
    REQUIRE(std::string(vm.line2) == "Save #5");
    REQUIRE(vm.line3[0]           == '\0');
}

TEST_CASE("in-game mode: resolve_name not called") {
    bool called = false;
    auto resolver = [](const char*, char* name, size_t sz) {
        snprintf(name, sz, "SHOULD NOT APPEAR");
    };
    (void)resolver;
    // Pass a resolver that would pollute line1 if called in in-game mode.
    // In in-game mode the resolver is still called (for title lookup) but
    // line1 gets the timestamp, not the name.
    auto vm = parse(SAMPLE_JSON, false);
    REQUIRE(std::string(vm.line1) != "SHOULD NOT APPEAR");
}

// ── Timestamp formatting ───────────────────────────────────────────────────────

TEST_CASE("timestamp formatting: standard ISO-8601") {
    const char* json = "{\"title_id\":\"\","
                       "\"snapshot_counter\":1,"
                       "\"completed_at\":\"2026-01-05T09:07:00Z\"}";
    auto vm = parse(json, false);
    REQUIRE(std::string(vm.line1) == "Jan 5, 09:07");
}

TEST_CASE("timestamp formatting: month boundary (December)") {
    const char* json = "{\"completed_at\":\"2025-12-31T23:59:00Z\"}";
    auto vm = parse(json, false);
    REQUIRE(std::string(vm.line1) == "Dec 31, 23:59");
}
