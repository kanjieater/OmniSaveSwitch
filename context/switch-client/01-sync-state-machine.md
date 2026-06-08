# 01-sync-state-machine.md — OmniSave Switch Client State Machine

**Status:** System Specification
**Component:** Switch Sysmodule (Execution Layer)
**Purpose:** Defines the deterministic synchronization lifecycle, transport scheduling, transition invariants, and resource governance required for safe background execution.

---

## 1. System States

The Sysmodule execution layer operates as an event-driven FSM.

| State | Description |
| --- | --- |
| **IDLE** | System waiting; sleepable state with zero continuous polling. |
| **UPLOADING** | Active transmission of chunks from `outbound/` to Server (network phase). |
| **DOWNLOADING** | Active chunked download of a restore payload from Server to `tmp_in/` (network phase). |
| **INBOUND_READY** | All restore chunks assembled; manifest promoted to `inbound/`; awaiting game-exit window. |
| **DELIVERING** | Atomic swap/injection in progress (or `tmp_in/` staging). |
| **RETRY_BACKOFF** | Transport failed; waiting for exponential backoff timer. |
| **FAILED** | Degraded state; suspends execution transitions until `recovery_sweep()`. |

---

## 2. Transition Rules (Event-Driven)

All state transitions are logged. The execution layer is single-threaded and enforces serialization.

* **IDLE → UPLOADING:** Triggered by `pmdmnt` exit event OR `recovery_sweep()` detection of existing `outbound/` payloads.
* **IDLE → DOWNLOADING:** Triggered by `GET /queue` returning a non-empty `pending_restores` list.
* **UPLOADING → IDLE:** Triggered by `POST /sessions/{id}/commit` returning `202 Accepted`.
* **UPLOADING → RETRY_BACKOFF:** Triggered by transport failure (network drop/timeout).
* **DOWNLOADING → INBOUND_READY:** Triggered by all chunks received, written to `tmp_in/`, and atomically promoted to `inbound/` via rename.
* **DOWNLOADING → RETRY_BACKOFF:** Triggered by transport failure during chunk download.
* **INBOUND_READY → DELIVERING:** Triggered by verification check (Game Process Not Running).
* **DELIVERING → IDLE:** Triggered by successful atomic inject and `POST /ack`.
* **ANY → FAILED:** Triggered by critical FS error, storage limit (>95%), forbidden conflict/lineage error, or unknown error code.
* **FAILED → IDLE:** Triggered by `recovery_sweep()` **only if** the underlying blocking condition is resolved.

---

## 3. Execution Loop & Tick Contract

The Sysmodule runs on a discrete, event-driven execution loop.

### 3.1 The Execution Tick Sequence

Every execution cycle (tick) MUST follow this strict sequence:

1. **Input Collection (Snapshot Phase):** Read external state (Filesystem scan, Network responses, Hints). All inputs are frozen as a logical snapshot for the duration of the tick.
2. **Scheduler Update:** Update scheduler status based on the snapshot of `outbound/` or `inbound/` presence.
3. **FSM Evaluation:** Re-evaluate transition eligibility based strictly on the snapshot.
4. **Transition Commit:** Re-validate target paths, then execute **at most one** state transition.

### 3.2 Filesystem I/O & Scanning Policy

* **Event-Driven Checks:** During active windows, the sysmodule MUST rely on OS events (`pmdmnt`) or memory-cached dirty flags to trigger directory scans.
* **No Idle Scans:** If the FSM is in `IDLE` and no dirty flag is set, `outbound/` and `inbound/` MUST NOT be scanned, **except** during the scheduled 5-minute reconciliation heartbeat (see 4.3).
* **Input Snapshot Consistency:** Any mutation to the filesystem or network state occurring *during* a tick is not visible to the FSM until the subsequent tick. `tmp_` directories MUST be ignored for decision-making.

---

## 4. Transport, Scheduling & Power Policy

The `IDLE` state is a **sleepable state**, not a continuous loop. The thread yields entirely between scheduled events.

### 4.1 Polling Cadences & Network Modes

* **Cold Poll Mode (`POLL_IDLE`):** 5-minute heartbeat. Minimal HTTP GET. Socket closed immediately.
* **Active Mode (`POLL_ACTIVE`):** 15-second window. Triggered only if `inbound/` or `outbound/` has work, or a server hint is received. Socket kept alive.
* **Recent Mode (`POLL_RECENT`):** 2-minute window (4 slots) after successful upload/restore.
* **Backoff Mode (`POLL_RETRY`):** {5s, 10s, 30s, 60s}. Resets to IDLE upon success.

### 4.2 Server Execution Hint Contract

Hints are observational accelerators.

* **Tick Coalescing:** All hints received within a single tick are merged and evaluated once.
* **Hint Cooldown:** Identical hints (`wake_signal`, `queue_hint`) are ignored if received within a 60-second cooldown window.

### 4.3 Safety vs. Liveness (The Dual-Path Model)

The system guarantees correctness through reconciliation, and guarantees low-latency through events. These two paths are strictly decoupled.

