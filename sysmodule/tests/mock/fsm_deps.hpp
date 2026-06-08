#pragma once
// Extern declarations for all mock control variables.
// Tests set these to drive stub behaviour for FSM dependencies.

#include "switch.h"

// ── transport_upload ───────────────────────────────────────────────────────────
// Returned from transport_upload(); >0 = snap_id (success), 0 = failure, -1 = yield.
extern int  g_mock_upload_rc;

// ── transport_poll_inbound ─────────────────────────────────────────────────────
// 0=success, -2=401 token revoked
extern int  g_mock_poll_inbound_calls;
extern int  g_mock_poll_inbound_rc;

// ── state_write_status ─────────────────────────────────────────────────────────
extern int  g_mock_status_write_calls;

// ── save_inject ────────────────────────────────────────────────────────────────
// 1 = success, 0 = transient failure, -1 = permanent failure.
extern int  g_mock_inject_rc;

// ── transport_ack / error ──────────────────────────────────────────────────────
extern int  g_mock_ack_calls;
extern int  g_mock_error_fail_calls;

// ── network / time ─────────────────────────────────────────────────────────────
extern int  g_mock_network_ready;  // 0 = not ready, 1 = ready
extern u64  g_mock_posix_utc;      // returned by get_posix_utc()

// ── state_read_lineage ─────────────────────────────────────────────────────────
extern int  g_mock_lineage_counter_out;

// ── save_extract ───────────────────────────────────────────────────────────────
// 0=dedup (save unchanged), 1=new outbound staged, -1=error
extern int  g_mock_save_extract_rc;

// ── Reset all mocks to sensible defaults ──────────────────────────────────────
void mock_reset(void);
