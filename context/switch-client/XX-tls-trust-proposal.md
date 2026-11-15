# Proposal: investigate intermittent Switch TLS rejection, preserve verified HTTPS

**Status:** Proposed; **no runtime change in this PR**.
**Component:** `sysmodule` HTTPS client.
**Goal:** Explain and prevent intermittent HTTPS certificate-validation failures while preserving certificate-chain, hostname, and date verification. **Do not assume the system CA store is broken.**

## Current behavior and evidence

### New observation: connection recovered without a known trust change

- **User observed:** saves successfully uploaded **after** the network/TLS failure. This is evidence of eventual successful connectivity and weakens the hypothesis of a permanently missing CA root **if** those transfers used the same HTTPS hostname, active TLS verification, CA configuration, and certificate chain.
- **Not yet established:** whether the successful upload used HTTPS with `verify_tls=1`, the same target hostname/remote endpoint and certificate chain, or whether the device clock, network path, process initialization, firmware, or server certificate changed in between. Verify these before drawing any trust-store conclusion. It is also possible the observed 'network' error came from a different cause than the earlier `bad_certificate` event.
- **Working hypotheses, not conclusions:** temporary device clock drift, sleep/wake/resume or SSL initialization state, network-path/DNS change, different certificate chains from server edges, transient chain delivery, or a genuine trust-store issue. **Successful uploads do not prove which one occurred.**
- **Decision:** diagnosis and error classification are first priority. **Do not ship a CA bundle or upgrade the TLS stack just because a `bad_certificate` event occurred.** Make trust-store changes only after a demonstrated validation-path problem.

- `sysmodule/source/http.cpp` creates ordinary, upload, and download curl handles. Each sets `CURLOPT_SSL_VERIFYPEER` and `CURLOPT_SSL_VERIFYHOST` based on `g_config.verify_tls`. The configuration exposes an unsafe `verify_tls=0` escape hatch and no explicit trust-bundle selection.
- `sysmodule/source/main.cpp::wait_network_ready()` retries `http_ping("/api/health")` ten times; any TLS rejection results in `false`. `sysmodule/source/fsm.cpp` can then label the condition `No Network — Will Retry`, conflating certificate trust and connectivity.
- The build installs `switch-curl` (`sysmodule/Dockerfile`). Its devkitPro package is based on curl 7.69.1 with the **libnx SSL backend**, not an mbedTLS-powered curl backend. Linking separate mbedTLS libraries in `sysmodule/Makefile` does not upgrade curl's active TLS backend.
- The devkitPro switch-curl patch maps underlying Horizon SSL verification errors to coarse curl errors. It calls `sslConnectionGetVerifyCertError()` inside the backend, but that detail is not consistently available at OmniSave's call sites. A bare `bad_certificate` or `CURLE_PEER_FAILED_VERIFICATION` does **not** identify the offending verification condition.
- **Root cause is not yet proven.** An incomplete server chain, unsupported/missing root, outdated firmware trust store, clock, backend incompatibility, and a genuine invalid certificate are distinct possibilities. A persistent CA-root deficit is **less likely** after successful same-endpoint verified HTTPS, but not eliminated until the above controls are checked.

