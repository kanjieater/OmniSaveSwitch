Understood. We are locking the architecture and stripping the hot loop. No redesigns, no sleep hacks, just pure overhead reduction.

Here is the exact execution plan to hand off for implementation.

### Implementation Plan: Hot-Path Throughput Optimization

**Objective:** Optimize the sysmodule network hot loop by minimizing per-window `libcurl` overhead and tuning TCP/buffers. Do not alter the 64MB windowing, recovery logic, or FSM architecture.

#### 1. TCP Keepalive Implementation

**File:** `sysmodule/source/http.cpp`
**Action:** Add the following to both `http_upload_handle_create()` and `http_download_handle_create()` to prevent NAT/router teardown during SD card stalls.

* `CURLOPT_TCP_KEEPALIVE`, `1L`
* `CURLOPT_TCP_KEEPIDLE`, `30L`
* `CURLOPT_TCP_KEEPINTVL`, `10L`

#### 2. Download Buffer Scaling

**File:** `sysmodule/source/http.cpp`
**Action:** In `http_download_handle_create()`, increase the internal buffer size to match the upload buffer and reduce `fsFileWrite` IPC frequency.

* `CURLOPT_BUFFERSIZE`, `2097152L` (2MB)

#### 3. Per-Window Overhead Removal

**File:** `sysmodule/source/http.cpp`
**Action:** Strip all static setups out of the hot loop functions (`http_put_window` and `http_get_window_validated`).

* **Move Callbacks:** Set `CURLOPT_READFUNCTION` and `CURLOPT_WRITEFUNCTION` once inside the respective `*_handle_create()` functions.
* **Move Headers:** Do not allocate and free `curl_slist` inside the window loop.
* Allocate the static headers (e.g., `Content-Type: application/octet-stream`) in `*_handle_create()`.
* Attach the `curl_slist` pointer to the handle or a tracking struct.
* Free the `curl_slist` only when `http_handle_close()` is called.


* **Dynamic Headers Only:** If a header must change per window (e.g., `Range`), only allocate and free that specific dynamic header within the loop, or use `libcurl`'s built-in `CURLOPT_RANGE` / `CURLOPT_RESUME_FROM_LARGE` to avoid manual header string formatting entirely.

#### 4. Sleep & Disconnect Handling (Strict No-Op)

**Action:** Do not write any code to detect, block, or manage Horizon OS sleep states.
**Expected Behavior:** If the console sleeps, the TCP connection will sever. `curl_easy_perform` will return an error. The sysmodule will log the failure, exit the transfer loop, and cleanly resume from the last known `verified_bytes` checkpoint upon wake/retry.