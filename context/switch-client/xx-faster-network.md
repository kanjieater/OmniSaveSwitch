This is your implementation guide now
# OmniSave High-Throughput Integrity Architecture Specification

## 1. Architectural Objective

Transition the OmniSave transport pipeline to a "Chunkless transport, checkpointed validation ledger" model. This maximizes throughput by minimizing setup overhead via standard HTTP range requests, while mathematically guaranteeing data integrity through a deterministic, offset-based resumption model. It maps 1:1 to `libcurl` and Horizon FS capabilities by eliminating all assumptions of real-time streaming network ACKs, relying entirely on synchronous, post-receipt validation windows.

## 2. Core Paradigms

* **Chunkless Transport (Large Ranged Requests):** The network transport unit is a substantial byte range (e.g., 16MB to 32MB) over an HTTP keep-alive connection. Requests contain opaque raw bytes only.
* **Checkpointed Validation Ledger:** Integrity validation occurs at smaller, logical internal boundaries (e.g., 4MB). The network layer is completely unaware of these checkpoints.
* **Receiver Authority:** The receiver of the data dictates the contiguous high-water mark. The client dictates `verified_bytes` for downloads; the server dictates `server_verified_bytes` for uploads.
* **Stateless HTTP Cycles:** Validation occurs deterministically on received window boundaries. The client does not depend on intermediate network results or live duplex ACKs during an active request.
* **Handle Resilience:** Persistent `libcurl` and filesystem handles are utilized purely as performance optimizations. The system gracefully re-opens, seeks, and recovers handles if interrupted by Horizon OS sleep states.

## 3. Core Data Structure: TransferSession Manifest

Instantiated at the start of any transfer, serving as the single source of truth. Structured as a compact binary payload.

* **`session_id` / `manifest_version`:** Cryptographic binding tying the transfer to a specific server state.
* **`total_bytes`:** The exact total byte size of the expected artifact.
* **`verified_bytes`:** The current contiguous verified offset (starting at 0).
* **`checkpoint_size`:** The byte size of the internal mathematical validation boundaries (e.g., 4MB).
* **`window_size`:** The byte size of the HTTP transport requests (e.g., 32MB).
* **`checkpoint_ledger`:** Ordered array of expected `xxHash32` checksums mapped strictly to the `checkpoint_size` boundaries.

## 4. Critical Invariants

* **Upload Manifest Freeze:** Before outbound byte transfer begins, the client MUST fully build the outbound archive, compute the `checkpoint_ledger` locally, and submit the complete `TransferSession Manifest` to the server. The server locks `total_bytes`, `checkpoint_size`, and the `checkpoint_ledger`. Opaque ranged uploads may only begin after this freeze.
* **Anti-Speculation Progression:** `verified_bytes` MUST NEVER advance based on local disk writes or socket transmission. It advances EXCLUSIVELY upon positive mathematical validation of internal checkpoints by the receiving authority.
* **Symmetric Tail Truncation:** If a connection drops, sleeps, or fails validation, the receiver MUST discard the volatile tail.
* *Uploads:* The server truncates its staging file back to the highest contiguous verified checkpoint.
* *Downloads:* The client truncates `tmp_in/save.zip` back to the highest contiguous verified checkpoint.


* **Linear Write Staging:** Staging artifacts (`tmp_in/`, `tmp_out/`) grow linearly. Sparse file pre-allocation is strictly prohibited.
* **Semantic Atomicity:** The UI overlay tracks progress purely via mathematical validity: `floor((verified_bytes / total_bytes) * 100)`. The target live save mount remains completely isolated and UNDEFINED during transport.

## 5. Execution Pipeline

### IDLE

* **Description:** System waiting; sleepable state.
* **Trigger:** Input snapshot indicates new data is queued.
* **Action:**
* If outbound: Build archive in `tmp_out/`, generate `TransferSession Manifest`, submit manifest to server for Upload Freeze, transition to UPLOADING.
* If inbound: Fetch server manifest, open staging file handle, transition to DOWNLOADING.



### UPLOADING

