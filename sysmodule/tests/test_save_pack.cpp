#include <catch2/catch_test_macros.hpp>
#include <rapidcheck.h>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include "mock/vfs.hpp"
#include "omnisave.h"

// Build two VFS instances: src_fs (save directory) and dst_fs (output file).
// pack_save reads from src_fs, writes to dst_fs.
// unpack_save reads packed blob from dst_fs, writes files back into a third VFS.

struct PackHarness {
    VfsData    src_vfs;   // save game directory
    VfsData    dst_vfs;   // ZIP output file
    VfsData    out_vfs;   // restored save directory
    FsFileSystem src_fs, dst_fs, out_fs;

    PackHarness() {
        src_fs.impl = &src_vfs;
        dst_fs.impl = &dst_vfs;
        out_fs.impl = &out_vfs;
    }

    void add_save_file(const std::string& rel_path,
                       const std::vector<uint8_t>& data) {
        // Files go at the VFS root — pack_save opens them as "/<rel_path>"
        src_vfs.put_file(rel_path, data);
    }

    bool pack(u64 save_data_size = 0x10000, u32* fp_out = nullptr) {
        u32 dummy = 0;
        int fc = 0;
        return pack_save(&src_fs, "/", &dst_fs, "/packed/save.zip", save_data_size,
                         fp_out ? fp_out : &dummy, &fc);
    }

    bool unpack(u64* sds_out = nullptr) {
        return unpack_save(&dst_fs, "/packed/save.zip", &out_fs, sds_out);
    }
};

// ── Correctness tests ──────────────────────────────────────────────────────────

SCENARIO("pack empty directory succeeds and unpacks cleanly", "[save_pack]") {
    GIVEN("an empty save directory") {
        PackHarness h;

        WHEN("packed") {
            bool ok = h.pack(0x8000);
            THEN("pack_save returns true") { REQUIRE(ok); }
            THEN("output file exists") { REQUIRE(h.dst_vfs.has_file("/packed/save.zip")); }

            AND_WHEN("unpacked") {
                u64 sds = 0;
                bool uok = h.unpack(&sds);
                THEN("unpack_save returns true") { REQUIRE(uok); }
                AND_THEN("save_data_size is preserved") { REQUIRE(sds == 0x8000); }
            }
        }
    }
}

SCENARIO("single file round-trips with identical content", "[save_pack]") {
    GIVEN("a save directory with one file") {
        PackHarness h;
        std::vector<uint8_t> data = {0x01, 0x02, 0x03, 0xAA, 0xBB, 0xFF};
        h.add_save_file("/system.dat", data);

        WHEN("packed then unpacked") {
            REQUIRE(h.pack());
            REQUIRE(h.unpack());

            THEN("restored file content matches original") {
                auto restored = h.out_vfs.get_file("/system.dat");
                REQUIRE(restored == data);
            }
        }
    }
}

SCENARIO("multiple files in a flat directory round-trip", "[save_pack]") {
    GIVEN("a save directory with three files") {
        PackHarness h;
        h.add_save_file("/a.dat", {0x01, 0x02});
        h.add_save_file("/b.dat", {0x10, 0x20, 0x30});
        h.add_save_file("/c.dat", {0xFF});

        WHEN("packed then unpacked") {
            REQUIRE(h.pack());
            REQUIRE(h.unpack());

            THEN("all files are restored with correct content") {
                REQUIRE(h.out_vfs.get_file("/a.dat") == std::vector<uint8_t>{0x01, 0x02});
                REQUIRE(h.out_vfs.get_file("/b.dat") == std::vector<uint8_t>{0x10, 0x20, 0x30});
                REQUIRE(h.out_vfs.get_file("/c.dat") == std::vector<uint8_t>{0xFF});
            }
        }
    }
}

SCENARIO("subdirectory files are packed and restored", "[save_pack]") {
    GIVEN("a save directory with nested subdirectory") {
        PackHarness h;
        h.add_save_file("/journal/entry1.dat", {0xAA, 0xBB});
        h.add_save_file("/settings.dat", {0x01});

        WHEN("packed then unpacked") {
            REQUIRE(h.pack());
            REQUIRE(h.unpack());

            THEN("nested file is restored") {
                REQUIRE(h.out_vfs.get_file("/journal/entry1.dat")
                        == std::vector<uint8_t>{0xAA, 0xBB});
            }
            AND_THEN("flat file is also restored") {
                REQUIRE(h.out_vfs.get_file("/settings.dat")
                        == std::vector<uint8_t>{0x01});
            }
        }
    }
}

SCENARIO("zero-length file round-trips", "[save_pack]") {
    GIVEN("a save directory containing an empty file") {
        PackHarness h;
        h.add_save_file("/empty.dat", {});

        WHEN("packed then unpacked") {
            REQUIRE(h.pack());
            REQUIRE(h.unpack());

            THEN("empty file is restored") {
                REQUIRE(h.out_vfs.has_file("/empty.dat"));
                REQUIRE(h.out_vfs.get_file("/empty.dat").empty());
            }
        }
    }
}

SCENARIO("corrupt magic header → unpack returns false", "[save_pack]") {
    GIVEN("a packed blob with corrupted magic") {
        PackHarness h;
        h.add_save_file("/x.dat", {0xDE, 0xAD});
        REQUIRE(h.pack());

        auto& blob = h.dst_vfs.files["/packed/save.zip"];
        REQUIRE(blob.size() >= 8);
        blob[0] = 0xFF; // corrupt the magic

        WHEN("unpack is attempted") {
            bool ok = h.unpack();
            THEN("returns false") { REQUIRE_FALSE(ok); }
        }
    }
}

SCENARIO("truncated blob → unpack returns false", "[save_pack]") {
    GIVEN("a packed blob truncated mid-file") {
        PackHarness h;
        h.add_save_file("/big.dat", std::vector<uint8_t>(512, 0xAA));
        REQUIRE(h.pack());

        auto& blob = h.dst_vfs.files["/packed/save.zip"];
        blob.resize(blob.size() / 2); // truncate

        WHEN("unpack is attempted") {
            bool ok = h.unpack();
            THEN("returns false") { REQUIRE_FALSE(ok); }
        }
    }
}

// ── RapidCheck property ────────────────────────────────────────────────────────

TEST_CASE("Property: arbitrary file sets round-trip through ZIP", "[save_pack][property]") {
    REQUIRE(rc::check("pack → unpack produces identical files", []() {
        // Generate 1-6 files, each with a short name and arbitrary byte content.
        auto num_files = *rc::gen::inRange(1, 7);
        std::map<std::string, std::vector<uint8_t>> original;

        for (int i = 0; i < num_files; i++) {
            auto name_len = *rc::gen::inRange(1, 16);
            std::string name;
            for (int j = 0; j < name_len; j++)
                name += (char)('a' + *rc::gen::inRange(0, 26));
            name += ".dat";

            auto content = *rc::gen::container<std::vector<uint8_t>>(
                *rc::gen::inRange(0, 256),
                rc::gen::arbitrary<uint8_t>());
            original[name] = content;
        }

        PackHarness h;
        for (auto& [name, data] : original)
            h.add_save_file("/" + name, data);

        bool pack_ok   = h.pack();
        bool unpack_ok = h.unpack();
        RC_ASSERT(pack_ok);
        RC_ASSERT(unpack_ok);

        for (auto& [name, data] : original) {
            auto restored = h.out_vfs.get_file("/" + name);
            RC_ASSERT(restored == data);
        }
    }));
}
