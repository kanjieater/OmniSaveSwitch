#include <catch2/catch_test_macros.hpp>
#include "signal_writer.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

// Each test gets an isolated temp directory.
static fs::path make_tmp_dir() {
    auto base = fs::temp_directory_path() / "omnisave_overlay_tests";
    fs::create_directories(base / "signals");
    return base;
}

TEST_CASE("write_signal creates file with content '1'") {
    auto tmp = make_tmp_dir();
    auto path = (tmp / "signals" / "batch_backup.request").string();
    REQUIRE(write_signal(path.c_str()) == true);
    REQUIRE(fs::exists(path));
    std::ifstream f(path);
    REQUIRE(f.get() == '1');
}

TEST_CASE("write_signal per-game path") {
    auto tmp = make_tmp_dir();
    auto path = (tmp / "signals" / "backup_game_0100F2C0115B6000.request").string();
    REQUIRE(write_signal(path.c_str()) == true);
    std::ifstream f(path);
    REQUIRE(f.get() == '1');
}

TEST_CASE("write_signal is idempotent — second write succeeds and still contains '1'") {
    auto tmp = make_tmp_dir();
    auto path = (tmp / "signals" / "dup.request").string();
    REQUIRE(write_signal(path.c_str()) == true);
    REQUIRE(write_signal(path.c_str()) == true);
    std::ifstream f(path);
    REQUIRE(f.get() == '1');
}

TEST_CASE("write_signal returns false for non-existent parent directory") {
    auto path = fs::temp_directory_path() / "omnisave_no_such_dir" / "no_subdir" / "sig.request";
    REQUIRE(write_signal(path.string().c_str()) == false);
}
