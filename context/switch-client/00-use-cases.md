**Status:** System Specification
**Component:** Switch Client (Sysmodule + Overlay)
**Purpose:** Defines the deterministic runtime boundaries between the Sysmodule (execution), Overlay (observability), and Server (authority).

---

## 1. Architectural Roles

* **Sysmodule (Headless Runtime):** The sole execution agent. Owns filesystem state, save data extraction, transport scheduling, and error generation. It is the only entity authorized to write to the SD card.
* **Overlay (Observer):** A read-only UI mirror. It mirrors filesystem state and triggers out-of-band commands via IPC files. It contains no business logic or sync awareness.
* **Server (Authority):** The source of truth. Handles conflict resolution, HEAD assignment, and restore decisions.

---

## 2. File-Based IPC & Atomic Staging Contract

### 2.1 Truth Ownership Map

| File/Directory | Purpose | Producer | Consumer |
| --- | --- | --- | --- |
| `current.json` | Active game context | Sysmodule | Overlay |
| `tmp_out/` | Atomic staging for save extraction | Sysmodule | Sysmodule |
| `outbound/` | Ready-to-upload queue | Sysmodule | Sysmodule |
| `tmp_in/` | Atomic staging for save injection | Sysmodule | Sysmodule |
| `inbound/` | Server-directed restore manifests | Sysmodule | Sysmodule |
| `errors/` | Unresolved local failures | Sysmodule | Overlay/Sysmodule |
| `signals/batch_backup.request` | Bulk backup trigger | Overlay | Sysmodule |

### 2.2 Transfer Consistency Contract

* **`tmp_` vs `outbound/` Partitioning:**
* **`tmp_` (Staging):** Strictly a non-authoritative workspace for atomic file assembly/extraction. **No progress or chunk state shall ever be stored here.**
* **`outbound/` (Durable Queue):** The source of truth for the upload lifecycle. Chunk progress, metadata, and resumption state must be stored in the `outbound/` manifest or server-side resume state.


* **Atomicity Guarantee:** Files in `tmp_` are considered disposable scratch space. They may be purged on startup.
* **Resumption Authority:** The Sysmodule treats `outbound/` as the durable state. If a crash occurs during chunked upload, the Sysmodule queries server-side upload status and resumes the transmission based on `outbound/` manifest metadata, not `tmp_` content.

---

## 3. Sysmodule Execution Layer

### 3.1 Transport & Recovery Logic

* **Poll Scheduler:** Manages HTTP transport scheduling.
* **Recovery Trigger:** An event-driven mechanism (PSC wake) that triggers a `recovery_sweep()`. This sweep validates the `outbound/` queue status against server-side progress.
* **Consistency Rule:** Polling endpoints MUST expose only committed transactional state. Partially processed or intermediate transitions MUST NEVER be externally visible.

### 3.2 Automated Lifecycle

* **Backup:** Triggered by `pmdmnt` game-exit. Save extraction → `tmp_out/` → Atomic rename to `outbound/` → Initiate chunked upload.
* **Restore:** Sysmodule periodically polls `inbound/`. If a manifest exists: Verify game not running → Atomic swap → Finalize → `POST /ack`.
* **Error Lifecycle:** Logged to `errors/`. Persist until explicitly cleared by server sync or manual purge.

---

## 4. Overlay Observability Layer

### 4.1 Consistency Model

* **Eventual Consistency:** The overlay is a point-in-time snapshot of the filesystem. It provides no live invalidation guarantees.
* **Refresh Semantics:** State is read on open. If the system state changes, the user must close and re-open the UI to trigger a fresh filesystem re-scan.

### 4.2 Constraints

* **Read-Only:** All overlay operations are passive.
* **Action Surface:** The only permitted write operation is writing `signals/batch_backup.request`.
* **Decoupling:** The overlay remains oblivious to server internals, HTTP protocols, and auth. It consumes local filesystem state only.

---

## 5. System-Wide Invariants & Hard Boundaries

### 5.1 Storage Protection

* **Hard Guardrail:** If SD card utilization > 95%, the Sysmodule shall transition to Read-Only mode. It MUST reject all new backup staging requests.

### 5.2 Error Handling

* **Non-Blocking:** Errors are logs, not terminal execution states.
* **Persistence:** Errors exist in `errors/` until the Sysmodule purges them upon server acknowledgment.

### 5.3 Non-Goals (Explicit Exclusions)

The following are strictly out-of-scope for the Switch client:

* **Conflict Resolution:** The client is "conflict blind." It cannot make decisions on divergent timelines.
* **Interactive Restore Selection:** The client only accepts server-pushed manifests.
* **Bidirectional Logic:** No UI-driven restore scheduling.
* **Server-Awareness:** The client only understands the request/response transport protocol, not the server's internals.

---

## 6. Deterministic Guarantee

The system is **statically partitioned**.

* The **Sysmodule** performs the work via atomic staging (`tmp_` to `outbound/inbound`).
* The **Overlay** observes the work (Snapshot).
* The **Server** judges the work (Conflict resolution).

No component is permitted to perform the role of another. Every component relies solely on the File-Based IPC contract, the Server-provided manifests, and the persistent `outbound/` queue state.

