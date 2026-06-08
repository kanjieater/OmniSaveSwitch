#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>
#include <vector>
#include "mock/vfs.hpp"
#include "omnisave.h"

// ── Test harness ───────────────────────────────────────────────────────────────

struct HashHarness {
    VfsData    vfs;
    FsFileSystem fs;

    HashHarness() { fs.impl = &vfs; }
};

// Packs a minimal valid ZIP into dst_vfs at dst_path and returns its fingerprint.
// Uses the real pack_save + zip_fingerprint pipeline for consistency tests.
static u32 make_packed_save(VfsData& src_vfs, VfsData& dst_vfs,
                             const char* dst_path,
                             const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files)
{
    FsFileSystem src_fs, dst_fs;
    src_fs.impl = &src_vfs;
    dst_fs.impl = &dst_vfs;
    // Files go at VFS root — pack_save is called with src_dir="/" and opens
    // them as "/<relpath>", matching the real sysmodule where save_fs root IS "/".
    for (auto& [name, data] : files)
        src_vfs.put_file(name, data);
    u32 fp = 0;
    int fc = 0;
    pack_save(&src_fs, "/", &dst_fs, dst_path, 0x1000, &fp, &fc);
    return fp;
}

// ── snap_hash_read / snap_hash_write ──────────────────────────────────────────

SCENARIO("snap_hash_read returns false when no .fp file exists", "[save_hash]") {
    GIVEN("an empty VFS") {
        HashHarness h;
        u32 out = 0xDEAD;
        WHEN("snap_hash_read is called") {
            bool ok = snap_hash_read(&h.fs, 0xAABBCCDDAABBCCDDull, "1122334455667788", &out);
            THEN("returns false") { REQUIRE_FALSE(ok); }
            AND_THEN("output is not modified") { REQUIRE(out == 0xDEAD); }
        }
    }
}

SCENARIO("snap_hash_write then snap_hash_read round-trips the fingerprint", "[save_hash]") {
    GIVEN("a VFS with the state/hashes directory") {
        HashHarness h;
        u64 tid = 0x0100F2C0115B6000ull;
        const char* uid = "AABBCCDD11223344";
        u32 written = 0xCAFEBABEu;

        WHEN("fingerprint is written then read back") {
            snap_hash_write(&h.fs, tid, uid, written);
            u32 read_back = 0;
            bool ok = snap_hash_read(&h.fs, tid, uid, &read_back);

            THEN("read succeeds") { REQUIRE(ok); }
            AND_THEN("value matches") { REQUIRE(read_back == written); }
        }
    }
}

SCENARIO("snap_hash_write overwrites an existing .fp file", "[save_hash]") {
    GIVEN("a VFS with an existing fingerprint") {
        HashHarness h;
        u64 tid = 0x0100EC001DE7E000ull;
        const char* uid = "DEADBEEFCAFE0001";
        snap_hash_write(&h.fs, tid, uid, 0x11111111u);

        WHEN("a new fingerprint is written") {
            snap_hash_write(&h.fs, tid, uid, 0x22222222u);
            u32 val = 0;
            snap_hash_read(&h.fs, tid, uid, &val);
            THEN("new value is returned") { REQUIRE(val == 0x22222222u); }
        }
    }
}

SCENARIO("different title_id → different .fp file", "[save_hash]") {
    GIVEN("two fingerprints for different title IDs") {
        HashHarness h;
        const char* uid = "AABBCCDD11223344";
        snap_hash_write(&h.fs, 0xAAAAAAAAAAAAAAAAull, uid, 0xAAAAAAAAu);
        snap_hash_write(&h.fs, 0xBBBBBBBBBBBBBBBBull, uid, 0xBBBBBBBBu);

        WHEN("each is read back") {
            u32 a = 0, b = 0;
            snap_hash_read(&h.fs, 0xAAAAAAAAAAAAAAAAull, uid, &a);
            snap_hash_read(&h.fs, 0xBBBBBBBBBBBBBBBBull, uid, &b);
            THEN("values are independent") {
                REQUIRE(a == 0xAAAAAAAAu);
                REQUIRE(b == 0xBBBBBBBBu);
            }
        }
    }
}

// ── sig_read / sig_write ───────────────────────────────────────────────────────

SCENARIO("sig_read returns false when no .sig file exists", "[save_hash]") {
    GIVEN("an empty VFS") {
        HashHarness h;
        SaveSignature out{};
        WHEN("sig_read is called") {
            bool ok = sig_read(&h.fs, 0x1234567890ABCDEFull, "DEADBEEF12345678", &out);
            THEN("returns false") { REQUIRE_FALSE(ok); }
        }
    }
}

SCENARIO("sig_write then sig_read round-trips all three fields", "[save_hash]") {
    GIVEN("a VFS") {
        HashHarness h;
        u64 tid = 0x0100000000000001ull;
        const char* uid = "0000000000000001";
        SaveSignature written;
        written.file_count   = 7;
        written.total_bytes  = 0x0102030405060708ull;
        written.newest_mtime = 0xFEDCBA9876543210ull;

        WHEN("signature is written then read back") {
            sig_write(&h.fs, tid, uid, &written);
            SaveSignature read_back{};
            bool ok = sig_read(&h.fs, tid, uid, &read_back);

            THEN("read succeeds") { REQUIRE(ok); }
            AND_THEN("file_count matches")   { REQUIRE(read_back.file_count   == written.file_count); }
            AND_THEN("total_bytes matches")  { REQUIRE(read_back.total_bytes  == written.total_bytes); }
            AND_THEN("newest_mtime matches") { REQUIRE(read_back.newest_mtime == written.newest_mtime); }
        }
    }
}

