#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <curl/curl.h>
#include "omnisave.h"
#include "game_event.h"
#include "sync_prefs.h"
#include "catalog.h"

#define INNER_HEAP_SIZE 0x100000

// ── Global definitions ─────────────────────────────────────────────────────────

char   s_fw_version[32]           = "unknown";
char   s_device_id[64]            = "unknown";
char   s_hw_type[16]              = "switch";
char   s_uid_hex[17]              = "0000000000000000";
char   s_account_nickname[33]     = "";
char   s_device_nickname_hint[32] = "";
u8     s_copy_buf[65536];

volatile bool s_dirty                 = false;
std::atomic<bool> s_system_sleeping{false};
std::atomic<bool> s_needs_recovery{false};
std::atomic<uint64_t> s_pending_extract_title{0};
std::atomic<uint64_t> s_extracting_title{0};  // non-zero while save_extract holds the save FS open

std::atomic<u64> s_running_pid{0};
std::atomic<u64> s_running_title_id{0};
char g_dns_servers[64]   = {0};

u64  s_save_data_size = 0;
u64  s_save_data_id   = 0;
u8   s_save_space_id  = 0;

Result s_socket_init_rc = 0;
static Thread s_psc_thread;
static Thread s_game_monitor_thread;

// ── libnx runtime ──────────────────────────────────────────────────────────────

#ifdef __cplusplus
extern "C" {
#endif

u32 __nx_applet_type     = AppletType_None;
u32 __nx_fs_num_sessions = 2;

void __libnx_initheap(void) {
    static u8 inner_heap[INNER_HEAP_SIZE];
    extern void* fake_heap_start;
    extern void* fake_heap_end;
    fake_heap_start = inner_heap;
    fake_heap_end   = inner_heap + sizeof(inner_heap);
}

void __appInit(void) {
    Result rc = smInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));

    rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
            snprintf(s_fw_version, sizeof(s_fw_version), "%d.%d.%d",
                fw.major, fw.minor, fw.micro);
        }
        setsysExit();
    }

    rc = fsInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));

    accountInitialize(AccountServiceType_System);
    nsInitialize();

    rc = timeInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(rc);

    fsdevMountSdmc();
    rc = pmdmntInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(rc);

    pminfoInitialize();

    static const SocketInitConfig sock_cfg = {
        .tcp_tx_buf_size     = 0x800,
        .tcp_rx_buf_size     = 0x1000,
        .tcp_tx_buf_max_size = 0x2EE0,
        .tcp_rx_buf_max_size = 0x2EE0,
        .udp_tx_buf_size     = 0,
        .udp_rx_buf_size     = 0,
        .sb_efficiency       = 4,
    };
    s_socket_init_rc = socketInitialize(&sock_cfg);

    nifmInitialize(NifmServiceType_User);
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

void __appExit(void) {
    curl_global_cleanup();
    nsExit();
    accountExit();
    socketExit();
    nifmExit();
    pminfoExit();
    pmdmntExit();
    fsdevUnmountAll();
    timeExit();
    fsExit();
    smExit();
}

#ifdef __cplusplus
}
#endif

// ── Time helpers ───────────────────────────────────────────────────────────────

u64 get_posix_utc(void) {
    for (int i = 0; i < 30; i++) {
        u64 posix = 0;
        if (R_SUCCEEDED(timeGetCurrentTime(TimeType_UserSystemClock, &posix)))
            if (posix > 1577836800ULL) return posix;
        svcSleepThread(500000000ULL);
    }
    return 0;
}

void format_local_time(char* out, size_t sz, u64 posix) {
    if (posix == 0) { snprintf(out, sz, "[time unavailable]"); return; }
    TimeCalendarTime cal;
    if (R_SUCCEEDED(timeToCalendarTimeWithMyRule(posix, &cal, NULL)))
        snprintf(out, sz, "%04d-%02d-%02d %02d:%02d:%02d",
            cal.year, cal.month, cal.day, cal.hour, cal.minute, cal.second);
    else
        snprintf(out, sz, "[tz conversion failed]");
}

