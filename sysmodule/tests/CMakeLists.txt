cmake_minimum_required(VERSION 3.20)
project(OmniSaveTests CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD   11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Coverage flags ─────────────────────────────────────────────────────────────
if(COVERAGE)
    add_compile_options(-O0 -g --coverage -fprofile-arcs -ftest-coverage)
    add_link_options(--coverage)
endif()

# ── FetchContent dependencies ──────────────────────────────────────────────────
include(FetchContent)

# Do not attempt network updates if deps were already fetched (offline-safe).
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.8.1
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(Catch2)

FetchContent_Declare(rapidcheck
    GIT_REPOSITORY https://github.com/emil-e/rapidcheck.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE)
set(RC_ENABLE_CATCH2 ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(rapidcheck)

# ── Mock include path (must precede system paths) ─────────────────────────────
# This ensures #include <switch.h> resolves to tests/mock/switch.h and
# #include <curl/curl.h> resolves to tests/mock/curl/curl.h.
set(MOCK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/mock)
set(INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../include)
set(SOURCE_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../source)

# ── Core library (production sources compiled for x86_64) ─────────────────────
add_library(omni_core STATIC
    ${SOURCE_DIR}/fsm.cpp
    ${SOURCE_DIR}/sync_prefs.cpp
    ${SOURCE_DIR}/save_pack.cpp
    ${SOURCE_DIR}/save_pack_read.cpp
    ${SOURCE_DIR}/save_hash.cpp
    ${SOURCE_DIR}/crc32.cpp
    ${SOURCE_DIR}/recovery.cpp
    ${SOURCE_DIR}/catalog.cpp
    ${SOURCE_DIR}/config.cpp
    ${SOURCE_DIR}/game_event.cpp
    ${SOURCE_DIR}/save_clear.cpp
    ${MOCK_DIR}/vfs.cpp
    ${MOCK_DIR}/omnisave_globals.cpp
    ${MOCK_DIR}/fsm_deps.cpp
    ${MOCK_DIR}/catalog_mock.cpp
)
target_include_directories(omni_core BEFORE PUBLIC
    ${MOCK_DIR}
    ${INCLUDE_DIR}
)
target_compile_options(omni_core PRIVATE -Wall -Wextra -Wno-unused-parameter)

# ── Test executable ────────────────────────────────────────────────────────────
add_executable(omnisave_tests
    test_fsm.cpp
    test_sync_prefs.cpp
    test_inbound_meta.cpp
    test_save_pack.cpp
    test_save_hash.cpp
    test_recovery.cpp
    test_catalog.cpp
    test_save_spaces.cpp
    test_config.cpp
    test_game_event.cpp
    test_save_ops.cpp
)
target_include_directories(omnisave_tests BEFORE PRIVATE
    ${MOCK_DIR}
    ${INCLUDE_DIR}
    ${SOURCE_DIR}
)
target_link_libraries(omnisave_tests PRIVATE
    omni_core
    Catch2::Catch2WithMain
    rapidcheck
)
target_compile_options(omnisave_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)

# ── State test executable (state.cpp + recovery.cpp + minimal stubs) ──────────
# Uses its own stubs; does NOT link omni_core (which has conflicting FSM mocks).
add_executable(omnisave_state_tests
    test_state.cpp
    ${SOURCE_DIR}/state.cpp
    ${SOURCE_DIR}/recovery.cpp
    ${MOCK_DIR}/vfs.cpp
    ${MOCK_DIR}/omnisave_globals.cpp
    ${MOCK_DIR}/state_test_deps.cpp
)
target_include_directories(omnisave_state_tests BEFORE PRIVATE
    ${MOCK_DIR}
    ${INCLUDE_DIR}
    ${SOURCE_DIR}
)
target_link_libraries(omnisave_state_tests PRIVATE
    Catch2::Catch2WithMain
)
target_compile_options(omnisave_state_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)

# ── CTest integration ──────────────────────────────────────────────────────────
include(CTest)
include(Catch)
catch_discover_tests(omnisave_tests)
catch_discover_tests(omnisave_state_tests)
