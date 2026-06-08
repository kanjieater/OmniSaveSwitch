// Definitions for all globals declared extern in omnisave.h but owned by
// main.cpp (which is not compiled in the test build).
// Note: g_config and g_debug are owned by config.cpp (now in the test build).
// Note: fs_write_text_file / fs_log / fs_read_text_file / ensure_dir are in fsm_deps.cpp.
#include "omnisave.h"

bool g_verbose_notif  = false;

char s_fw_version[32]            = "test-fw";
char s_device_id[64]             = "AA:BB:CC:DD:EE:FF";
char s_hw_type[16]               = "Switch";
char s_uid_hex[17]               = "0000000000000001";
char s_account_nickname[33]      = "TestUser";
char s_device_nickname_hint[32]  = "TestDevice";
u8   s_copy_buf[65536];

volatile bool s_dirty                 = false;
std::atomic<bool> s_system_sleeping{false};
std::atomic<bool> s_needs_recovery{false};
std::atomic<uint64_t> s_pending_extract_title{0};
std::atomic<uint64_t> s_extracting_title{0};
int           s_heartbeat_ticks       = 0;

std::atomic<u64> s_running_pid{0};
std::atomic<u64> s_running_title_id{0};
char g_dns_servers[64]  = "";

u64 s_save_data_size = 0;
u64 s_save_data_id   = 0;
u8  s_save_space_id  = 0;

char g_http_last_err[256] = "";

int  s_poll_hot_remain    = 0;
u64  s_last_activity_posix = 0;
bool s_network_was_down   = false;