void make_ts_folder(char* out, size_t sz, u64 posix) {
    TimeCalendarTime cal;
    if (posix > 0 && R_SUCCEEDED(timeToCalendarTimeWithMyRule(posix, &cal, NULL)))
        snprintf(out, sz, "%04d%02d%02d_%02d%02d%02d",
            cal.year, (int)cal.month, (int)cal.day,
            (int)cal.hour, (int)cal.minute, (int)cal.second);
    else
        snprintf(out, sz, "00000000_000000");
}

bool get_title_name(u64 title_id, char* out, size_t sz) {
    static NsApplicationControlData ns_ctrl;
    u64 actual = 0;
    if (R_FAILED(nsGetApplicationControlData(NsApplicationControlSource_Storage,
                 title_id, &ns_ctrl, sizeof(ns_ctrl), &actual)))
        return false;
    for (int i = 0; i < 16; i++) {
        if (ns_ctrl.nacp.lang[i].name[0]) {
            snprintf(out, sz, "%s", ns_ctrl.nacp.lang[i].name);
            return true;
        }
    }
    return false;
}

// ── Network helpers ────────────────────────────────────────────────────────────

void refresh_dns_servers(void) {
    u32 ip = 0, mask = 0, gw = 0, dns1 = 0, dns2 = 0;
    if (R_FAILED(nifmGetCurrentIpConfigInfo(&ip, &mask, &gw, &dns1, &dns2))
            || ip == 0 || dns1 == 0)
        return;
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
        (dns1 >> 0) & 0xFF, (dns1 >> 8) & 0xFF,
        (dns1 >> 16) & 0xFF, (dns1 >> 24) & 0xFF);
    if (dns2 != 0 && n > 0 && n < (int)sizeof(buf) - 1)
        snprintf(buf + n, sizeof(buf) - (size_t)n, ",%u.%u.%u.%u",
            (dns2 >> 0) & 0xFF, (dns2 >> 8) & 0xFF,
            (dns2 >> 16) & 0xFF, (dns2 >> 24) & 0xFF);
    memcpy(g_dns_servers, buf, sizeof(g_dns_servers));
}

bool wait_network_ready(void) {
    if (g_config.server_host[0] == '\0') return false;
    for (int i = 0; i < 10; i++) {
        refresh_dns_servers();
        if (http_ping("/api/health") > 0) return true;
        svcSleepThread(2000000000ULL);
    }
    return false;
}

// ── Device identity ────────────────────────────────────────────────────────────

static void detect_device_id(FsFileSystem* sd) {
    smInitialize();
    if (R_SUCCEEDED(setcalInitialize())) {
        SetCalBdAddress bd = {0};
        bool all_zero = true;
        if (R_SUCCEEDED(setcalGetBdAddress(&bd))) {
            for (int i = 0; i < 6; i++) if (bd.bd_addr[i]) { all_zero = false; break; }
            if (!all_zero)
                snprintf(s_device_id, sizeof(s_device_id),
                         "%02X%02X%02X%02X%02X%02X",
                         bd.bd_addr[0], bd.bd_addr[1], bd.bd_addr[2],
                         bd.bd_addr[3], bd.bd_addr[4], bd.bd_addr[5]);
        }
        setcalExit();
    }
    if (R_SUCCEEDED(setsysInitialize())) {
        SetSysProductModel model = SetSysProductModel_Nx;
        if (R_SUCCEEDED(setsysGetProductModel(&model))) {
            switch (model) {
                case SetSysProductModel_Hoag:
                case SetSysProductModel_Calcio:
                    snprintf(s_hw_type, sizeof(s_hw_type), "switch_lite"); break;
                case SetSysProductModel_Aula:
                    snprintf(s_hw_type, sizeof(s_hw_type), "switch_oled"); break;
                default:
                    snprintf(s_hw_type, sizeof(s_hw_type), "switch");     break;
            }
        }
        setsysExit();
    }
    smExit();

    if (strcmp(s_device_id, "unknown") == 0) {
        char gen_id[64] = {0};
        bool found = fs_read_text_file(sd, OMNI_ROOT "/state/device_id.txt",
                                       gen_id, sizeof(gen_id));
        for (int i = 0; i < 63; i++)
            if (gen_id[i] == '\n' || gen_id[i] == '\r') { gen_id[i] = '\0'; break; }
        if (!found || gen_id[0] == '\0') {
            u64 tick = svcGetSystemTick();
            u64 posix = get_posix_utc();
            snprintf(gen_id, sizeof(gen_id), "GEN-%016llX",
                     (unsigned long long)(tick ^ (posix << 16)));
            ensure_dir(sd, OMNI_ROOT "/state");
            fs_write_text_file(sd, OMNI_ROOT "/state/device_id.txt", gen_id);
            fsFsCommit(sd);
        }
        snprintf(s_device_id, sizeof(s_device_id), "%.63s", gen_id);
    }

    // Append device_suffix from config.ini when set — allows distinguishing
    // SysNAND from EmuNAND, which share the same Bluetooth MAC via PRODINFO.
    config_apply_device_suffix(s_device_id, sizeof(s_device_id));
}

