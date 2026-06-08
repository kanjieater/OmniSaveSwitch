Got it. Using the MAC address as the root of trust is a very common embedded pattern—it guarantees that if a user formats their SD card or moves it to a new unit, the hardware identity remains physically anchored. We will lock that back in.

Here is the final, production-ready data model specification integrating the MAC address identity, the absolute outbound lock, the consistent filesystem view, and the strict schema definitions.

---

# 02-data-model.md — OmniSave V1: Switch Client Data Model

**Status:** System Specification

**Component:** Switch Sysmodule (Execution Layer)

**Purpose:** Defines the strict filesystem contract, persistent schemas, identity bindings, and local storage structures required for deterministic execution.

---

## 1. Data Ownership Boundaries & Identity Binding

The filesystem acts as the absolute persistence layer and IPC boundary. There is no internal database.

### 1.1 Data Ownership Boundaries

* **Sysmodule Authority:** The Sysmodule is the **exclusive writer** for all directories except explicit Overlay IPC command triggers.
* **Overlay Authority:** The Overlay is **strictly read-only** for all state files.
* **Server Authority:** The Server exclusively authors `snapshot_id`, `snapshot_counter`, and conflict metadata. The client stores these immutably and never generates identifiers locally.

### 1.2 The Identity Binding Contract

To prevent cross-account corruption and cleanly separate authentication from execution, the sysmodule operates under a strict identity namespace.

1. **The 3-Key Execution Scope:** All execution state, manifests, and lineage records MUST be scoped strictly by the composite key: `(device_id, switch_user_id, title_id)`.
2. **Auth vs. Execution Separation:** The `omni_user_id` (server account) is an **Auth Domain** concern, strictly isolated to `state/auth.json`. It is validated prior to network calls but is NEVER used to construct filesystem paths or snapshot identities.
3. **Identity Precedence (The "Poison Pill"):**
* **`omni_user_id` Mismatch:** FSM triggers a **HARD FAIL** (Auth invalid).
* **`device_id` Mismatch:** FSM triggers a **HARD FAIL** (Hardware rejected by server).
* **`switch_user_id` Mismatch:** Handled as completely isolated namespaces. Cross-profile merging is structurally forbidden.



### 1.3 Snapshot Reference Tracking (Primary Key)

All Snapshot IDs and Manifest filenames MUST adhere to this strict Server-defined schema to ensure lexical sorting and collision resistance:

**Format:** `YYYYMMDD_HHMMSS-TTTTTTTTTTTTTTTT-UUUUUUUUUUUUUUUU`

* `YYYYMMDD_HHMMSS`: UTC timestamp of snapshot creation.
* `TTTTTTTTTTTTTTTT`: 16-character Switch Title ID.
* `UUUUUUUUUUUUUUUU`: 16-character Switch User ID (Profile ID).

---

## 2. Directory Contracts & Filesystem Scanning

All data resides in `sdmc:/switch/omnisave/`. The presence of a file in a specific directory inherently defines its execution intent.

### 2.1 The Consistent View Contract

At the start of the Execution Tick, the FSM constructs an `InputSnapshot` in memory based on the current filesystem state. The FSM operates strictly on this in-memory snapshot. Any asynchronous OS interruptions or file mutations that hit the disk *during* the tick's execution phase are structurally invisible to the current evaluation and are processed in the subsequent tick.

### 2.2 Directory Semantics

| Path | Local Staging Directory Contract |
| --- | --- |
| `state/` | Contains core JSON schemas. Written exclusively via temporary file + atomic `rename()`. |
| `outbound/` | Transactional queue. Filename MUST be `<target_snapshot_id>.json`. If a file exists here, it is fully formed and guaranteed ready for transport. |
| `inbound/` | Completed download artifacts. Directory name MUST be `<snap_key>` (SNAP_KEY_LEN). `<snap_key>.json` metadata sidecar holds `transaction_id`. Validated on read before inject. |
| `errors/` | Persistent log of terminal FSM aborts (e.g., out of space, 409 conflicts). |

### 2.3 Temporary File Semantics & Wake Isolation