* **Description:** Transmission of opaque ranged windows (e.g., 32MB) from `outbound/` to the Server.
* **Action:**
* Seek file handle to the server's reported contiguous `verified_bytes` offset.
* Transmit the next `window_size` via a pure HTTP `POST`/`PUT` containing only raw bytes.
* The server receives the HTTP request body.
* Upon receipt (or via internal buffering), the server evaluates the received payload against its 4MB checkpoint ledger.
* The server synchronously returns its highest contiguous `server_verified_bytes` in the HTTP response.
* Advance local `verified_bytes` to match the server's reported contiguous offset.


* **Transition:** When `verified_bytes == total_bytes`, transition to IDLE.
* **Failure:** If the HTTP request fails or times out, loop to RETRY_BACKOFF.

### DOWNLOADING

* **Description:** Download of large opaque ranged windows (e.g., 32MB) from the Server to `tmp_in/`.
* **Action:**
* Seek file handle to the local contiguous `verified_bytes`.
* Request the next `window_size` byte range.
* Stream opaque bytes linearly to disk using `libcurl` write callbacks. Feed the rolling `xxHash32` state incrementally.
* Every local `checkpoint_size` (e.g., 4MB) boundary *during* the stream (as buffers fill), finalize the local hash and compare it against the `checkpoint_ledger`.
* If valid: Advance `verified_bytes` immediately.
* If invalid: Abort the HTTP transfer, truncate the file back to the last `verified_bytes` boundary, and loop to RETRY_BACKOFF.


* **Transition:** When `verified_bytes == total_bytes`, transition to INBOUND_READY.
* **Failure:** If the connection drops mid-window, truncate to the incremental `verified_bytes` marker and loop to RETRY_BACKOFF.

### INBOUND_READY

* **Description:** Staging artifact mathematically proven to mirror server state.
* **Action:** Wait for system quiescence (target game confirmed closed).
* **Transition:** Transition to DELIVERING.

### DELIVERING

* **Description:** Atomic swap/injection in progress.
* **Action:**
* Execute destructive root purge of the target save mount within the OS transaction boundary.
* Unpack the validated archive from `tmp_in/` into the live save mount.
* Commit the OS journal.


* **Transition:** Transition to IDLE.

### RETRY_BACKOFF

* **Description:** Transport or validation failed; waiting for backoff timer.
* **Action:** Sleep for the duration of the backoff.
* **Transition:** Return to UPLOADING or DOWNLOADING. Explicitly re-establish/seek the file handle and socket strictly to the contiguous `verified_bytes` boundary and resume.

### FAILED

* **Description:** Degraded state; suspends execution transitions.
* **Action:** Halt progression for the specific title ID. Emit alert to UI.
* **Transition:** Await external reset or boot sequence.
This version is very close to “implementation-correct” for a Switch sysmodule, and importantly: it’s no longer mixing incompatible models. You’ve successfully collapsed the architecture into something that maps cleanly onto:



libcurl easy handle + keep-alive

HTTP range/PUT semantics

linear FS writes

monotonic resume offset FSM

That’s the right class of system.

But there are 3 subtle correctness issues left, and they matter a lot more than the high-level design now.

1. The biggest remaining conceptual bug: “checkpoint during stream”

This line is still slightly misleading:



“Every local checkpoint_size boundary during the stream (as buffers fill), finalize the local hash…”

Why this is wrong (in practice)

With libcurl:



your write callback receives arbitrary buffer splits

you do NOT control 4MB alignment

you might get:

512KB

1.7MB

3MB + 1MB split

So “boundary during stream” is not a real event.

✔ Correct model

You need a byte accumulator layer, not “stream boundary detection”.



Replace mental model with:

running_bytes_in_window

rolling_hash_state

when running_bytes_in_window >= checkpoint_size:

finalize checkpoint

reset accumulator

carry remainder forward

In other words:

Checkpoints are logical counters over a byte stream, not network events.

2. Upload side ambiguity: server validation timing

You currently say:



“server evaluates received payload against its 4MB checkpoint ledger”

and



“returns server_verified_bytes in response”

This is correct ONLY if interpreted as:

✔ server validates after full request body is received

NOT:

❌ streaming validation during upload

Correct mental contract

Each HTTP request is:



POST window (32MB)

→ server buffers entire request

→ server validates checkpoint ledger

→ server computes new contiguous high-water mark

→ response returns verified_bytes

So:



checkpoints are internal server logic

response is atomic per request

no incremental network semantics exist

3. One subtle but important FS risk: truncation rule

You wrote:



“truncate file back to last verified_bytes boundary”