// ── Setup ──────────────────────────────────────────────────────────────────────

static void setup(void) {
    FsFileSystem sd;
    if (R_FAILED(fsOpenSdCardFileSystem(&sd))) return;

    ensure_dir(&sd, OMNI_ROOT);
    ensure_dir(&sd, OMNI_ROOT "/state");
    ensure_dir(&sd, OMNI_ROOT "/state/lineage");
    ensure_dir(&sd, OMNI_ROOT "/tmp_out");
    ensure_dir(&sd, OMNI_ROOT "/outbound");
    ensure_dir(&sd, OMNI_ROOT "/tmp_in");
    ensure_dir(&sd, OMNI_ROOT "/inbound");
    ensure_dir(&sd, OMNI_ROOT "/errors");
    fsFsCommit(&sd);

    config_load(&sd);
    detect_device_id(&sd);
    state_init(&sd);
    sync_prefs_load(&sd);

    fsFsClose(&sd);
}

// ── InputSnapshot builder ──────────────────────────────────────────────────────

int  s_heartbeat_ticks       = 10;
int  s_poll_hot_remain       = 0;
u64  s_last_activity_posix   = 0;
bool s_network_was_down      = false;
static int s_extract_retry_count = 0;
static u64 s_extract_retry_title = 0;

static void build_snapshot(FsFileSystem* sd, InputSnapshot* snap) {
    memset(snap, 0, sizeof(*snap));

    {
        FsDir odir;
        if (R_SUCCEEDED(fsFsOpenDirectory(sd, OMNI_ROOT "/outbound",
                FsDirOpenMode_ReadDirs, &odir))) {
            FsDirectoryEntry ents[ENTRY_BATCH]; s64 count = 0;
            fsDirRead(&odir, &count, ENTRY_BATCH, ents);
            fsDirClose(&odir);
            // Pass 1: oldest non-deferred key.
            for (int i = 0; i < (int)count && !snap->has_outbound; i++) {
                if (strlen(ents[i].name) == SNAP_KEY_LEN &&
                        !fsm_is_key_deferred(ents[i].name)) {
                    snap->has_outbound = true;
                    snprintf(snap->outbound_key, sizeof(snap->outbound_key),
                             "%s", ents[i].name);
                }
            }
            // Pass 2: fallback — only deferred key(s) remain.
            for (int i = 0; i < (int)count && !snap->has_outbound; i++) {
                if (strlen(ents[i].name) == SNAP_KEY_LEN) {
                    snap->has_outbound = true;
                    snprintf(snap->outbound_key, sizeof(snap->outbound_key),
                             "%s", ents[i].name);
                }
            }
        }
    }

    {
        FsDir idir;
        if (R_SUCCEEDED(fsFsOpenDirectory(sd, OMNI_ROOT "/inbound",
                FsDirOpenMode_ReadDirs, &idir))) {
            FsDirectoryEntry ents[ENTRY_BATCH]; s64 count = 0;
            if (R_SUCCEEDED(fsDirRead(&idir, &count, ENTRY_BATCH, ents)) && count > 0
                    && strlen(ents[0].name) == SNAP_KEY_LEN) {
                snap->has_inbound = true;
                snprintf(snap->inbound_key, sizeof(snap->inbound_key),
                         "%s", ents[0].name);
                char meta[FS_MAX_PATH], buf[128];  // meta JSON is ~72 bytes; 64 was too small
                snprintf(meta, sizeof(meta), OMNI_ROOT "/inbound/%s.json",
                         ents[0].name);
                if (fs_read_text_file(sd, meta, buf, sizeof(buf))) {
                    const char* p = strstr(buf, "\"snap_id\":");
                    if (p) snap->inbound_snap_id = (int)strtol(p + 10, NULL, 10);
                }
            }
            fsDirClose(&idir);
        }
    }

    snap->heartbeat_due = (--s_heartbeat_ticks <= 0);
    if (snap->heartbeat_due) {
        if (s_poll_hot_remain > 0) {
            s_poll_hot_remain--;
            s_heartbeat_ticks = 2;
        } else {
            u64 now = get_posix_utc();
            if (now < s_last_activity_posix) s_last_activity_posix = now;
            u64 idle = now - s_last_activity_posix;
            if      (idle <   60) s_heartbeat_ticks =  15;
            else if (idle <  300) s_heartbeat_ticks =  30;
            else if (idle < 1200) s_heartbeat_ticks =  60;
            else                  s_heartbeat_ticks = 300;
        }
    }

    snap->game_running     = (s_running_pid.load(std::memory_order_relaxed) != 0);
    snap->running_title_id = s_running_title_id.load(std::memory_order_relaxed);

    snap->storage_critical = false;
}

