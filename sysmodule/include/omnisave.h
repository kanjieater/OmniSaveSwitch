#pragma once
#include <switch.h>
#include <curl/curl.h>
#ifdef __cplusplus
#include <atomic>
#endif

// ── Build constants ────────────────────────────────────────────────────────────

#define OMNISAVE_VERSION    "2.0.0"
#define DEFAULT_SERVER_PORT  8991
#define ENTRY_BATCH          8
#define SNAP_KEY_LEN         52   // "YYYYMMDD_HHMMSS_nn-TTTTTTTTTTTTTTTT-UUUUUUUUUUUUUUUU"
                                  //  nn = per-process monotonic counter (00-99); prevents
                                  //  same-second key collision between game-close extraction
                                  //  and delivery pre-inject preservation.
#define OMNI_ROOT            "/switch/omnisave"

// ── V2 transport constants ─────────────────────────────────────────────────────

#define CHECKPOINT_SIZE  (4  * 1024 * 1024)   // 4 MB — xxHash32 granularity
#define WINDOW_SIZE      (64 * 1024 * 1024)   // 64 MB — HTTP transport granularity
#define MAX_CHECKPOINTS  256                   // max 1 GB saves

// ── V2 checkpoint ledger ───────────────────────────────────────────────────────

typedef struct {
    uint32_t h[MAX_CHECKPOINTS];
    int      count;
} CheckpointLedger;

// ── Save stat signature (preflight change detection) ──────────────────────────

typedef struct {
    uint32_t file_count;
    uint64_t total_bytes;
    uint64_t newest_mtime;   // 0 when filesystem doesn't expose timestamps
} SaveSignature;

// ── FSM states ─────────────────────────────────────────────────────────────────

typedef enum {
    IDLE          = 0,
    UPLOADING     = 1,
    DOWNLOADING   = 2,
    INBOUND_READY = 3,
    DELIVERING    = 4,
    RETRY_BACKOFF = 5,
} FsmState;

static inline const char* fsm_state_name(FsmState s) {
    switch (s) {
        case IDLE:          return "IDLE";
        case UPLOADING:     return "UPLOADING";
        case DOWNLOADING:   return "DOWNLOADING";
        case INBOUND_READY: return "INBOUND_READY";
        case DELIVERING:    return "DELIVERING";
        case RETRY_BACKOFF: return "RETRY_BACKOFF";
        default:            return "UNKNOWN";
    }
}

// ── Tick input snapshot (frozen at tick start) ─────────────────────────────────

typedef struct {
    bool has_outbound;
    char outbound_key[SNAP_KEY_LEN + 2];

    bool has_inbound;
    char inbound_key[SNAP_KEY_LEN + 2];
    int  inbound_snap_id;

    bool game_running;
    u64  running_title_id;

    bool storage_critical;
    bool heartbeat_due;
} InputSnapshot;

// ── Server config ──────────────────────────────────────────────────────────────

typedef struct {
    char server_scheme[8];
    char server_host[256];
    u16  server_port;
    char device_token[256];   // optional; if set, sent as Authorization: Bearer header
    bool verify_tls;          // true = verify peer cert when scheme is https; set false for self-signed certs
    char device_suffix[32];   // optional; appended as "-<suffix>" to MAC to distinguish SysNAND/EmuNAND
} OmniSaveConfig;

extern OmniSaveConfig g_config;
extern bool g_debug;
extern bool g_verbose_notif;

// ── Globals (defined in main.cpp) ─────────────────────────────────────────────

extern char s_fw_version[32];
extern char s_device_id[64];   // MAC or cal ID: "AA:BB:CC:DD:EE:FF"
extern char s_hw_type[16];
extern char s_uid_hex[17];
extern char s_account_nickname[33];
extern char s_device_nickname_hint[32];
extern u8   s_copy_buf[65536];

extern volatile bool s_dirty;             // set when new work staged to outbound
#ifdef __cplusplus
extern std::atomic<bool> s_system_sleeping;
extern std::atomic<bool> s_needs_recovery;
extern std::atomic<uint64_t> s_pending_extract_title; // set by game_monitor on game close
extern std::atomic<uint64_t> s_extracting_title;      // non-zero while save_extract holds the save FS open
extern std::atomic<u64> s_running_pid;
extern std::atomic<u64> s_running_title_id;
#endif
extern int  s_heartbeat_ticks;            // countdown to next server poll
extern int  s_poll_hot_remain;            // HOT burst ticks remaining (2s each)
extern u64  s_last_activity_posix;        // POSIX time of last real work; drives decay
extern bool s_network_was_down;           // set when network unavailable; cleared on reconnect
extern char g_dns_servers[64];

