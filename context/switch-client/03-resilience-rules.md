# OmniSave V1: Switch Client Resilience Rules

**Status:** Final Specification
**Component:** Switch Sysmodule (Execution Layer)
**Purpose:** Defines the deterministic client-side execution boundaries, state reconstruction rules, and error handling invariants for the chunked transport protocol.

---

## 1. Global Architectural Invariants

* **Server as Sole Authority:** The Server response is the *only* state transition trigger. Local filesystem state is merely **queued intent** (outbound/inbound) and **cached context** (lineage).
* **The Chunked Transport Protocol:** Every synchronization action executes through the server's multi-step chunked pipeline. Upload: `POST /transactions/inbound` → `PUT /sessions/{id}/chunks/{n}` → `POST /sessions/{id}/commit`. Restore: `GET /queue` → `POST /queue/claim` → `GET /transactions/{id}/chunks/{n}` → `POST /ack`. First-boot identity seeding uses a normal `POST /transactions/inbound` with `parent_sequence_num: null` — there is no separate registration or bootstrap endpoint.
* **Single-Step Progression:** Each `ExecutionTick()` may produce **at most one** externally visible effect (Network Call OR Filesystem Mutation OR State Transition).
* **Outbound Precedence:** `outbound/` artifact processing always supersedes `inbound/` processing. The system must ensure local intent is committed before applying remote changes.

---

## 2. Recovery Hierarchy

Every `ExecutionTick()` evaluates system validity in the following order. If a violation is found, the system halts progression for the current tick:

1. **Integrity Validation:** Validate `state/lineage/` cache and `outbound/inbound/` manifests. If any artifact is corrupted/malformed, discard it.
2. **Reconciliation:** Evaluate the presence of `signals/batch_backup.request` (Dirty Flag) or `inbound/` manifests.
3. **Transport Execution:** Execute the appropriate chunked pipeline (`UPLOADING` or `DOWNLOADING`) depending on FSM state and server queue contents.
4. **State Application:** Apply authoritative updates (lineage cache update, inbound inject) derived *only* from the server response.

---

## 3. Failure & Recovery Modes

### 3.1 Metadata & Payload Domain

* **Metadata (`lineage.json`):** Ephemeral performance cache. If corrupted, unlink immediately. The system relies on the next server poll cycle to reconstruct the lineage anchor; there is no local repair logic.
* **Payloads (`inbound/` / `outbound/`):** If an artifact or manifest is corrupted, it is unlinked/discarded.
* **Interrupted Injection:** If an atomic swap (injection) is interrupted (e.g., process kill), the system makes no attempt to recover the partial local state. The state is discarded, and the system relies on the next poll cycle to re-fetch and inject the canonical HEAD.

### 3.2 Transient Execution Errors

* **No FAILED State:** The system maintains no persistent failure state.
* **Ephemeral Logging:** Network/Filesystem errors are logged to `errors/` for Overlay observability *only*. They have zero impact on the FSM decision loop.
* **Self-Healing:** If an error occurs, the FSM transitions to `IDLE` or `RETRY_BACKOFF`. The next `ExecutionTick` re-evaluates the environment; if the blocker (e.g., disk full, network down) is removed, the system naturally resumes.

---

## 4. Operational Invariants

### 4.1 Storage & Capacity

* **Read-Only Mode:** If storage utilization exceeds **95%**, the Sysmodule transitions to `READ_ONLY`. All `inbound/` processing and `outbound/` staging are blocked.
* **Event-Driven Resolution:** Capacity is re-evaluated on every `ExecutionTick`. Once usage drops below threshold, the system automatically exits `READ_ONLY`.

### 4.2 Wake & Boot Scheduling

* **Non-Blocking Wake:** Upon OS wake, the Sysmodule performs a `recovery_sweep()` and *schedules* an `ExecutionTick`. It does *not* force network activity during the wake context.
* **Event-Driven Triggers:** All execution is driven by `pmdmnt` (Dirty Flag), `inbound/` artifact presence, or the scheduled 5-minute heartbeat. There are no "special" code paths for Boot or Wake.

---

## 5. Summary Table: Deterministic Outcomes

| Scenario | Recovery Strategy |
| --- | --- |
| **Lineage Cache Corruption** | Discard → Next `POST /transactions/inbound` response provides lineage anchor. |
| **Outbound Upload Failure** | Terminate Tick → Retry on next `ExecutionTick` (Backoff-scheduled). |
| **Interrupted Download** | Keep `tmp_in/<key>/` partial file → Resume byte-range download from last validated checkpoint offset on next tick. |
| **Interrupted Restore** | Re-poll `GET /queue` on next cycle; re-download and inject. |
| **Network Failure** | `RETRY_BACKOFF` → Retry on next `ExecutionTick`. |
| **Storage Full** | Transition to `READ_ONLY` → Auto-resume on next `ExecutionTick`. |
| **Game Running** | Defer `inbound/` injection → Re-evaluate on next `ExecutionTick` (after game exit). |

---

## 6. Implementation Guarantee

The client is a **filesystem-interpreter**. It does not maintain a memory-resident state of "what it thinks it should be doing." It interprets the filesystem queue, executes the appropriate transport pipeline against the server, and applies the result. **The Server response is the only transition trigger.** There is no "recovery mode," no "backup files," and no "failure tracking." Every `ExecutionTick` is a fresh interpretation of intent versus reality.