This is correct, BUT only safe if:



You guarantee one of these:

✔ SAFE CASE (your intended design)

writes are strictly append-only OR

writes are strictly offset-seek + overwrite

⚠️ DANGER CASE

If libcurl writes are buffered and crash mid-write:



you may have partial garbage beyond verified_bytes

truncation must ALWAYS be explicit

Required invariant (missing but critical)

Add this:



“All writes must be explicitly offset-addressed and never rely on implicit append mode.”

In practice:



CURLOPT_WRITEFUNCTION must always respect offset

file must always be fseek(fd, verified_bytes)

You already imply this, but it should be explicit.

4. What you got right (this is the important part)

This architecture is now structurally sound:



✔ Correct decisions

1. Chunkless transport (correct abstraction)

You eliminated:



RPC chunk coupling

duplex ACK loops

streaming protocol dependency

Good.

2. HTTP request = unit of work

This is exactly right:



window → validate → advance offset

This is the “real internet model”.

3. Server-authoritative high-water mark

This is critical and correct:



client never decides truth

This is what makes resume deterministic.

4. Checkpoints decoupled from transport

Also correct:



checkpoint_size is purely integrity logic, not network structure

5. Is this sysmodule-feasible now?

Yes — and importantly:



This is now in the “boringly implementable” zone

You are no longer designing a protocol.

You are implementing:



✔ A resumable HTTP ranged transfer engine

Equivalent to:



aria2 internals (simplified)

Steam download model (conceptually)

rsync-like checkpoint ledger (but server-side)

6. Performance reality check (important for your earlier concern)

This answers your original “why is it slow?” concern:



Your final model achieves:

1 TLS session reuse (keep-alive)

16–32MB payload amortization

minimal libcurl setup cost

no per-chunk handshake

no per-chunk file open/close

Bottleneck is now ONLY:

SD card write speed

CPU hashing cost (xxHash32 is fine)

WiFi throughput

NOT protocol overhead anymore.

7. Final verdict

✔ Architecture status: correct

✔ Sysmodule feasibility: high

✔ Resumability: strong (deterministic high-water mark)

✔ Performance model: near-optimal for Horizon OS constraints

8. If you want the next step

The next real upgrade is not architectural — it’s implementation tightening:

I would suggest we formalize:



“Execution-grade spec layer”

exact libcurl options

exact FS guarantees

exact retry state machine transitions

exact failure taxonomy (timeout vs NACK vs sleep interrupt)

That’s where most sysmodule bugs actually live.

If you want, I can turn this into a 1:1 implementation contract spec (C-level mapping for transport_upload.cpp / transport_download.cpp) so there’s zero ambiguity left.


Yes, that is slow if this was a local LAN transfer. With the new offset-synced HTTP architecture, a local transfer of 487MB should realistically take **15 to 30 seconds** (15–30 MB/s), bottlenecked primarily by the Switch's SD card read speed and Wi-Fi chip limits.

However, if you are uploading to a **remote server over the internet**, 1.75 MB/s (which is exactly **14 Mbps**) is a very common hardware-capped upload limit for standard asymmetrical home broadband connections in the US (like Xfinity/Spectrum).

If this transfer was executed on a local LAN, the architecture is not performing as designed. Here are the three most likely culprits where the implementation may have deviated from the spec:

### 1. The Request Window is Too Small

The speed advantage of this architecture relies on "Large Ranged Requests" (16MB–32MB).

* If the sysmodule was configured to push 1MB or 4MB windows, you have reintroduced massive HTTP/TLS setup churn.
* Check the client logs to verify exactly how many HTTP `PUT` requests were made. For 487MB at 32MB windows, there should only be **16 requests**.

### 2. Libcurl Keep-Alive is Failing

If your `CURL*` easy handle is being destroyed and recreated, or if the server is closing the connection after every window, you are paying the TCP handshake and TLS negotiation tax on every single request.

* **Diagnosis:** Check the server access logs or libcurl verbose output. Ensure standard `Connection: keep-alive` is being respected across the 16 requests.

### 3. Server-Side Blocking (I/O or Hashing)

The server is supposed to validate the internal 4MB checkpoints as the 32MB window arrives (or immediately after).

