#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>
#include "mock/vfs.hpp"
#include "omnisave.h"

// ── helpers ───────────────────────────────────────────────────────────────────

static VfsData  g_vfs;
static FsFileSystem g_fs;

static void setup_config_vfs(const std::string& content) {
    g_vfs = VfsData{};
    g_fs.impl = &g_vfs;
    // config_load reads from OMNI_ROOT "/config.ini"
    g_vfs.put_file(OMNI_ROOT "/config.ini", content);
}

static void reset_device_suffix() {
    g_config.device_suffix[0] = '\0';
}

// ── config_load: device_suffix parsing ───────────────────────────────────────

SCENARIO("config_load parses device_suffix", "[config]") {
    GIVEN("config.ini contains device_suffix=emu") {
        setup_config_vfs("server_address=http://server:8991\n"
                         "device_suffix=emu\n");

        WHEN("config_load is called") {
            config_load(&g_fs);

            THEN("g_config.device_suffix is 'emu'") {
                REQUIRE(std::string(g_config.device_suffix) == "emu");
            }
        }
    }

    GIVEN("config.ini has device_suffix= (empty value)") {
        setup_config_vfs("device_suffix=\n");

        WHEN("config_load is called") {
            config_load(&g_fs);

            THEN("g_config.device_suffix is empty") {
                REQUIRE(g_config.device_suffix[0] == '\0');
            }
        }
    }

    GIVEN("config.ini has no device_suffix key") {
        setup_config_vfs("server_address=http://server:8991\n");

        WHEN("config_load is called") {
            g_config.device_suffix[0] = 'X';   // poison value
            config_load(&g_fs);

            THEN("g_config.device_suffix is reset to empty") {
                REQUIRE(g_config.device_suffix[0] == '\0');
            }
        }
    }

    GIVEN("a previous config_load set device_suffix, and the new config has none") {
        setup_config_vfs("device_suffix=sysnand\n");
        config_load(&g_fs);
        REQUIRE(std::string(g_config.device_suffix) == "sysnand");

        setup_config_vfs("server_address=http://server:8991\n");

        WHEN("config_load is called again without device_suffix") {
            config_load(&g_fs);

            THEN("device_suffix is cleared") {
                REQUIRE(g_config.device_suffix[0] == '\0');
            }
        }
    }

    GIVEN("config.ini has device_suffix with a 31-char value (max)") {
        setup_config_vfs("device_suffix=1234567890123456789012345678901\n");  // 31 chars

        WHEN("config_load is called") {
            config_load(&g_fs);

            THEN("suffix is stored in full") {
                REQUIRE(std::string(g_config.device_suffix) == "1234567890123456789012345678901");
            }
        }
    }

    GIVEN("config.ini has device_suffix with a value that would overflow the field") {
        // device_suffix field is 32 bytes → max 31 meaningful chars
        setup_config_vfs("device_suffix=12345678901234567890123456789012345\n");  // 35 chars

        WHEN("config_load is called") {
            config_load(&g_fs);

            THEN("suffix is truncated to 31 chars") {
                REQUIRE(strlen(g_config.device_suffix) == 31);
            }
        }
    }
}

// ── config_apply_device_suffix ────────────────────────────────────────────────

SCENARIO("config_apply_device_suffix appends to device ID when suffix set", "[config]") {
    GIVEN("device_suffix is 'emu'") {
        reset_device_suffix();
        strncpy(g_config.device_suffix, "emu", sizeof(g_config.device_suffix) - 1);

        WHEN("apply is called on a MAC-format ID") {
            char id[64] = "AABBCCDDEE00";
            config_apply_device_suffix(id, sizeof(id));

            THEN("suffix is appended with a dash") {
                REQUIRE(std::string(id) == "AABBCCDDEE00-emu");
            }
        }
    }

    GIVEN("device_suffix is empty") {
        reset_device_suffix();

        WHEN("apply is called") {
            char id[64] = "AABBCCDDEE00";
            config_apply_device_suffix(id, sizeof(id));

            THEN("buffer is unchanged") {
                REQUIRE(std::string(id) == "AABBCCDDEE00");
            }
        }
    }

    GIVEN("two different suffixes for SysNAND and EmuNAND") {
        char sys_id[64] = "AABBCCDDEE00";
        char emu_id[64] = "AABBCCDDEE00";

        reset_device_suffix();
        strncpy(g_config.device_suffix, "sys", sizeof(g_config.device_suffix) - 1);
        config_apply_device_suffix(sys_id, sizeof(sys_id));

        strncpy(g_config.device_suffix, "emu", sizeof(g_config.device_suffix) - 1);
        config_apply_device_suffix(emu_id, sizeof(emu_id));

        THEN("SysNAND ID differs from EmuNAND ID") {
            REQUIRE(std::string(sys_id) != std::string(emu_id));
        }
        AND_THEN("SysNAND ID is AABBCCDDEE00-sys") {
            REQUIRE(std::string(sys_id) == "AABBCCDDEE00-sys");
        }
        AND_THEN("EmuNAND ID is AABBCCDDEE00-emu") {
            REQUIRE(std::string(emu_id) == "AABBCCDDEE00-emu");
        }
    }
}

SCENARIO("config_apply_device_suffix does not overflow the buffer", "[config]") {
    GIVEN("device_suffix is set and the ID buffer is nearly full") {
        reset_device_suffix();
        strncpy(g_config.device_suffix, "emu", sizeof(g_config.device_suffix) - 1);

        // Fill buffer leaving 1 byte for NUL — suffix should not overflow.
        char id[16];
        memset(id, 'A', sizeof(id) - 1);
        id[sizeof(id) - 1] = '\0';

        WHEN("apply is called") {
            config_apply_device_suffix(id, sizeof(id));

            THEN("buffer is NUL-terminated within bounds") {
                bool nul_found = false;
                for (size_t i = 0; i < sizeof(id); i++) {
                    if (id[i] == '\0') { nul_found = true; break; }
                }
                REQUIRE(nul_found);
            }
        }
    }
}