* **The Fast-Path (Liveness):** Driven by non-authoritative acceleration signals (dirty flags, Server Hints). The ~5s reaction time SLA applies **only** when the system is awake, the FSM is warm, and OS event coherence is intact.
* **The Reconciliation-Path (Safety):** Driven by the scheduled 5-minute `POLL_IDLE` heartbeat. **This heartbeat unconditionally forces a local directory scan and a server cold-poll**, overriding the dirty-flag requirement. This is the ultimate safety net for missed events.
* **Degraded Detection State:** If an OS event or hint is dropped, the system functionally degrades to the Reconciliation-Path. The system is blind until the next 5-minute unconditional scan discovers the state change.

---

## 5. Resilience, Sleep & Restartability

### 5.1 Sleep/Wake Contract

* **Sleep Entry:** Upon receiving a system sleep notification, the sysmodule immediately aborts all active sockets, pauses the FSM, and halts all execution ticks. **There are zero execution ticks during sleep.**
* **Wake Envelope Rule & Immediate Tick:** On every wake event, the FSM remains logically frozen. The client first runs `recovery_sweep()` to reconstruct local FS truth. It then performs a single server state fetch (pulling pending hints and manifests) to build an immutable **Wake Envelope snapshot**.
* **Forced Execution:** Once the Wake Envelope is constructed, the Sysmodule MUST immediately dispatch a forced execution tick to process it, precluding the need to wait for a scheduled tick slot. This guarantees the ~5s restore SLA upon device wake.

### 5.2 Recovery Sweep (`recovery_sweep`)

Triggered ONLY on: (1) Startup, (2) PSC wake notification, (3) Server `wake_signal`, (4) Transitioning out of `FAILED`.
**Logic:**

1. **Integrity:** Check `outbound/` and `inbound/`.
2. **Staging:** Purge all `tmp_` directories.
3. **Failure Recovery:** If `FAILED`, check health. If healthy, transition to `IDLE`.

---

## 6. Formal Architectural Invariants

1. **The Battery Invariance Rule:** No subsystem may wake the CPU or network radio unless there exists (a) queued local work, (b) a server hint, or **(c) a scheduled reconciliation tick (heartbeat).**
2. **Acceleration Signal Invariant:** Event signals (dirty flags, server hints, OS callbacks) are strictly non-authoritative acceleration signals. They must never be treated as correctness dependencies.
3. **Reconciliation Ownership:** Reconciliation is the final authority for restore initiation. The 5-minute heartbeat unconditionally bypasses dirty flags to guarantee eventual consistency.
4. **Execution Tick Atomicity:** Only one FSM transition allowed per tick; no chained state logic.
5. **Input Snapshot Consistency:** All filesystem and network inputs MUST be treated as a single frozen logical snapshot captured at tick start.
6. **Stateless Lifecycle:** FSM is stateless across process lifecycles; all state is derived from external systems.
7. **Snapshot Immutability:** Once a snapshot is assigned a sequence number, its data and lineage are immutable.
8. **Conflict Blindness:** If divergence is detected, the client halts HEAD advancement.
9. **Atomic Injection:** Restores are atomic. If injection fails, the system reverts.
10. **Chunk Resumption Authority:** The client queries the server for "resume offsets"; it does not track progress locally.
11. **Single-Writer Invariant:** Only the Sysmodule writes to `outbound/`, `inbound/`, `tmp_`, and `errors/`.
12. **Read-Only Safety:** 95% disk usage forces Read-Only mode; recovery is fully automatic.

---

## 7. Deterministic Guarantee

The system is **statically partitioned**.

* The **Sysmodule** performs the work via atomic staging.
* The **Overlay** observes the work (Snapshot).
* The **Server** judges the work (Conflict resolution/HEAD assignment).

No component is permitted to perform the role of another. Every component relies solely on the File-Based IPC contract, the Server-provided manifests, and the persistent `outbound/` queue state.

## 8. First-Boot Behavior

On first boot OR when `state/lineage.json` has no entry for a given `(title_id, switch_user_id)`:

* The FSM has no special bootstrap state. There is no enrollment flow.
* On game exit, the sysmodule proceeds with the normal `UPLOADING` flow.
* `POST /transactions/inbound` is called with `parent_sequence_num: null`.
* The server treats a null parent as an initial seed — it creates the lineage anchor, advances HEAD, and the upload completes normally.
* On success, `state/lineage.json` is written with the returned sequence number and the sysmodule enters the normal sync loop.

**Invariant:** The FSM does not model lifecycle states or make enrollment decisions locally. It is fully reactive. All lineage assignment and identity resolution are determined exclusively by the server response.

## 9. Transport Symmetry (UPLOADING vs DOWNLOADING)

`UPLOADING` and `DOWNLOADING` are symmetric network transport phases. Both use chunked, resumable transfers with server-authoritative progress.

| Phase | Direction | Chunks | Progress Field |
| --- | --- | --- | --- |
| `UPLOADING` | Client → Server | `PUT /sessions/{id}/chunks/{n}` | `uploaded_chunks` |
| `DOWNLOADING` | Server → Client | `GET /transactions/{id}/chunks/{n}` | `downloaded_chunks` |
| `DELIVERING` | Local inject only | None | None |

`DELIVERING` is a short, atomic local filesystem operation. It has no network calls, no progress percentage, and no chunk tracking. See `04-device-sync-api.md` for full protocol details.