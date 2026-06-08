# OmniSave V1: Switch Sysmodule Backend

**Status:** Final Specification
**Component:** Switch Sysmodule (Execution Layer)
**Purpose:** Defines the architectural responsibilities, OS interaction boundaries, and module ownership for the OmniSave background sysmodule.

---

## 1. Responsibilities & Ownership

The sysmodule is the **sole execution agent** on the Switch. No other process may write to OmniSave SD card directories or perform HTTP transport.

| Domain | Owner | Notes |
| --- | --- | --- |
| Save FS extraction | Sysmodule | Captures Nintendo Account UID via `fslib` |
| Chunked upload | Sysmodule | `PUT /sessions/{id}/chunks/{n}`, commit session |
| Chunked download | Sysmodule | `GET /queue`, claim, `GET /transactions/{id}/chunks/{n}` |
| Local inject | Sysmodule | Atomic swap into game save FS |
| All SD card directory writes | Sysmodule | `outbound/`, `inbound/`, `tmp_`, `state/`, `errors/`, `signals/` |
| `state/status.json` writes | Sysmodule | Once per tick; atomic rename |
| `signals/batch_backup.request` consumption | Sysmodule | Reads and deletes signal on tick |
| Overlay reads | Overlay | Read-only; only writes `signals/batch_backup.request` |
| Conflict resolution | Server | Client is conflict-blind |

---

## 2. Horizon OS Service Interactions

| Service | Event | Sysmodule Response |
| --- | --- | --- |
| `pmdmnt` | Game process exit | Trigger save extraction → `UPLOADING` |
| `psc` | System sleep notification | Abort active sockets; halt FSM ticks |
| `psc` | System wake notification | Run `recovery_sweep()`; build Wake Envelope; dispatch forced tick |
| `appletmng` | Applet state | Observed for game-running check before `DELIVERING` |

---

## 3. Runtime Execution Model

The sysmodule runs as a background process. A single execution thread owns the FSM. All HTTP I/O is synchronous within a tick's network operation window.

### 3.1 Thread Ownership

* **Main thread:** FSM tick loop, all SD card I/O, all HTTP calls.
* **Event thread(s):** OS event listeners (`pmdmnt`, `psc`). These set dirty flags and memory-cached hints only — they never mutate FSM state directly.
* **No dynamic allocation** (`malloc`/`new`) during active FSM states. All buffers are pre-allocated at sysmodule startup.

### 3.2 Module Layout (`sysmodule/source/`)

| File | Responsibility |
| --- | --- |
| `main.cpp` | Entry point; event threads; `InputSnapshot` assembly; calls `fsm_tick()` |
| `fsm.cpp` | All state transitions; owns the FSM state variable |
| `transport_upload.cpp` | Upload pipeline: `POST /transactions/inbound`, `PUT /sessions/{id}/chunks/{n}`, `POST /sessions/{id}/commit` |
| `transport_poll.cpp` | Restore pipeline: `GET /queue`, `POST /queue/claim`, `GET /transactions/{id}/chunks/{n}`, `POST /ack` |
| `transport.cpp` | Shared HTTP helpers (libcurl wrappers) |
| `save_ops.cpp` | Save FS mount/extract/inject using Nintendo Account UID; save dedup via fingerprint |
| `save_pack.cpp` | Deterministic ZIP(STORE) writer: enum → lexsort → stream → CRC back-patch → CDH → EOCD |
| `save_pack_read.cpp` | ZIP(STORE) reader: forward-only unpack with CRC32 verification; `zip_fingerprint()` |
| `save_dump.cpp` | `dump_all_saves()`: bulk export of all account saves to `outbound/` |
| `recovery.cpp` | `recovery_sweep()`: purge `tmp_`, validate `outbound/`/`inbound/`, reset FSM to `IDLE` |
| `state.cpp` / `status.cpp` | Atomic JSON writes to `state/` via tmp-file rename |
| `config.cpp` | Loads `server_address` from `config.ini` |
| `http.cpp` | libcurl wrappers: `http_put_file`, `http_post_json`, `http_get_body`, `http_get_to_file`, `http_ping` |
| `fs_helpers.cpp` | `path_join`, `copy_dir`, `fs_write_text_file`, `fs_log` |
| `notif.cpp` | In-game notification display |

---

## 4. Poll Scheduler Ownership

The sysmodule owns all polling cadence decisions. The overlay and server have no authority over poll scheduling.

Cadences — `POLL_IDLE` (5-min heartbeat), `POLL_ACTIVE` (15-sec when work is pending), `POLL_RECENT` (2-min after success), `POLL_RETRY` (backoff) — are managed internally. See `01-sync-state-machine.md` §4 for the full scheduling contract.

---

## 5. Recovery Sweep Contract

`recovery_sweep()` is synchronous and MUST complete before any network call is made.

Operations in order:
1. Purge all `tmp_out/` and `tmp_in/` contents.
2. Validate all `outbound/` manifests (JSON parse + required fields). Discard corrupted entries.
3. Validate all `inbound/` manifests. Discard any with `expires_at` in the past.
4. If `DELIVERING` was active pre-crash: discard `inbound/` manifest; rely on next `GET /queue` poll to re-deliver.
5. If `UPLOADING` or `DOWNLOADING` was active pre-crash: leave `outbound/` manifest intact; re-execute from server-authoritative resume offset on next tick.
6. Clear FSM to `IDLE`.

---

## 6. Notification Emission

The sysmodule appends to `state/notifications.json` via atomic rename when:
* Storage exceeds 95% threshold.
* Upload or download fails after exhausting retries (FSM → `RETRY_BACKOFF` threshold exceeded).
* `DELIVERING` fails (atomic inject error).

Notifications are cleared by the sysmodule only after the underlying condition resolves or the user acknowledges via an overlay IPC signal. The overlay reads and displays them; it never writes to `state/notifications.json` directly.