// ── Game monitor thread (polls pmdmnt independently of FSM blocking) ───────────

static void game_monitor_func(void* arg) {
    (void)arg;
    u64 last_pid   = 0;
    u64 last_title = 0;
    int fail_count = 0;
    int run_ticks  = 0;  // 200ms pmdmnt polls while game is confirmed running

    while (true) {
        u64 pid = 0;
        Result rc = pmdmntGetApplicationProcessId(&pid);

        if (R_SUCCEEDED(rc) && pid != 0) {
            fail_count = 0;
            if (last_pid == 0) {
                run_ticks = 0;
                pminfoGetProgramId(&last_title, pid);
                if (s_extracting_title.load(std::memory_order_relaxed) != 0 &&
                    last_title == s_extracting_title.load(std::memory_order_relaxed)) {
                    char label[32] = {0};
                    title_label(last_title, label, sizeof(label));
                    char msg[96];
                    snprintf(msg, sizeof(msg), "%s: Backup In Progress...", label);
                    notif_push("backup-locked", msg);
                }
            }
            run_ticks++;
            last_pid           = pid;
            s_running_pid.store(pid, std::memory_order_relaxed);
            s_running_title_id.store(last_title, std::memory_order_relaxed);
        } else {
            fail_count++;
            // Require 2 consecutive fails — avoids transient pmdmnt errors.
            if (fail_count >= 2 && last_pid != 0 && last_title != 0) {
                // Skip blocked/instant launches (< 1s = 5 ticks × 200ms).
                // A save-locked rejection by Horizon is gone in < 200ms; real
                // gameplay always accumulates several seconds of runtime.
                if (run_ticks >= 5)
                    game_event_signal(last_title);
                last_pid           = 0;
                last_title         = 0;
                run_ticks          = 0;
                s_running_pid.store(0, std::memory_order_relaxed);
                s_running_title_id.store(0, std::memory_order_relaxed);
                fail_count         = 0;
            }
        }
        svcSleepThread(200000000ULL);
    }
}

// ── PSC wake/sleep thread ──────────────────────────────────────────────────────

