#include <catch2/catch_test_macros.hpp>
#include "status_parser.hpp"
#include <cstring>

static void mock_resolve(const char* /*tid*/, char* name, size_t sz) {
    snprintf(name, sz, "Test Game");
}

static StatusViewModel parse(const char* json, bool offline = false,
                              void (*res)(const char*, char*, size_t) = mock_resolve) {
    StatusViewModel vm;
    parse_status(json, offline, vm, res);
    return vm;
}

// ── Offline ───────────────────────────────────────────────────────────────────

TEST_CASE("offline → Sysmodule offline") {
    auto vm = parse("{}", true);
    REQUIRE(std::string(vm.label) == "Sysmodule offline");
    REQUIRE(vm.secondary[0] == '\0');
    REQUIRE(vm.progress[0]  == '\0');
}

// ── FSM states ────────────────────────────────────────────────────────────────

TEST_CASE("IDLE state") {
    auto vm = parse("{\"fsm_state\":\"IDLE\"}");
    REQUIRE(std::string(vm.label) == "Up to Date");
    REQUIRE(vm.secondary[0] == '\0');
    REQUIRE(vm.progress[0]  == '\0');
}

TEST_CASE("missing fsm_state defaults to IDLE") {
    auto vm = parse("{}");
    REQUIRE(std::string(vm.label) == "Up to Date");
}

TEST_CASE("RETRY_BACKOFF state") {
    auto vm = parse("{\"fsm_state\":\"RETRY_BACKOFF\"}");
    REQUIRE(std::string(vm.label)     == "Network Issue");
    REQUIRE(std::string(vm.secondary) == "Retrying Shortly...");
    REQUIRE(vm.progress[0] == '\0');
}

TEST_CASE("UPLOADING state with game name and progress") {
    const char* json = "{\"fsm_state\":\"UPLOADING\","
                       "\"title_id\":\"0100F2C0115B6000\","
                       "\"progress_pct\":42}";
    auto vm = parse(json);
    REQUIRE(std::string(vm.label)     == "Backing Up");
    REQUIRE(std::string(vm.secondary) == "Test Game");
    REQUIRE(std::string(vm.progress)  == "42%");
}

TEST_CASE("DOWNLOADING state with game name and progress") {
    const char* json = "{\"fsm_state\":\"DOWNLOADING\","
                       "\"title_id\":\"0100F2C0115B6000\","
                       "\"progress_pct\":7}";
    auto vm = parse(json);
    REQUIRE(std::string(vm.label)     == "Downloading");
    REQUIRE(std::string(vm.secondary) == "Test Game");
    REQUIRE(std::string(vm.progress)  == "7%");
}

TEST_CASE("UPLOADING with no resolver → secondary empty") {
    const char* json = "{\"fsm_state\":\"UPLOADING\","
                       "\"title_id\":\"0100F2C0115B6000\","
                       "\"progress_pct\":10}";
    auto vm = parse(json, false, nullptr);
    REQUIRE(std::string(vm.label)    == "Backing Up");
    REQUIRE(vm.secondary[0]          == '\0');
    REQUIRE(std::string(vm.progress) == "10%");
}

TEST_CASE("UPLOADING with negative progress_pct → progress empty") {
    const char* json = "{\"fsm_state\":\"UPLOADING\","
                       "\"title_id\":\"0100F2C0115B6000\","
                       "\"progress_pct\":-1}";
    auto vm = parse(json);
    REQUIRE(vm.progress[0] == '\0');
}

TEST_CASE("INBOUND_READY state") {
    const char* json = "{\"fsm_state\":\"INBOUND_READY\","
                       "\"title_id\":\"0100F2C0115B6000\"}";
    auto vm = parse(json);
    REQUIRE(std::string(vm.label)     == "Applying Save");
    REQUIRE(std::string(vm.secondary) == "Test Game");
    REQUIRE(vm.progress[0]            == '\0');
}

TEST_CASE("DELIVERING state") {
    const char* json = "{\"fsm_state\":\"DELIVERING\","
                       "\"title_id\":\"0100F2C0115B6000\"}";
    auto vm = parse(json);
    REQUIRE(std::string(vm.label) == "Applying Save");
}

TEST_CASE("unknown FSM state passes through as label") {
    auto vm = parse("{\"fsm_state\":\"SOME_FUTURE_STATE\"}");
    REQUIRE(std::string(vm.label) == "SOME_FUTURE_STATE");
    REQUIRE(vm.secondary[0] == '\0');
}

TEST_CASE("READ_ONLY is not a known FSM state — passes through as label") {
    // Sysmodule never emits READ_ONLY; ensure it doesn't crash or silently map.
    auto vm = parse("{\"fsm_state\":\"READ_ONLY\"}");
    REQUIRE(std::string(vm.label) == "READ_ONLY");
}
