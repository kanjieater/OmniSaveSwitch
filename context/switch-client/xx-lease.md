# OBSOLETE — Lease Consistency & Boot-Time Reconciliation Contract

> **This document describes functionality that has been removed.** The lease model (DELIVERING state,
> lease_id, lease_expires_at, /queue/claim, /error) was replaced by stateless byte-range downloads
> authenticated by X-Device-ID. See `04-device-sync-api.md` for the current protocol.

---

# OmniSave V1 (Archived): Lease Consistency & Boot-Time Reconciliation Contract

## 1. Architectural Objective

Eliminate the "orphan lease" distributed state bug. This specification guarantees that an abrupt client termination (e.g., Horizon OS reboot, system panic, battery death) during an active transfer does not result in the server holding a stale lease for its full duration. The sysmodule will assert its state to the server upon boot, forcing an immediate lease rollback and state reversion.

---

## 2. Client-Side State Persistence (The Lease Companion File)

Currently, the sysmodule maintains lease state strictly in volatile memory during the transfer. To survive a reboot, the lease identity must be durably persisted to the SD card *before* any network I/O begins.

### Implementation Contract

When the sysmodule successfully executes `POST /queue/claim` (or initiates an outbound upload), it MUST immediately write a companion JSON file to the staging directory before opening the binary file handle for the actual payload.

* **File Location:** `sdmc:/switch/omnisave/tmp_in/<transaction_id>.lease.json` (or `tmp_out/` for uploads).
* **Write Atomicity:** This file must be written and `fsync`'d before the first `GET /range` or `PUT /range` request is dispatched.
* **Payload Structure:**

```json
{
  "transaction_id": "b5a6ca85-47fd-4ff8-8b0b-ba05a81fc623",
  "lease_id": "lease_abc123"
}

```

---

## 3. The `recovery_sweep` Reconciliation Protocol

The `recovery_sweep` function currently acts as a local garbage collector, wiping `tmp_in/` and `tmp_out/` on sysmodule boot. It MUST be upgraded to act as a distributed state reconciler.

### Execution Sequence

During sysmodule initialization, `recovery_sweep` MUST perform the following operations:

1. **Scan:** Enumerate the `tmp_in/` and `tmp_out/` directories for any `.lease.json` files.
2. **Evaluate:** Any lease file found during boot is, by definition, an orphan from a dead process.
3. **Notify:** For each orphaned lease, the sysmodule MUST dispatch an explicit abandonment signal to the server.
4. **Purge:** Only after the server ACKs the abandonment (or confirms the lease is already dead) should the sysmodule delete the `.lease.json` and its associated binary artifacts (`.bin` or `.zip`).

---

## 4. Server API Interaction

To release the lease, the sysmodule will utilize the existing transient error reporting endpoint. This avoids creating new API surface area while achieving the exact desired state transition (reverting to `READY_FOR_RESTORE`).

### Request

`POST /api/v1/sync/transactions/{transaction_id}/error`

```json
{
  "lease_id": "lease_abc123",
  "reason": "CLIENT_ABRUPT_REBOOT"
}

```

### Server-Side State Machine Rule

Upon receiving this payload, the server MUST execute the following within a single transaction:

1. Validate the `lease_id` matches the current active lease for `transaction_id`.
2. Clear the `lease_id` and `lease_expires_at` fields.
3. Revert the transaction `state` from `DELIVERING` (or `UPLOADING`) back to `READY_FOR_RESTORE`.
4. Emit an event to the UI-API event folding engine indicating the transient failure, allowing the UI to accurately reflect the interrupted state without waiting for the timeout.

### Client-Side Error Handling During Sweep

If the Switch boots without an active Wi-Fi connection, the `recovery_sweep` `POST` will fail.

* **Rule:** The sysmodule MUST NOT delete the `.lease.json` file if the network request fails due to lack of connectivity. It should leave the file on disk and attempt the reconciliation sweep again on the next internal polling cycle once network connectivity is established.

---

## 5. Lifecycle Completion (Happy Path Cleanup)

To prevent false-positive orphaned leases during the next boot, the sysmodule MUST clean up its own state upon successful completion of a transaction.

* **Inbound (Downloads):** Immediately after the atomic save injection is completed (`DELIVERING` phase finishes) and `POST /ack` succeeds, the sysmodule MUST delete the `<transaction_id>.lease.json` file.
* **Outbound (Uploads):** Immediately after `POST /sessions/{session_id}/commit` succeeds, the sysmodule MUST delete the `<transaction_id>.lease.json` file.

6. The only real remaining risk

There is one edge case still:

⚠ “boot crash during recovery_sweep after lease file detection”

If:

sweep starts
detects lease file
sends abort
crashes before purge

You can get duplicate retries.

Fix (minimal):

Add a tiny state marker:

lease.json → lease.processing.lock

Flow:

detect lease
rename to .processing
send abort
only then delete

This makes recovery idempotent.