extern u32 s_key_seq;        // monotonic counter; incremented at every key generation site
extern u64 s_save_data_size;
extern u64 s_save_data_id;
extern u8  s_save_space_id;

// ── main.cpp helpers ───────────────────────────────────────────────────────────

u64  get_posix_utc(void);
void format_local_time(char* out, size_t sz, u64 posix);
void make_ts_folder(char* out, size_t sz, u64 posix);
bool get_title_name(u64 title_id, char* out, size_t sz);
void title_label(u64 title_id, char* out, size_t sz);
void refresh_dns_servers(void);
bool wait_network_ready(void);

// ── crc32.cpp ─────────────────────────────────────────────────────────────────

void     crc_init(void);
uint32_t crc_run(uint32_t c, const void* d, size_t n);

// ── fs_helpers.cpp ─────────────────────────────────────────────────────────────

void fs_write_text_file(FsFileSystem* fs, const char* path, const char* text);
bool fs_read_text_file(FsFileSystem* fs, const char* path, char* buf, size_t buf_sz);
void fs_log(FsFileSystem* fs, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void ensure_dir(FsFileSystem* fs, const char* path);
void path_join(char* out, size_t sz, const char* dir, const char* name);
bool copy_file(FsFileSystem* src_fs, const char* src_path,
               FsFileSystem* dst_fs, const char* dst_path);
bool copy_dir(FsFileSystem* src_fs, const char* src_path,
              FsFileSystem* dst_fs, const char* dst_path);

// ── config.cpp ─────────────────────────────────────────────────────────────────

void config_load(FsFileSystem* fs);
void config_set_device_token(FsFileSystem* fs, const char* token);
// Appends "-<device_suffix>" to buf when device_suffix is non-empty.
void config_apply_device_suffix(char* buf, size_t buf_sz);

// ── http.cpp ───────────────────────────────────────────────────────────────────

extern char g_http_last_err[CURL_ERROR_SIZE];

// Session-scoped handles — create once before window loop, reuse across windows, close after.
CURL* http_upload_handle_create(void);
CURL* http_download_handle_create(void);
void  http_handle_close(CURL* c);

// V2: PUT raw bytes from file at offset; returns server_verified_bytes, -1=err, -2=sleep.
// c must be a handle from http_upload_handle_create().
s64 http_put_window(CURL* c, const char* url_path, FsFileSystem* sd,
                    const char* sd_path, s64 offset, s64 window_len,
                    char* resp_buf, int resp_sz);

// V2: offset-param GET with inline xxHash32 accumulator; returns new local verified_bytes.
// Returns verified_bytes (unchanged) on hash mismatch — caller truncates to that offset.
// Returns -1 on transport err, -2 on sleep abort.
// c must be a handle from http_download_handle_create().
s64 http_get_window_validated(CURL* c, const char* url_base_path,
                               FsFileSystem* sd, const char* sd_path,
                               s64 verified_bytes, s64 window_size,
                               const CheckpointLedger* ledger,
                               s64 checkpoint_size, s64 total_bytes);

int  http_post_empty(const char* url_path, char* resp_buf, int resp_sz);
int  http_post_json(const char* url_path, const char* json_body,
                    char* resp_buf, int resp_sz);
int  http_get_body(const char* url_path, char* resp_buf, int resp_sz);
int  http_ping(const char* path);

// ── transport.cpp / transport_upload.cpp / transport_poll.cpp ──────────────────

// Returns: 1=success, 0=transport failure, -1=cooperative yield, -2=auth rejected (401)
int  transport_upload(FsFileSystem* sd, const char* key, int* out_snap_seq,
                      bool preservation = false);
int  transport_poll_inbound(FsFileSystem* sd);  // returns -2 on 401 (token revoked)
void transport_ack(FsFileSystem* sd, const char* key);
void transport_error_fail(FsFileSystem* sd, const char* key);

// ── save_ops.cpp ───────────────────────────────────────────────────────────────

// uid_hex: if non-empty, match only the account with that UID hex; if empty, first match.
Result open_save_fs(u64 title_id, const char* uid_hex, FsFileSystem* out);
// Returns count of account UIDs that have a save for title_id (≤ max_uids).
int    save_enum_title_uids(u64 title_id, char uid_hexes[][17], int max_uids);
// Returns: 1=extracted (new outbound staged), 0=dedup (save unchanged), -1=failure
// uid_hex: if non-empty, extract only the save owned by that account UID.
int    save_extract(u64 title_id, const char* uid_hex, FsFileSystem* sd,
                    char* out_key, size_t key_sz);
// Returns: 1=ok, 0=transient failure (retry), -1=permanent failure (game not installed)
// target_user_key: if non-empty, try to match a local account with that UID hex; fall back
// to last-opened user if not found, logging USER_CONTEXT_FALLBACK.
int    save_inject(const char* key, FsFileSystem* sd, const char* target_user_key);
void   dump_all_saves(void);

// ── save_hash.cpp ──────────────────────────────────────────────────────────────

// CRC32 fingerprint — updated after upload/inject success.
bool snap_hash_read(FsFileSystem* sd, u64 title_id, const char* uid_hex, u32* out);
void snap_hash_write(FsFileSystem* sd, u64 title_id, const char* uid_hex, u32 fp);
// Reads ZIP central-directory CRC and stores it; called by fsm.cpp and save_inject.
void snap_hash_update_from_archive(FsFileSystem* sd, const char* key, const char* path);
// Stat signature — updated when dedup or new outbound is confirmed.
bool sig_read(FsFileSystem* sd, u64 title_id, const char* uid_hex, SaveSignature* out);
void sig_write(FsFileSystem* sd, u64 title_id, const char* uid_hex, const SaveSignature* sig);

// ── state.cpp ──────────────────────────────────────────────────────────────────

void state_init(FsFileSystem* sd);
void state_write_status(FsFileSystem* sd, FsmState fsm, const char* key,
                        s64 verified_bytes, s64 total_bytes);
void state_write_last_backup(FsFileSystem* sd, const char* title_id,
                              const char* snap_key, int counter);
void state_write_last_restore(FsFileSystem* sd, const char* title_id,
                               const char* snap_key, int counter);
void state_write_lineage(FsFileSystem* sd, const char* title_id, const char* uid,
                         const char* snap_id, int counter);
bool state_read_lineage(FsFileSystem* sd, const char* title_id, const char* uid,
                        char* base_snap_out, size_t snap_sz, int* counter_out);
uint32_t state_get_sync_generation(void);
void     state_set_sync_generation(FsFileSystem* sd, uint32_t gen);
void     state_apply_backup_updates(FsFileSystem* sd, const char* arr);
void     state_atomic_write(FsFileSystem* sd, const char* path, const char* text);
void     state_append_event(FsFileSystem* sd, const char* level, const char* msg);

// ── recovery.cpp ───────────────────────────────────────────────────────────────

typedef enum {
    SWEEP_BOOT_CLEAN_ALL,   /* boot: always purge tmp_out */
    SWEEP_WAKE_SAFE_ONLY    /* wake: only purge tmp_out if .active marker absent */
} RecoverySweepMode;

void recovery_sweep(FsFileSystem* sd, RecoverySweepMode mode);

// ── save_pack.cpp ──────────────────────────────────────────────────────────────

// src_fs and dst_fs must be different filesystem handles (can't pack into the
// same fs being read — Horizon won't allow concurrent dir-read + file-write).
bool pack_save(FsFileSystem* src_fs, const char* src_dir,
               FsFileSystem* dst_fs, const char* out_path,
               u64 save_data_size, u32* fp_out, int* out_file_count);
bool unpack_save(FsFileSystem* sd, const char* packed_path,
                 FsFileSystem* save_fs, u64* save_data_size_out);
bool zip_fingerprint(FsFileSystem* sd, const char* path, u32* fp_out);

// ── fsm.cpp ────────────────────────────────────────────────────────────────────

void     fsm_init(void);
void     fsm_on_wake(void);
void     fsm_tick(FsFileSystem* sd, const InputSnapshot* snap);
FsmState fsm_get_state(void);
bool     fsm_is_key_deferred(const char* key);
bool     fsm_is_title_inject_blocked(const char* title_id);

// ── notif.cpp ──────────────────────────────────────────────────────────────────

void notif_push(const char* event_slug, const char* message);
void notif_verbose(const char* event_slug, const char* message);
