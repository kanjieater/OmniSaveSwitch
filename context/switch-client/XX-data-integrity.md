# OmniSave Symmetric Integrity Specification

## 1. Architectural Objective

Decouple transport mechanics from state progression by enforcing a monotonic trust-accumulation pipeline. Both inbound and outbound operations share an identical, manifest-driven execution engine. Completeness and correctness are mathematically proven at the chunk level before any transaction reaches the final mutation phase (`FSM_DELIVERING`).

## 2. Core Data Structure: `TransferSession` Manifest

A strictly scoped session object acts as the single source of truth for transit integrity. It is instantiated at the start of a transfer and dictates all I/O operations.

* **`session_id` / `manifest_version`:** Cryptographic or UUID token binding the transfer to a specific, immutable server state.
* **`expected_total_size`:** The exact byte size of the final assembled artifact.
* **`chunk_specs`:** Total number of chunks and the uniform chunk size.
* **`ledger`:** An array of expected hashes (e.g., `xxHash32`) mapping to a `verified_chunks` state tracker (boolean array or bitfield) to mark successful chunk commits.

## 3. Execution Pipeline

### Phase 1: Session Initialization & Staging Isolation

A new `TransferSession` strictly invalidates all previous state. Cross-transaction contamination is structurally impossible.

* **Inbound (`FSM_POLLING`):** Fetches the manifest, wipes `tmp_in/<inbound_key>`, and initializes a clean staging environment before requesting chunk 0.
* **Outbound (`FSM_UPLOADING`):** Generates the authoritative deterministic ZIP(STORE) archive in `tmp_out/<outbound_key>/save.zip`, freezing the state.
* **Constraint:** Stale or partial data from previous failed sessions is explicitly annihilated before new data flows.

### Phase 2: Chunk-Atomic Verification (The Untrusted Edge)

Integrity is assessed immediately at the transport boundary, per chunk. Transport is treated as a stateless, untrusted pipe.

* **Inbound:** 1. Download chunk bytes.
2. Write to a provisional file (e.g., `.part`) to ensure crash safety before validation.
3. Compute local `xxHash32` of the `.part` file.
4. Compare against the manifest's expected hash.
5. **Match:** Rename to final chunk index, mark `verified_chunks[i] = true`.
6. **Mismatch:** Delete provisional file, trigger retry.
* **Outbound:** 1. Read chunk from `tmp_out`.
2. Compute local `xxHash32`.
3. Upload chunk, sending the hash in the HTTP header.
4. Server validates against received payload.
5. **Match (200 OK):** Mark `verified_chunks[i] = true`.
6. **Mismatch (400 Bad Request):** Trigger retry.

### Phase 3: The Readiness Gate (`FSM_STAGING_READY`)

The system transitions from transit to finalization *only* if the mathematical contract is fulfilled. This replaces naive size-checking.

* **Conditions for Advancement:**
1. The `verified_chunks` count exactly matches `expected_chunks`.
2. No chunk indexes are missing or unresolved in the ledger.
3. The physical byte size of the assembled file precisely matches `expected_total_size`.


* **Transitions:**
* **Inbound:** Transitions to `FSM_DELIVERING` (safe to execute destructive purge + unpack).
* **Outbound:** Sends final commit ping, transitions to `FSM_IDLE`.



## 4. Crash Recovery & Implementation Detail

To manage the `verified_chunks` state within sysmodule constraints (tight memory, no dynamic heap allocation), implement the ledger as a **static `u32` bitfield array** persisted to disk (e.g., alongside `session.json`).

**Why Disk-Backed Bitfield over Pure RAM:**

* **Restart Safety:** If the sysmodule hard-crashes on chunk 99 of 100, a RAM-only ledger forces a full re-download upon reboot.
* **Minimal Overhead:** A bitfield tracking up to 1024 chunks requires only 32 bytes of memory. Writing this 32-byte state to `tmp_in/session.bin` after each successful chunk verification provides granular resume capability with negligible I/O penalty.
* **FSM Determinism:** On cold boot, the FSM reads the bitfield. If an active session exists, it resumes exactly where it left off, hashing only the remaining unverified chunks. If the session is invalid or missing, it purges the directory.

OmniSave High-Throughput Integrity Architecture Specification
1. Architectural Objective
Transition the OmniSave transport pipeline from a chunk-based RPC model to an offset-synced deterministic file reconstruction system. This architecture maximizes throughput on the Nintendo Switch sysmodule by utilizing stateless HTTP range requests, persistent infrastructure reuse, and a monotonic, offset-based resumption model. It explicitly eliminates custom streaming protocols, bidirectional ACK loops, and chunk-addressed transport.

2. Core Paradigms
Offset-Addressed Transport (The Oracle): Transport is entirely chunkless. The network layer moves opaque bytes via standard HTTP range requests (offset and length). The server acts as a stateless byte oracle.

Precomputed Checkpoint Ledger (The Authority): Integrity validation occurs at internal logical boundaries (e.g., every 4MB). The network layer is unaware of these checkpoints. The ledger is an immutable mathematical map generated before transfer begins.

Receiver Authority: The receiver of the data dictates the contiguous high-water mark. The client tracks verified_bytes for downloads; the server tracks server_verified_bytes for uploads.

Stateless HTTP Cycles: The server is stateless per transfer request. Session state is authoritative only at the manifest level, not at the transport level.