static void psc_thread_func(void* arg) {
    (void)arg;
    PscPmModule module;
    bool init_ok = false;
    smInitialize();
    if (R_SUCCEEDED(pscmInitialize())) {
        const u32 deps[] = { PscPmModuleId_Fs };
        if (R_SUCCEEDED(pscmGetPmModule(&module, (PscPmModuleId)0x100, deps, 1, true)))
            init_ok = true;
        else
            pscmExit();
    }
    smExit();
    if (!init_ok) return;

    while (true) {
        PscPmState state; u32 flags;
        if (R_SUCCEEDED(pscPmModuleGetRequest(&module, &state, &flags))) {
            switch (state) {
                case PscPmState_ReadySleep:
                case PscPmState_ReadySleepCritical:
                    s_system_sleeping.store(true, std::memory_order_release);
                    svcSleepThread(1500000000ULL);  // 1.5 s: lets progress_cb fire and abort curl
                    pscPmModuleAcknowledge(&module, state);
                    break;
                case PscPmState_ReadyAwaken:
                case PscPmState_ReadyAwakenCritical:
                    pscPmModuleAcknowledge(&module, state);
                    s_system_sleeping.store(false, std::memory_order_release);
                    s_needs_recovery.store(true, std::memory_order_release);
                    break;
                default:
                    pscPmModuleAcknowledge(&module, state);
                    break;
            }
        }
        svcSleepThread(33000000ULL);
    }
    pscPmModuleClose(&module);
    pscmExit();
}

// ── Device-config bootstrap (token auto-delivery) ─────────────────────────────

static char s_last_pairing_code[7] = {0};  // avoid re-writing pairing.json on every poll

static void poll_device_config(FsFileSystem* sd) {
    if (g_config.server_host[0] == '\0') return;

    // Enumerate all Nintendo Account UIDs and build known_profiles JSON array.
    AccountUid all_uids[8];
    s32 count = 0;
    (void)accountListAllUsers(all_uids, 8, &count);

    char profiles_json[1024] = {0};
    int ppos = 0;
    ppos += snprintf(profiles_json + ppos, sizeof(profiles_json) - ppos, "[");
    for (s32 i = 0; i < count; i++) {
        char uid_hex[17];
        snprintf(uid_hex, sizeof(uid_hex), "%016llX",
                 (unsigned long long)all_uids[i].uid[0]);

        char nickname[33] = {0};
        AccountProfile prof;
        if (R_SUCCEEDED(accountGetProfile(&prof, all_uids[i]))) {
            AccountProfileBase base;
            if (R_SUCCEEDED(accountProfileGet(&prof, NULL, &base))) {
                strncpy(nickname, base.nickname, sizeof(nickname) - 1);
                nickname[sizeof(nickname) - 1] = '\0';
                // Sanitize for JSON embedding.
                for (char* p = nickname; *p; p++)
                    if (*p == '"' || *p == '\\' || *p == '\n' || *p == '\r') *p = ' ';
            }
            accountProfileClose(&prof);
        }

        ppos += snprintf(profiles_json + ppos, sizeof(profiles_json) - ppos,
                         "%s{\"profile_id\":\"%s\",\"profile_name\":\"%s\"}",
                         (i > 0 ? "," : ""), uid_hex, nickname);
        if (ppos >= (int)sizeof(profiles_json) - 2) break;
    }
    snprintf(profiles_json + ppos, sizeof(profiles_json) - ppos, "]");

    // Static to avoid large stack frames; poll_device_config is only called from main loop.
    static char catalog_json[CATALOG_MAX * 24 + 4];
    catalog_json[0] = '\0';
    if (s_catalog_dirty)
        catalog_build_json(s_catalog_entries, s_catalog_count, catalog_json, sizeof(catalog_json));

    static char body[1280 + CATALOG_MAX * 24];
    if (s_catalog_dirty && catalog_json[0])
        snprintf(body, sizeof(body), "{\"known_profiles\":%s,\"installed_titles\":%s}",
                 profiles_json, catalog_json);
    else
        snprintf(body, sizeof(body), "{\"known_profiles\":%s}", profiles_json);

    char resp[512] = {0};
    if (http_post_json("/api/v1/sync/device-config", body, resp, sizeof(resp)) <= 0)
        return;

    // Catalog was accepted — update hash and clear dirty flag.
    if (s_catalog_dirty && catalog_json[0]) {
        s_catalog_hash = catalog_hash(s_catalog_entries, s_catalog_count);
        s_catalog_dirty = false;
    }

    // Check for pairing_code (unpaired device awaiting user to enter code on web UI)
    const char* pc_key = strstr(resp, "\"pairing_code\"");
    if (pc_key) {
        const char* pc_colon = strchr(pc_key + 14, ':');
        if (pc_colon) {
            pc_colon++;
            while (*pc_colon == ' ') pc_colon++;
            if (*pc_colon == '"') {
                pc_colon++;
                const char* pc_end = strchr(pc_colon, '"');
                if (pc_end && pc_end - pc_colon == 6) {
                    char code[7] = {0};
                    memcpy(code, pc_colon, 6);
                    // Only write if the code changed — avoids the delete+recreate
                    // cycle that causes the overlay display to flicker.
                    if (strncmp(code, s_last_pairing_code, 6) != 0) {
                        char pairing_json[64];
                        snprintf(pairing_json, sizeof(pairing_json), "{\"code\":\"%s\"}", code);
                        fs_write_text_file(sd, "/switch/omnisave/state/pairing.json", pairing_json);
                        memcpy(s_last_pairing_code, code, 6);
                        fs_log(sd, "PAIRING_CODE_READY");
                    }
                }
            }
        }
        return;
    }

    // Parse {"device_token": "..."} from response.
    const char* key = strstr(resp, "\"device_token\"");
    if (!key) return;
    const char* colon = strchr(key + 14, ':');
    if (!colon) return;
    colon++;
    while (*colon == ' ') colon++;
    if (*colon != '"') return;
    colon++;
    const char* end = strchr(colon, '"');
    if (!end || end == colon) return;

    char new_token[256] = {0};
    size_t tlen = (size_t)(end - colon);
    if (tlen >= sizeof(new_token)) return;
    memcpy(new_token, colon, tlen);
    new_token[tlen] = '\0';

    config_set_device_token(sd, new_token);
    strncpy(g_config.device_token, new_token, sizeof(g_config.device_token) - 1);
    g_config.device_token[sizeof(g_config.device_token) - 1] = '\0';
    // Clear pairing display — device is now paired
    fsFsDeleteFile(sd, "/switch/omnisave/state/pairing.json");
    s_last_pairing_code[0] = '\0';
    fs_log(sd, "TOKEN_AUTO_DELIVERED");
}

