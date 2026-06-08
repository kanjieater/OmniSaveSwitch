Here is the finalized `04-overlay-ui-spec.md` exactly as we locked it down.

---

# 04-overlay-ui-spec.md — OmniSave V1: Switch Client Overlay & Status Contracts

**Status:** Final Specification
**Component:** Switch Overlay (Tesla Menu / libtesla) & Sysmodule IPC
**Purpose:** Defines the read-only observability layer, layout constraints, the filesystem-based IPC triggers for the UI, and the strict backend data contracts required to populate them safely.

---

## 1. Top Section: Current Status (Dynamic)

This section provides real-time visibility into the sysmodule's current execution state. It continuously polls `state/status.json` at a low frequency (e.g., 1Hz) to update the UI without blocking.

### 1.1 FSM to User-Facing Translations & Layout

Due to strict horizontal width constraints in the Tesla overlay, all dynamic status fields MUST render vertically. Horizontal compound strings (e.g., "Backing up: Game (42%)") are strictly forbidden.

**IDLE (Empty Queues)**

* Line 1: `Up to Date`

**UPLOADING (Network Phase)**

* Line 1: `Backing Up`
* Line 2: `[Game Name]`
* Line 3: `[XX]%` (Omitted if unavailable or `total_chunks <= 0`)

**DOWNLOADING (Network Phase)**

* Line 1: `Downloading`
* Line 2: `[Game Name]`
* Line 3: `[XX]%` (Omitted if unavailable or `total_chunks <= 0`)

**DELIVERING (Local Filesystem Phase)**

* Line 1: `Applying Save`
* Line 2: `[Game Name]`
* *Line 3 is intentionally omitted. No percentages are shown during local I/O.*

**RETRY_BACKOFF**

* Line 1: `Network issue.`
* Line 2: `Retrying shortly...`

**READ_ONLY**

* Line 1: `Paused: Storage Full`
* Line 2: `(SD Card >95%)`

### 1.2 The Progress Scope & Chunk Contract Rule (Unified Transport Model)

Progress percentages apply **only** to network transport phases (`UPLOADING` and `DOWNLOADING`). All local filesystem phases (including extraction, injection, and application) are explicitly non-progress-renderable.

#### 1.2.1 Unified Chunk Progress Contract

The sysmodule exposes two symmetric counters in `state/status.json`:

| FSM State | Counter Field | Meaning |
| --- | --- | --- |
| **UPLOADING** | `uploaded_chunks` | Last contiguous ACKed outbound chunk |
| **DOWNLOADING** | `downloaded_chunks` | Last contiguous ACKed inbound chunk |

Both counters MUST obey identical semantics:

* Strictly contiguous ACK progression only.
* No speculative or optimistic updates.
* No byte-based interpolation.
* No filesystem-derived estimation.

#### 1.2.2 Progress Calculation Rule

For BOTH `UPLOADING` and `DOWNLOADING`:
`progress_pct = floor((active_counter / total_chunks) * 100)`

Where:

* `active_counter` = The state-dependent counter (`uploaded_chunks` or `downloaded_chunks`).
* `total_chunks` = The server-declared immutable value for the current session.

#### 1.2.3 DOWNLOADING Authority Model

`DOWNLOADING` progress is valid ONLY when:

1. The inbound session is chunk-streamed from the server.
2. Chunks are committed through the same ACK pipeline as the upload.
3. The server is the authority for `total_chunks`.

The overlay MUST NOT infer progress from:

* File size on disk.
* Partial inbound files in `tmp_in/`.
* I/O activity during the `DELIVERING` phase.

#### 1.2.4 UI Rendering Rule (No Blending)

The overlay MUST strictly enforce the mapping:

* **`UPLOADING`** $\rightarrow$ reads `uploaded_chunks`
* **`DOWNLOADING`** $\rightarrow$ reads `downloaded_chunks`
* **`DELIVERING`** $\rightarrow$ renders NO progress field.

### 1.3 The Status Write Guarantee (Anti-Freeze Rule)

1. **Tick-Driven Updates:** The sysmodule MUST rewrite `state/status.json` at least once per `ExecutionTick` while the FSM is in any state other than `IDLE`.
2. **Atomic Writes:** `status.json` MUST be written atomically (`write` $\rightarrow$ `rename`) to prevent partial reads by the Overlay resulting in UI blanking.
3. **Monotonic Heartbeat:** The schema MUST include a `tick` integer that increments every `ExecutionTick`. The Overlay can use this to detect if the sysmodule itself has hung.

---

## 2. Restore Section (Last Successful Restore)

This section displays a static log of the most recent completed restore operation. **No progress percentages are ever displayed here.**

### 2.1 Line Order (Strict)

1. **Game Name** (Human-readable title)
2. **Timestamp** (Date + Time, no year — e.g., "Oct 24, 14:30")
3. **Snapshot ID**

### 2.2 Section Rules

* The Game Name MUST be the first line.
* The Timestamp MUST be the second line.
* The Snapshot ID MUST be the third line.
* **No Title ID is ever shown in the UI.**
* If any field is missing from the local state, its respective line is omitted entirely. Placeholders (e.g., "Unknown Game") are forbidden.

---

## 3. Backup Section (Last Outbound State)

This section reflects a static log of the most recent outbound backup activity. **No progress percentages are ever displayed here.**

### 3.1 Line Order (Strict)

1. **Game Name** (Human-readable title)
2. **Timestamp** (Date + Time, no year)
3. **Snapshot ID**

### 3.2 Section Rules

* The Game Name MUST be the first line.
* The Timestamp MUST be the second line.
* The Snapshot ID MUST be the third line.
* Only the *most recent* outbound entry is shown.
* **No Title ID or internal identifiers are ever exposed.**

---

## 4. Triggers & Actions: Full Batch Backup

The overlay provides a single interactive action line.

### 4.1 UI Element

* **Action Line:** `Backup All (Slow)`

### 4.2 IPC Behavior (Filesystem Intent)

When the user triggers this action, the overlay performs exactly one operation:

1. Write a simple, empty batch intent file to `signals/batch_backup.request`.
2. The overlay requires no response and does not wait for execution.

### 4.3 Sysmodule Handling

On the next `ExecutionTick()`, the sysmodule MUST:

1. Detect the presence of `signals/batch_backup.request`.
2. Consume it atomically (delete it or rename it to `processing/`).
3. Iterate through all installed titles and enqueue save entries into `outbound/` as independent transactions.
4. Proceed using normal FSM outbound handling rules.

### 4.4 Invariants & Constraints

* **Idempotency:** The signal file is single-use and idempotent. Multiple button presses by the user may safely overwrite or re-create the same signal file.
* **Level-Triggered:** The sysmodule treats the file's existence as a level-triggered intent, not an edge event.
* **No Network Path:** There is absolutely no UI-to-network execution path. The overlay cannot execute network calls or force the FSM to change states directly.
* **IPC Isolation:** "Backup All (Slow)" is implemented strictly as a filesystem-level intent primitive. The sysmodule observes and consumes state transitions encoded as files.