References:
- [OmniSave HTTP implementation](https://github.com/kanjieater/OmniSaveSwitch/blob/main/sysmodule/source/http.cpp)
- [devkitPro switch-curl packaging](https://github.com/devkitPro/pacman-packages/blob/master/switch/curl/PKGBUILD) and [libnx TLS backend patch](https://github.com/devkitPro/pacman-packages/blob/master/switch/curl/switch-curl.patch)
- [uNSS custom CA import](https://github.com/prodeveloper0/uNSS/blob/main/client/source/http.cpp): verifies peer and hostname, conditionally sets `CURLOPT_CAINFO` for `sdmc:/uNSS/cacert.pem`
- [nx.js certificate verification using system CA roots](https://github.com/TooTallNate/nx.js/pull/194)
- [switch-curl certificate-info crash report](https://github.com/devkitPro/pacman-packages/issues/436): beware enabling `CURLOPT_CERTINFO` blindly as a diagnostic shortcut

## Required implementation

### Phase 1 — diagnose on actual hardware

1. **Correlate actual failures with later successful uploads before changing settings.** From timestamped `events.log` and server access logs, identify the failed `/api/health` or upload attempt, next successful HTTPS upload, whether both used the same hostname, certificate chain and `verify_tls` setting, and any intervening sleep/wake, restart, network/DNS/remote-IP, clock adjustment, certificate renewal, or configuration change. Do not assume two events are the same failure type; preserve logs and redact secrets.
2. Reproduce with the configured **dev HTTPS hostname** on a Switch if feasible (intermittent failures might not reproduce on demand). Record firmware version, curl version and active SSL backend (`curl_version_info`), TLS failure code, system clock/timezone accuracy, and relevant sleep/wake/SSL-init events without logging tokens.
3. Obtain the backend's original verification result where possible. In `switch-curl`, `sslConnectionGetVerifyCertError()` is invoked when peer verification fails, but existing translation loses detail. If curl's error buffer and `CURLINFO_SSL_VERIFYRESULT` are insufficient or unsupported, apply a narrowly scoped, versioned backend patch to preserve the numeric libnx result and meaningful error text. Do not guess a diagnosis from `bad_certificate`.
4. Independently inspect the dev endpoint's served chain, intermediates and hostname verification (for example with `openssl s_client -verify_return_error -verify_hostname HOST -servername HOST -connect HOST:443`). Check the Switch clock. Correct server-side incomplete chains at the server, not with client verification bypasses.
5. Record failed-versus-successful evidence, the exact validation error where obtainable, and the resulting tested root-cause hypothesis. If the logs cannot prove it, state *undetermined* and prioritize instrumentation plus repeat capture over speculative trust-store changes. Keep the findings attached to the implementation PR.

### Phase 2 — apply only evidence-backed trust corrections; keep verification strict

1. Centralize TLS configuration for `make_curl()`, `http_upload_handle_create()` and `http_download_handle_create()`. HTTPS must set `CURLOPT_SSL_VERIFYPEER=1L` and `CURLOPT_SSL_VERIFYHOST=2L`. Check all relevant `curl_easy_setopt` results and fail closed on configuration/import errors.
2. Prefer the Switch system trust store when it validates the endpoint. **Only if Phase 1 demonstrates missing/outdated CA roots**, support a maintained, supplemental CA PEM at `sdmc:/switch/omnisave/cacert.pem` through `CURLOPT_CAINFO`. Verify that this **particular libnx curl backend** can open and import the file from a sysmodule, whether it augments system roots, and its PKI/memory/size constraints. uNSS is a pattern, not proof that an unrestricted Mozilla PEM works on every Switch.
3. **Only if supported firmware roots are proven insufficient**, package a maintained CA trust set with releases rather than relying on manual one-off fixes. Use an auditable source such as [curl's Mozilla CA extract](https://curl.se/docs/caextract.html), record version/source/update process, validate against the actual Switch backend, and include it in the SD-card release layout. If a full bundle is unsupported, choose and document an appropriately maintained, backend-compatible root set or update the TLS backend. Never silently truncate the bundle or treat an import failure as success.
4. If a bundle is introduced, define clear behavior for missing or malformed optional/required CA files. If a configured trust source cannot be loaded, fail with an explicit error rather than silently switching to insecure transport.
5. Remove the insecure `verify_tls=0` recommendation from `dist/config.ini` and the generated template in `sysmodule/source/config.cpp`. An existing `verify_tls=0` must not turn off verification; reject or ignore it with a diagnostic and a migration note. Do not automatically downgrade HTTPS to HTTP.
6. Do not pin the server leaf certificate (nor its changing fingerprint). A renewed leaf valid under a trusted CA and matching hostname must continue to work with no client update. Do not fetch trust anchors over an untrusted connection.

### Phase 3 — classify errors without changing the synchronization protocol

1. Introduce a compact transport result/classification for **network unavailable**, **DNS/connectivity**, **TLS handshake**, **certificate validation**, **HTTP response/auth**, and **success**. Preserve existing API return behavior where callers require it; pass the failure class and underlying codes explicitly.
2. Stop interpreting `http_ping`'s TLS rejection as `No Network`. `wait_network_ready()` and its FSM callers should log a clear certificate/TLS failure; do not perform ten blind retries of a deterministic trust rejection. Transient network errors retain bounded retry/backoff.
3. Include hostname, curl code, curl error text, and numeric libnx verification result when available in bounded `events.log` entries. Never log device bearer tokens, full credential-bearing URLs, or PEM contents. Rate-limit repetitive failures; do not suppress the first precise diagnostic.
4. Preserve `context/switch-client/03-resilience-rules.md` invariants: local queued artifacts survive transport failure, no failed certificate can authorize an upload/download/restore, no new recovery state, no changed keeper/transaction logic, and normal recovery after trust is restored.
5. Cover persistent upload/download handles and one-shot API calls equally, including resume after sleep/wake and TLS failures mid-transfer. Do not introduce heap allocations in the active gameplay hot path.

### Expected implementation locations

| Area | Likely files |
| --- | --- |
| One-shot and persistent HTTPS handles | `sysmodule/source/http.cpp`; shared transport types/header as needed |
| Reachability and FSM classification | `sysmodule/source/main.cpp`, `sysmodule/source/fsm.cpp` |
| Config migration and user guidance | `sysmodule/source/config.cpp`, `dist/config.ini`, `README.md` |
| Trust bundle packaging and maintenance (only if required) | `.github/workflows/release-build.yml`, build/deploy scripts, provenance/update documentation |
| Tests and specification alignment | `sysmodule/tests/`, `context/switch-client/03-resilience-rules.md` |

## Acceptance matrix

| Test case | Required result |
| --- | --- |
| Valid public-CA dev HTTPS with correct hostname | Connection and sync succeed |
| Renewed server leaf under the same trusted CA | Works unchanged, no pin update |
| Newly trusted public root (if an updated CA store is proven necessary) | Works after supported trust-store refresh; no unneeded bundle deployed |
| Wrong hostname | Hard TLS reject; explicit diagnostic |
| Unknown CA/self-signed without deliberately trusted CA | Hard TLS reject; explicit diagnostic |
| Expired/not-yet-valid certificate or incorrect clock | Hard TLS reject; explicit diagnostic |
| Incomplete server chain | Reject unless a legitimate trusted path can be built; fix chain at server |
| Missing/corrupted configured CA bundle | Explicit trust-configuration failure; no insecure fallback |
| Network/DNS outage | Clearly not reported as certificate rejection; bounded retry |
| `verify_tls=0` in old config | Verification stays enabled or configuration rejects; never bypasses |
| Mid-upload/download TLS failure and wake/retry | No data loss or injection; resumable per existing checkpoints |
| Device bearer token / headers | Never leaked to diagnostic logs |

Run Catch2/RapidCheck/mock coverage for classification and TLS-option setup, `docker compose run --rm test-sysmodule` (100% diff coverage), `./build.sh`, release packaging checks, and **on-device acceptance on OG and Lite Switch**, including a real dev HTTPS certificate renewal. Document exact firmware/backend versions used.

## Explicit non-goals

- No `verify_tls=0`, `CURLOPT_SSL_VERIFYPEER=0`, `CURLOPT_SSL_VERIFYHOST=0`, plaintext HTTP downgrade, fingerprint/leaf-certificate pinning, or certificate-error suppression.
- No redesign of the save FSM, retry journal, window sizes, or server API.
- No claim that refreshing mbedTLS by itself upgrades the active libnx curl SSL backend.

## Proposal review decision

Start with **failed-versus-successful attempt correlation** and exact TLS error capture, including verified hostname, TLS settings, clock, wake/resume and possible server-edge differences. A permanent CA-root deficiency is less likely given subsequent successful uploads. Do **not** change trust sources or upgrade libraries without evidence; fix transient state or backend initialization if that is what the investigation demonstrates. Then ship centralized strict TLS options, meaningful failure classification, and end-to-end rejection/renewal tests. CA bundle maintenance remains a conditional fallback, not a presumed fix. This PR documents the plan; it does **not** implement or validate the fix.