Handle Resilience: Persistent libcurl and filesystem handles are utilized as critical performance optimizations, but they are not correctness primitives. The system safely recovers handles interrupted by Horizon OS sleep states.

3. The Server Manifest Pipeline (Offline Job)
The server must act as the absolute mathematical authority before any byte transfer occurs. It executes a single CPU-intensive pipeline asynchronously:

Freeze: A canonical save file is finalized (either uploaded from a device or pulled from ROMM).

Compute: The server slices the file into logical integrity boundaries (e.g., 4MB) and computes xxHash32 for each block.

Store: The server persists the total_bytes, checkpoint_size, and the resulting ledger array.

Ready: The transaction is marked READY_FOR_RESTORE.

4. The API Surface (Stateless & Chunkless)
The API relies entirely on byte math, completely removing chunk_index from the URI.

4.1 Manifest Authority
POST /queue/claim
Grants the active lease and returns the immutable mathematical map.

Returns: session_id, total_bytes, checkpoint_size, and the complete ledger array.

4.2 Byte Oracle (Download)
GET /transactions/{session_id}/range?offset=X&length=Y

Behavior: Server seeks to offset, reads length bytes, and returns the raw opaque binary stream. No hashes, no logic, no state mutation.

4.3 Opaque Upload
PUT /transactions/{session_id}/range?offset=X

Behavior: Client sends raw bytes. Server accepts the stream, validates internal checkpoints against its frozen manifest, and synchronously returns {"verified_bytes": Z} in the response.

5. Critical Invariants
Anti-Speculation Progression: verified_bytes MUST NEVER advance based on local disk writes or socket transmission. It advances EXCLUSIVELY upon positive mathematical validation of internal checkpoints by the receiving authority.

Symmetric Tail Truncation: All writes must be explicitly offset-addressed. Implicit append mode is forbidden. If a connection drops, the receiver MUST explicitly truncate or seek its staging file back to the highest contiguous verified_bytes marker. Unvalidated volatile tails are always discarded.

Incremental Download Validation: During a download request, the client must finalize the local rolling hash and advance verified_bytes immediately as each internal checkpoint_size boundary is crossed within the read buffer. It must not wait for the entire HTTP request to complete before validating.

Semantic Atomicity: The UI overlay tracks progress purely via mathematical validity: floor((verified_bytes / total_bytes) * 100). The target live save mount remains completely isolated and UNDEFINED until the transaction enters final delivery.

6. Execution Pipeline (State Machine)
IDLE
Description: System waiting; sleepable state.

Trigger: Input snapshot indicates new data is queued.

Action:

If outbound: Build archive in tmp_out/, generate TransferSession Manifest, submit manifest to server for Upload Freeze, transition to UPLOADING.

If inbound: Fetch server manifest via /queue/claim, open staging file handle, transition to DOWNLOADING.

UPLOADING
Description: Transmission of opaque ranged windows (e.g., 32MB) from outbound/ to the Server.

Action:

Explicitly seek file handle to the server's reported contiguous verified_bytes offset.

Transmit the next transfer window via PUT /range?offset={verified_bytes} containing only raw bytes.

The server receives the request, buffers or processes the stream sequentially, and validates checkpoints against its ledger.

The server synchronously returns its highest contiguous verified offset in the HTTP response body.

Advance local verified_bytes to match the server's returned offset.

Transition: When verified_bytes == total_bytes, transition to IDLE.

Failure: If the HTTP request fails or times out, loop to RETRY_BACKOFF.

DOWNLOADING
Description: Download of opaque ranged windows (e.g., 16MB-32MB) from the Server to tmp_in/.

Action:

Explicitly seek file handle to the local contiguous verified_bytes.

Request the next byte range via GET /range?offset={verified_bytes}&length={window_size} using a persistent libcurl keep-alive handle.

Stream opaque bytes strictly to offset-addressed disk locations. Feed the rolling xxHash32 state incrementally.

Every checkpoint_size boundary (e.g., 4MB) crossed during the stream triggers a hash finalization and ledger comparison.

If valid: Advance verified_bytes immediately and reset the hash state.

If invalid: Abort the HTTP transfer, truncate the file back to the verified_bytes boundary, and loop to RETRY_BACKOFF.

Transition: When verified_bytes == total_bytes, transition to INBOUND_READY.

Failure: If the connection drops mid-window, truncate the volatile tail back to the incremental verified_bytes marker and loop to RETRY_BACKOFF.

INBOUND_READY
Description: Staging artifact mathematically proven to mirror server state.

Action: Wait for system quiescence (target game confirmed closed).

Transition: Transition to DELIVERING.

DELIVERING
Description: Atomic swap/injection in progress.

Action:

Execute destructive root purge of the target save mount within the OS transaction boundary.

Unpack the fully validated archive from tmp_in/ into the live save mount.

Commit the OS journal.

Transition: Transition to IDLE.

RETRY_BACKOFF
Description: Transport or validation failed; waiting for backoff timer.

Action: Sleep for the duration of the backoff.

Transition: Return to UPLOADING or DOWNLOADING. Re-establish persistent handles, seek the filesystem to the contiguous verified_bytes boundary, and resume the ranged transfer.

FAILED
Description: Degraded state; suspends execution transitions.

Action: Halt progression for the specific title ID. Emit alert to UI.

Transition: Await external reset or boot sequence.