# OmniSave V2: Switch Client Device Sync API

**Status:** Final Specification
**Component:** Switch Sysmodule (Transport Layer)
**Purpose:** Defines the exact sequence of HTTP calls the sysmodule makes, with request/response shapes, retry semantics, and idempotency expectations.

---

## 1. Global Contract

* **Base URL:** Read from `sdmc:/switch/omnisave/config.ini` (`server_address`), appended with `/api/v1/sync`.
* **Required Header:** Every request MUST include `X-Device-ID: <device_id>` (MAC address from `state/device.json`).
* **Content-Type:** `application/json` unless uploading binary data (`application/octet-stream`).
* **Retry Policy:** All transient failures (network timeout, 5xx) use exponential backoff: {5s, 10s, 30s, 60s}. After 4 consecutive failures on any single operation, FSM transitions to `RETRY_BACKOFF`.
* **No Enrollment Endpoint:** First-boot identity seeding is not a special flow. Send `parent_sequence_num: null` on `POST /transactions/inbound` and the server creates the lineage anchor automatically.

---

## 2. Upload Flow (FSM: UPLOADING)

Triggered by `pmdmnt` game-exit event. Steps execute in strict sequence; each step is gated on the previous succeeding.

### Step 1 — Propose Upload
`POST /api/v1/sync/transactions/inbound`

```json
{
  "title_id": "0100F2C0115B6000",
  "total_size_bytes": 14500000,
  "hardware_type": "HAC-001",
  "parent_sequence_num": 42
}
```

* `parent_sequence_num`: read from `state/lineage.json` for this `(title_id, switch_user_id)`. Send `null` on first boot or if no lineage entry exists.

Response `201 Created`:
```json
{
  "transaction_id": "tx_abc123",
  "session_id": "sess_xyz789"
}
```

### Step 2 — Upload Windows
`PUT /api/v1/sync/sessions/{session_id}/window`

* `Content-Type: application/octet-stream`
* Query params: `?offset=0` (byte offset of this window in the full file)
* Body: raw binary bytes (up to 64 MB window; last window may be smaller).
* **Idempotent:** retry the same window on failure; server ignores overlapping writes (high-water mark).
* Upload windows sequentially from offset 0 to `total_size_bytes`.

Response: `202 Accepted`.

### Step 3 — Commit
`POST /api/v1/sync/sessions/{session_id}/commit`

Empty body. Server verifies received bytes == `total_size_bytes`.

* `202 Accepted` → upload accepted for async processing; FSM transitions to `IDLE`.
* `409 Conflict` → completeness gate failed; re-send missing windows, retry commit.

---

## 3. Restore Poll & Download Flow (FSM: DOWNLOADING)

The FSM polls for pending restores on every heartbeat and when returning from `RETRY_BACKOFF`.

### Step 1 — Poll Queue
`GET /api/v1/sync/queue`

Response `200 OK`:
```json
{
  "pending": [
    {
      "transaction_id": "tx_def456",
      "title_id": "0100F2C0115B6000",
      "snapshot_sequence": 6,
      "total_bytes": 14500000,
      "checkpoint_ledger": [2547384827, 1839201945, 3094827163]
    }
  ]
}
```

Empty `pending` → remain `IDLE`. Non-empty → FSM transitions to `DOWNLOADING`.

`checkpoint_ledger` is an array of xxHash32 values, one per 4 MB checkpoint boundary. The client validates each checkpoint during download.

### Step 2 — Download (Byte-Range)
`GET /api/v1/sync/transactions/{transaction_id}/range?offset=X&length=Y`

* Response: raw binary (`application/octet-stream`) for the requested byte slice.
* Write each window to `tmp_in/<key>/save.zip` before updating progress.
* On success, validate the xxHash32 of each completed 4 MB checkpoint against `checkpoint_ledger`.
* On hash mismatch: truncate file back to last validated checkpoint, retry the window.
* On `403` (transaction not found or superseded): re-poll queue on next tick.
* On transport failure: resume from last validated byte offset (file size on disk).

### Step 3 — Acknowledge (After Inject Succeeds)
After download completes and file is validated, FSM transitions to `INBOUND_READY`, then (when game is not running) to `DELIVERING` for the local atomic inject. On inject success:

`POST /api/v1/sync/ack`

```json
{
  "transaction_id": "tx_def456"
}
```

Response `200 OK` → FSM returns to `IDLE`. Update `state/lineage.json` with new `snapshot_counter` and `last_synced_at`.

### Step 4 — Report Permanent Failure (if inject fails)
If the inject fails permanently (save data corrupt, game not installed):

`POST /api/v1/sync/fail`

```json
{"transaction_id": "tx_def456", "error_code": "inject_fail"}
```

FSM returns to `IDLE`. Server marks transaction `FAILED` and logs the error.

**Transient blocks (game running, storage full) are handled locally** — the FSM defers the inject and retries without contacting the server.

---

## 4. Error & Idempotency Summary

| Situation | Action |
| --- | --- |
| Network timeout on any call | Retry with backoff |
| `409` on commit | Re-upload missing windows, retry commit |
| `403` on range download | Re-poll queue on next tick |
| 4 consecutive failures on one operation | FSM → `RETRY_BACKOFF` |
| Hash mismatch on checkpoint | Truncate to last valid checkpoint, retry window |
| Game running at inject time | Defer locally — no server call; retry on next tick after game exits |
| `507 Insufficient Storage` from server | Uploads blocked; restores still allowed; wait |