// ── Main ───────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    (void)argc; (void)argv;

    if (R_SUCCEEDED(threadCreate(&s_psc_thread, psc_thread_func,
            NULL, NULL, 0x4000, 0x2C, -2)))
        threadStart(&s_psc_thread);

    if (R_SUCCEEDED(threadCreate(&s_game_monitor_thread, game_monitor_func,
            NULL, NULL, 0x4000, 0x2C, -2)))
        threadStart(&s_game_monitor_thread);

    setup();
    fsm_init();
    s_last_activity_posix = get_posix_utc();

    FsFileSystem sd;
    if (R_SUCCEEDED(fsOpenSdCardFileSystem(&sd))) {
        recovery_sweep(&sd, SWEEP_BOOT_CLEAN_ALL);
        fsFsCommit(&sd);
        // Force catalog enumeration + report on first heartbeat.
        s_catalog_hash = 0;
        s_catalog_dirty = false;  // will be set by heartbeat after enumeration
        if (g_config.device_token[0] == '\0')
            poll_device_config(&sd);
        fsFsClose(&sd);
        s_heartbeat_ticks = 1;
    }

    while (true) {
        if (s_system_sleeping.load(std::memory_order_acquire)) {
            svcSleepThread(1000000000ULL);
            continue;
        }

        if (s_needs_recovery.load(std::memory_order_acquire)) {
            s_needs_recovery.store(false, std::memory_order_relaxed);
            fsm_on_wake();
            FsFileSystem rsd;
            if (R_SUCCEEDED(fsOpenSdCardFileSystem(&rsd))) {
                recovery_sweep(&rsd, SWEEP_WAKE_SAFE_ONLY);
                fsFsCommit(&rsd);
                fsFsClose(&rsd);
                s_heartbeat_ticks    = 1;
                s_poll_hot_remain    = (s_poll_hot_remain > 30) ? s_poll_hot_remain : 30;
                s_last_activity_posix = get_posix_utc();
                s_network_was_down   = false;
                s_catalog_hash = 0;  // force re-enumerate + report after wake
            }
        }

        FsFileSystem sd2;
        if (R_FAILED(fsOpenSdCardFileSystem(&sd2))) {
            svcSleepThread(1000000000ULL);
            continue;
        }

        // Drain game-close events from game_monitor_thread (atomic read-and-clear).
        u64 closed_tid = game_event_drain();
        if (closed_tid != 0) {
            s_extract_retry_count = 0;
            s_extract_retry_title = 0;
        } else if (s_extract_retry_title != 0) {
            closed_tid = s_extract_retry_title;
            s_extract_retry_title = 0;
        }
        if (closed_tid != 0) {
            if (s_extract_retry_count == 0)
                fs_log(&sd2, "GAME_CLOSE title=%016llX", (unsigned long long)closed_tid);
            // Write UPLOADING status now so overlay shows "Backing Up" during the
            // pack phase (save_extract blocks the main loop for large saves).
            {
                char ext_key[SNAP_KEY_LEN + 2] = {0};
                snprintf(ext_key, sizeof(ext_key), "000000000000000-%016llX-0000000000000000",
                         (unsigned long long)closed_tid);
                state_write_status(&sd2, UPLOADING, ext_key, 0, 0);
            }
            {
                char label[32] = {0};
                title_label(closed_tid, label, sizeof(label));
                char msg[96];
                snprintf(msg, sizeof(msg), "Reading %s save...", label);
                notif_push("save-read", msg);
            }
            // Enumerate all account UIDs for this title and extract each.
            // Handles multi-user devices: both users' saves are staged separately.
            static char uid_hexes[8][17];
            int nusers = save_enum_title_uids(closed_tid, uid_hexes, 8);
            if (nusers == 0) { nusers = 1; uid_hexes[0][0] = '\0'; }

            int any_new = 0, any_fail = 0;
            for (int u = 0; u < nusers; u++) {
                char out_key[SNAP_KEY_LEN + 2] = {0};
                s_extracting_title.store(closed_tid, std::memory_order_relaxed);
                int rc = save_extract(closed_tid, uid_hexes[u], &sd2, out_key, sizeof(out_key));
                s_extracting_title.store(0, std::memory_order_relaxed);
                if (rc == 1) {
                    any_new = 1;
                    // Evict older outbound entries for the same (tid, uid).
                    char ts_n[20] = {0}, tid_n[17] = {0}, uid_n[17] = {0};
                    if (sscanf(out_key, "%18[^-]-%16[^-]-%16s", ts_n, tid_n, uid_n) == 3) {
                        FsDir odir;
                        if (R_SUCCEEDED(fsFsOpenDirectory(&sd2, OMNI_ROOT "/outbound",
                                FsDirOpenMode_ReadDirs, &odir))) {
                            FsDirectoryEntry ents[ENTRY_BATCH]; s64 cnt = 0;
                            while (R_SUCCEEDED(fsDirRead(&odir, &cnt, ENTRY_BATCH, ents))
                                   && cnt > 0) {
                                for (s64 i = 0; i < cnt; i++) {
                                    if (strlen(ents[i].name) != SNAP_KEY_LEN) continue;
                                    if (strcmp(ents[i].name, out_key) == 0) continue;
                                    char ts_o[20] = {0}, tid_o[17] = {0}, uid_o[17] = {0};
                                    if (sscanf(ents[i].name, "%18[^-]-%16[^-]-%16s",
                                               ts_o, tid_o, uid_o) != 3) continue;
                                    if (strcmp(tid_o, tid_n) != 0 || strcmp(uid_o, uid_n) != 0)
                                        continue;
                                    char evict[FS_MAX_PATH];
                                    path_join(evict, sizeof(evict), OMNI_ROOT "/outbound",
                                              ents[i].name);
                                    fs_log(&sd2, "EXTRACT_EVICT %s superseded by %s",
                                           ents[i].name, out_key);
                                    fsFsDeleteDirectoryRecursively(&sd2, evict);
                                }
                            }
                            fsDirClose(&odir);
                            fsFsCommit(&sd2);
                        }
                    }
                } else if (rc == -1) {
                    any_fail = 1;
                }
            }

            if (any_new) {
                s_dirty               = true;
                s_heartbeat_ticks     = 1;
                s_extract_retry_count = 0;
            } else if (!any_fail) {
                state_write_status(&sd2, IDLE, NULL, 0, 0);
                // All UIDs dedup — save unchanged since last upload/restore.
                s_extract_retry_count = 0;
                char label[32] = {0};
                title_label(closed_tid, label, sizeof(label));
                char msg[96];
                snprintf(msg, sizeof(msg), "%s: No New Changes", label);
                notif_push("backup-dedup", msg);
            } else if (s_extract_retry_count < 3) {
                state_write_status(&sd2, IDLE, NULL, 0, 0);
                s_extract_retry_count++;
                s_extract_retry_title = closed_tid;
                fs_log(&sd2, "EXTRACT_RETRY %d/3 title=%016llX",
                       s_extract_retry_count, (unsigned long long)closed_tid);
                s_heartbeat_ticks = 1;
            } else {
                state_write_status(&sd2, IDLE, NULL, 0, 0);
                s_extract_retry_count = 0;
                {
                    char fail_label[32] = {0};
                    title_label(closed_tid, fail_label, sizeof(fail_label));
                    char fail_msg[96];
                    snprintf(fail_msg, sizeof(fail_msg), "%s: Backup Failed", fail_label);
                    notif_push("backup-fail", fail_msg);
                }
                fs_log(&sd2, "EXTRACT_FAIL title=%016llX",
                       (unsigned long long)closed_tid);
            }
            fsFsCommit(&sd2);
        }

        // Overlay "Backup All" signal.
        FsFile cmd_f;
        if (s_running_pid.load(std::memory_order_relaxed) == 0 &&
                R_SUCCEEDED(fsFsOpenFile(&sd2, OMNI_ROOT "/signals/batch_backup.request",
                                         FsOpenMode_Read, &cmd_f))) {
            fsFileClose(&cmd_f);
            fsFsDeleteFile(&sd2, OMNI_ROOT "/signals/batch_backup.request");
            fsFsCommit(&sd2);
            notif_push("dump-start", "Backing Up All Saves...");
            dump_all_saves();
            s_dirty           = true;
            s_heartbeat_ticks = 1;
            s_catalog_hash    = 0;  // dump may have changed catalog; force re-enumerate
        }

        // Catalog enumeration: only while idle (avoid save-reader contention during transfers).
        {
            FsmState cur_state = fsm_get_state();
            if (cur_state == IDLE || cur_state == RETRY_BACKOFF) {
                CatalogEntry new_entries[CATALOG_MAX];
                int new_count = catalog_enumerate(new_entries, CATALOG_MAX);
                u64 new_hash  = catalog_hash(new_entries, new_count);
                if (new_hash != s_catalog_hash) {
                    memcpy(s_catalog_entries, new_entries, (size_t)new_count * sizeof(CatalogEntry));
                    s_catalog_count = new_count;
                    s_catalog_dirty = true;
                }
            }
        }

        if (g_config.device_token[0] == '\0')
            poll_device_config(&sd2);
        else if (s_catalog_dirty)
            poll_device_config(&sd2);

        InputSnapshot snap;
        build_snapshot(&sd2, &snap);
        fsm_tick(&sd2, &snap);

        fsFsCommit(&sd2);
        fsFsClose(&sd2);
        FsmState cur = fsm_get_state();
        bool active = (cur != IDLE && cur != RETRY_BACKOFF);
        if (!active && s_pending_extract_title.load(std::memory_order_relaxed) == 0)
            svcSleepThread(1000000000ULL);
    }

    return 0;
}
