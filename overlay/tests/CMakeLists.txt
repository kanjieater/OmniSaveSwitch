cmake_minimum_required(VERSION 3.20)
project(OmniSaveOverlayTests CXX C)

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
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.8.1
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(Catch2)

# ── Overlay parser library (no Tesla, no libnx — pure C++ 17) ─────────────────
set(OVERLAY_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../source)

add_library(overlay_parsers STATIC
    ${OVERLAY_SRC}/status_parser.cpp
    ${OVERLAY_SRC}/history_parser.cpp
    ${OVERLAY_SRC}/signal_writer.cpp
)
target_include_directories(overlay_parsers PUBLIC ${OVERLAY_SRC})
target_compile_options(overlay_parsers PRIVATE -Wall -Wextra -Wno-unused-parameter)

# ── Test executable ────────────────────────────────────────────────────────────
add_executable(overlay_tests
    test_status_parser.cpp
    test_history_parser.cpp
    test_signal_writer.cpp
)
target_include_directories(overlay_tests PRIVATE ${OVERLAY_SRC})
target_link_libraries(overlay_tests PRIVATE
    overlay_parsers
    Catch2::Catch2WithMain
)
target_compile_options(overlay_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)

# ── CTest integration ──────────────────────────────────────────────────────────
include(CTest)
include(Catch)
catch_discover_tests(overlay_tests)