* **Staging Isolation:** Files in `tmp_` directories (`tmp_in/`, `tmp_out/`) do not exist as far as the Execution FSM is concerned. They are volatile assembly buffers.
* **Wake Envelope Isolation:** When the OS wakes, network materialization (fetching manifests) writes strictly to `tmp_in/`. It NEVER writes directly to `inbound/`.
* **Atomic Promotion:** The subsequent FSM Tick evaluates the files in `tmp_in/`. If they pass conflict arbitration, the FSM performs an atomic `rename()` from `tmp_in/` to `inbound/`.
* **Corruption Behavior:** Any partial read or JSON parse failure forces the FSM to treat the artifact as corrupted, immediately unlinking the file.

### 2.4 Active State Arbitration (Absolute Outbound Lock)

The sysmodule enforces strict linear precedence. Local un-synced progress always survives.

* **The Rule:** If an `outbound/` manifest exists for a title, OR if that title's game process is currently running, the FSM completely ignores and discards any incoming Server manifests for that title.
* **Linear Precedence:** `inbound/` manifests may ONLY be processed and injected if the `outbound/` queue for that title is entirely empty.
* **Crash Arbitration:** If a crash leaves files in *both* `outbound/` and `inbound/` upon boot, the `recovery_sweep()` unconditionally deletes the `inbound/` manifest.

---

## 3. Persistent Client State Requirements

The system guarantees deterministic recovery from hard crashes, power loss, and sleep cycles based on these persistence tiers.

* **Durable (Reboot Survival):** `state/device.json`, `state/auth.json`, `state/lineage.json`, and fully formed payloads/manifests in `outbound/` and `inbound/` are trusted as canonical truth.
* **Volatile (Discarded on Recovery):** All contents of `tmp_out/` and `tmp_in/`, and any manifest in `inbound/` where `expires_at` is in the past, are unconditionally purged during the system `recovery_sweep()`.
* **RAM State:** Entirely lost on reboot. Execution rehydrates strictly from the surviving directory contracts.

---

## 4. Core Schemas (Persistent & Staging)

### 4.1 Device Identity Schema (`state/device.json`)

The immutable hardware identity anchored to the physical network interface.

```json
{
  "device_id": "00:11:22:33:44:55",
  "hw_type": "HAC-001",
  "fw_version": "16.0.3",
  "client_version": "1.0.0"
}

```

* **Lifecycle:** Evaluated on boot. The `device_id` MUST be deterministically derived from the console's physical MAC address. If `state/device.json` is missing (e.g., following an SD card format), the client seamlessly reconstructs the exact same identity by reading the hardware MAC address.

### 4.2 Sync Preference Cache Structures (`state/prefs.json`)

Governs FSM evaluation boundaries. If missing/corrupted, the FSM defaults to the most restrictive safe state (`false` for all automated operations).

```json
{
  "global_auto_upload": true,
  "global_auto_download": true,
  "pause_sync_on_battery_below": 20,
  "title_overrides": {
    "0100F2C0115B6000": { "auto_upload": false, "auto_download": false }
  }
}

```

### 4.3 Base Snapshot Lineage Tracking (`state/lineage.json`)

The client's read-only cache for local save history.

```json
{
  "titles": {
    "0100F2C0115B6000": {
      "profiles": {
        "0000000000000001": { 
          "switch_username": "KanjiEater",
          "base_snapshot_id": "20260610_143000-0100F2C0115B6000-0000000000000001",
          "snapshot_counter": 42,
          "last_synced_at": "2026-06-10T14:30:00Z"
        }
      }
    }
  }
}

```

* **Cache Contract:** This file is a CACHE, not an arbitrator. It is used *only* to populate the `base_snapshot_id` of new `outbound` payloads. It does not dictate correctness or resolve conflicts.
* **Lifecycle:** Updated ONLY upon successful completion of a server transaction (`POST /_done` or `POST /ack`).

### 4.4 Manifest Schema & Inbound/Outbound Metadata (`<snap_id>.json`)

The canonical definition of an in-flight save data payload, residing in `outbound/` or `inbound/`.

```json
{
  "transaction_id": "tx_def456",
  "title_id": "0100F2C0115B6000",
  "direction": "inbound",
  "total_bytes": 14500000,
  "checkpoint_ledger": [2547384827, 1839201945, 3094827163]
}
```

Written to `inbound/<key>.json` after download completes. The `checkpoint_ledger` is validated during download against xxHash32 of each 4 MB block.

* **Validation:** `transaction_id`, `title_id`, `direction`, `total_bytes`, and `checkpoint_ledger` are required.
* There is no lease object — downloads are read-only range requests authenticated by device identity (`X-Device-ID`).