* If the server is synchronously writing 32MB to a slow spinning disk, closing the file, reopening it to hash it, and *then* responding to the HTTP request, the Switch spends most of its time sitting idle waiting for the HTTP response.
* **Diagnosis:** Cross-reference the database transaction timestamps with the raw HTTP logs. If there are multi-second gaps *between* the server receiving the payload and emitting the `200 OK`, the server's staging logic is blocking.



You’re right about the expectation — on a healthy LAN, this design should not be anywhere near 1.75 MB/s. That number is far below what your architecture should converge to.

So the important conclusion is:

This is not a “theoretical bandwidth limit” problem. It’s a pipeline inefficiency or serialization bottleneck somewhere in the implementation.

Let’s pin this down properly.

1. What your architecture should actually achieve

With:

single persistent TCP/TLS connection (keep-alive)
16–32MB ranged windows
streaming writes
no per-file overhead
no compression
Expected real performance on LAN:
Component	Realistic ceiling
Switch Wi-Fi	20–60 MB/s burst, 10–30 sustained
SD write	10–40 MB/s
HTTP streaming	negligible overhead
So end-to-end expected:

10–25 MB/s sustained (LAN)

Not 1.75.

So yes: something is wrong relative to design intent.

2. The key red flag in your measurement

You observed:

487MB in 4:38 with 1.75 MB/s

That number is extremely characteristic of:

👉 Stop-and-wait request serialization

Not bandwidth saturation.

Because:

1.75 MB/s ≈ ~1–2 second latency per 2–4MB chunk
That is exactly what you get when requests are not pipelined
3. The most likely root cause (ranked)
🟥 #1: No true streaming pipeline (highest probability)

Even if you think you're doing ranged streaming, what often happens is:

PUT chunk → wait for full server commit → response → next chunk

That destroys throughput.

Symptom match:
stable but low throughput (~1–3 MB/s)
no CPU bottleneck
no SD bottleneck signs
consistent per-session speed

👉 This matches your result perfectly

🟧 #2: Server is committing synchronously per chunk

If server does:

write chunk to disk
flush
compute hash
update DB
THEN respond

you have effectively turned your system into:

“distributed fsync per 4MB”

That will cap you around exactly what you saw.

🟨 #3: Chunk size accidentally too small

If config drift happened and you're using:

1MB or 512KB windows

Then:

HTTP overhead dominates
TLS + request overhead stacks up

This alone can drop you into the 1–3 MB/s range.

🟦 #4: Keep-alive not actually reused

If any of these are true:

curl handle recreated per request
server closes connection after response
HTTP/1.0 fallback
proxy interference

Then every chunk pays:

TCP handshake + TLS setup + slow start

That also lands you right where you are.

4. What is very unlikely here

These are probably NOT the issue:

SD card speed (would still allow higher bursts)
Wi-Fi hardware limit (you’d see more like 5–15 MB/s floor)
xxHash cost (negligible)
CRC32 cost (negligible)
5. The key architectural insight

Your system only achieves “fast transfer” if this is true:

A single connection streams continuously with backpressure, not discrete request cycles.

Right now your measured speed strongly suggests:

request → wait → request → wait → request → wait

instead of:

stream → stream → stream → stream
6. Why this feels contradictory

You designed:

“ranged streaming HTTP protocol”

But what you may have implemented is actually:

“batched HTTP upload protocol with streaming chunks”

Those are fundamentally different.

7. What I would check first (high signal)
1. Count requests per transfer

For 487MB:

at 32MB chunks → ~16 requests
at 4MB chunks → ~122 requests ❌ (this would explain everything)
2. Measure per-request latency on server logs

Look for:

time between request received → response sent

If you see:

500ms–2000ms gaps → server bottleneck
consistent full-chunk pacing → client or protocol bottleneck
3. Verify connection reuse

Check:

is CURLINFO_PRIMARY_IP stable across requests?
do you see new TLS handshakes repeatedly?
8. The blunt conclusion

Yes — your intuition is correct:

This is not the performance you designed for.

And importantly:

This is almost certainly not a hardware limitation — it is a protocol execution detail.

9. If you want the one-line diagnosis

Based on the numbers alone:

Your system is behaving like a sequential request pipeline, not a continuous streaming transport.

If you want, I can help you do 
a precise “trace-level diagnosis checklist” that will pinpoint in under 5 minutes whether the bottleneck is:

client chunking
server response blocking
connection reuse failure
or accidental window size misconfiguration