// ── zip_fingerprint consistency ────────────────────────────────────────────────

SCENARIO("zip_fingerprint matches the fp_out from pack_save", "[save_hash][zip_fingerprint]") {
    GIVEN("a packed save ZIP") {
        VfsData src_vfs, sd_vfs;
        FsFileSystem sd_fs;
        sd_fs.impl = &sd_vfs;
        const char* zip_path = "/archive/save.zip";
        u32 pack_fp = make_packed_save(src_vfs, sd_vfs, zip_path, {
            {"/system.dat", {0x01, 0x02, 0x03}},
            {"/extra.dat",  {0xAA, 0xBB}},
        });

        WHEN("zip_fingerprint reads the Central Directory") {
            u32 read_fp = 0;
            bool ok = zip_fingerprint(&sd_fs, zip_path, &read_fp);

            THEN("zip_fingerprint succeeds") { REQUIRE(ok); }
            AND_THEN("fingerprint matches pack_save's fp_out") { REQUIRE(read_fp == pack_fp); }
        }
    }
}

SCENARIO("snap_hash_update_from_archive writes the same fp as pack_save", "[save_hash]") {
    GIVEN("a packed save on the same VFS used for hash storage (mirrors real SD card)") {
        // In production both the archive and the .fp file live on the SD card (same FsFileSystem).
        VfsData src_vfs, sd_vfs;
        FsFileSystem sd_fs;
        sd_fs.impl = &sd_vfs;

        const char* zip_path = "/outbound/20240101_120000_00-AABBCCDDAABBCCDD-1122334455667788/save.zip";
        const char* key      = "20240101_120000_00-AABBCCDDAABBCCDD-1122334455667788";
        u32 pack_fp = make_packed_save(src_vfs, sd_vfs, zip_path, {
            {"/data.dat", {0xDE, 0xAD, 0xBE, 0xEF}},
        });

        WHEN("snap_hash_update_from_archive is called") {
            snap_hash_update_from_archive(&sd_fs, key, zip_path);

            THEN("the stored .fp matches the pack fingerprint") {
                u32 stored = 0;
                bool ok = snap_hash_read(&sd_fs, 0xAABBCCDDAABBCCDDull, "1122334455667788", &stored);
                REQUIRE(ok);
                REQUIRE(stored == pack_fp);
            }
        }
    }
}

// ── Phase 1 contract: stat dedup requires FP file ─────────────────────────────
// These tests verify the contract that save_extract Phase 1 enforces:
// snap_hash_read must return true before stat dedup can fire.
// We test the primitives directly since save_extract is hardware-gated.

SCENARIO("snap_hash_read is false without a prior upload: stat dedup cannot fire", "[save_hash][phase1_contract]") {
    GIVEN("no .fp file has ever been written for a title") {
        HashHarness h;
        u32 fp = 0;
        WHEN("snap_hash_read is checked") {
            bool has_fp = snap_hash_read(&h.fs, 0xAAAAAAAAAAAAAAAAull, "1122334455667788", &fp);
            THEN("has_fp is false — Phase 1 guard prevents stat dedup") {
                REQUIRE_FALSE(has_fp);
            }
        }
    }
}

SCENARIO("snap_hash_read is true after upload: stat dedup is permitted", "[save_hash][phase1_contract]") {
    GIVEN("a successful upload has written a .fp file") {
        HashHarness h;
        u64 tid = 0xAAAAAAAAAAAAAAAAull;
        const char* uid = "1122334455667788";
        snap_hash_write(&h.fs, tid, uid, 0x12345678u);

        WHEN("snap_hash_read is checked") {
            u32 fp = 0;
            bool has_fp = snap_hash_read(&h.fs, tid, uid, &fp);
            THEN("has_fp is true — Phase 1 guard allows stat dedup") {
                REQUIRE(has_fp);
                REQUIRE(fp == 0x12345678u);
            }
        }
    }
}

SCENARIO("fp matches after identical repack: Phase 3 would correctly dedup", "[save_hash][phase1_contract]") {
    GIVEN("a packed save with known fingerprint stored") {
        std::vector<std::pair<std::string, std::vector<uint8_t>>> files = {
            {"/save.dat", {0x01, 0x02, 0x03, 0x04}},
        };

        VfsData src_vfs1, zip_vfs1, src_vfs2, zip_vfs2;
        const char* zip1 = "/out/key1/save.zip";
        const char* zip2 = "/out/key2/save.zip";
        u32 fp1 = make_packed_save(src_vfs1, zip_vfs1, zip1, files);
        u32 fp2 = make_packed_save(src_vfs2, zip_vfs2, zip2, files);

        WHEN("fingerprints of two identical packs are compared") {
            THEN("they are equal — Phase 3 FP dedup would correctly suppress the upload") {
                REQUIRE(fp1 == fp2);
            }
        }
    }
}

SCENARIO("fp differs after content change: Phase 3 correctly allows upload", "[save_hash][phase1_contract]") {
    GIVEN("two packs with different file content") {
        VfsData src_a, zip_a, src_b, zip_b;
        u32 fp_a = make_packed_save(src_a, zip_a, "/a.zip", {{"/d.dat", {0x01}}});
        u32 fp_b = make_packed_save(src_b, zip_b, "/b.zip", {{"/d.dat", {0x02}}});

        WHEN("fingerprints are compared") {
            THEN("they differ — Phase 3 would not suppress the changed save") {
                REQUIRE(fp_a != fp_b);
            }
        }
    }
}