---

## 5. Runtime Observability Schemas

These artifacts provide external visibility for the Overlay. They do not dictate FSM execution.

### 5.1 Local Sync Status Structures (`state/status.json`)

Read-only view for the Overlay UI, written by the FSM strictly at the end of the Transition Commit phase.

```json
{
  "fsm_state": "UPLOADING",
  "current_activity": {
    "title_id": "0100F2C0115B6000",
    "switch_user_id": "0000000000000001",
    "switch_username": "KanjiEater",
    "target_snapshot_id": "20260610_143000-0100F2C0115B6000-0000000000000001",
    "target_counter": 43,
    "progress_pct": 50 
  }
}

```

* **Consistency:** `progress_pct` is derived strictly from the Server's `upload_status` response. If the server is unreachable, this value defaults to `-1` (unknown). It is never interpolated locally.

### 5.2 Notification Payload Structures (`state/notifications.json`)

Append-only event stream for user alerts, retained until cleared by explicit IPC command from the Overlay.

```json
{
  "events": [
    {
      "id": "evt-001",
      "level": "warning",
      "message": "Storage >95%",
      "code": "ERR_FS_FULL"
    }
  ]
}

```

* **Ordering:** The sysmodule guarantees ordering inherently via the filesystem append operation. The Overlay UI processes the array sequentially.

3. Override: 02-data-model.md
Remove: enrollment_status enum.

Add: Lineage Cache Schema. The file state/lineage/<title_id>.json is now explicitly defined as a Client Cache of Server Identity.

JSON


{
  "lineage_id": "string",
  "last_known_head": "string"
}
Invariant: If this file is missing or corrupted, the client MUST treat the identity as unknown and trigger the baseline upload flow (POST /sync/baseline). The client MUST NOT attempt to repair or "fix" this file; it simply re-fetches the identity.

5.3 Inbound Chunk Transfer Contract (DOWNLOADING)

This section mirrors the UPLOADING chunk model and ensures symmetric, resumable transport for server → client transfers.

5.3.1 Chunked Inbound Delivery Model

All large save payloads delivered via inbound/ MUST be transmitted as a chunked, resumable stream.

The server does NOT send a complete file atomically.

Instead it streams:

chunk_index → chunk_data → ACK → next chunk
5.3.2 Required Server Behavior

For every inbound transfer session:

The server MUST partition payloads into deterministic chunks
The server MUST maintain:
total_chunks
last_acked_contiguous_chunk
The server MUST allow resume from any contiguous offset
5.3.3 Client State Tracking (DOWNLOADING)

The sysmodule MUST maintain:

downloaded_chunks = last contiguous chunk successfully committed to disk
total_chunks = server-declared total (immutable per session)

Update rule:

downloaded_chunks increments ONLY when:
chunk is received
written successfully to tmp_in/
validated
atomically committed into the inbound assembly buffer

No optimistic increments are allowed.

5.3.4 Progress Reporting Contract

During DOWNLOADING, state/status.json MUST expose:

{
  "fsm_state": "DOWNLOADING",
  "current_activity": {
    "title_id": "0100F2C0115B6000",
    "target_snapshot_id": "xxx",
    "downloaded_chunks": 42,
    "total_chunks": 480,
    "progress_pct": 8
  }
}
Rule:
progress_pct = floor(downloaded_chunks / total_chunks * 100)
5.3.5 Symmetry Invariant (Critical)

UPLOADING and DOWNLOADING are structurally identical:

Direction	Counter Field
UPLOAD	uploaded_chunks
DOWNLOAD	downloaded_chunks

Both MUST obey:

contiguous ACK tracking only
no speculative increments
no byte-based progress
no local inference


Full Example state/status.json (Unified Model)

This is what I would lock as the canonical overlay contract:

{
  "tick": 18392,

  "fsm_state": "DOWNLOADING",

  "current_activity": {
    "title_id": "0100F2C0115B6000",
    "switch_user_id": "0000000000000001",
    "switch_username": "KanjiEater",

    "target_snapshot_id": "20260610_143000-0100F2C0115B6000-0000000000000001",

    "direction": "inbound"
  },

  "transport": {
    "uploaded_chunks": null,
    "downloaded_chunks": 42,
    "total_chunks": 480
  },

  "ui": {
    "progress_pct": 8,
    "show_progress": true,
    "render_mode": "NETWORK"
  }
}