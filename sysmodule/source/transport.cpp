#include <string.h>
#include <stdio.h>
#include "omnisave.h"

#pragma GCC diagnostic ignored "-Wformat-truncation"

static bool json_str(const char* json, const char* key, char* out, size_t out_sz) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char* p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ') p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_sz)
        out[i++] = *p++;
    out[i] = '\0';
    return i > 0;
}

// ── transport_ack ──────────────────────────────────────────────────────────────
// Reads inbound/<key>.json for transaction_id, then POSTs to /api/v1/sync/ack
// so the server marks the outbound transaction COMPLETED.

void transport_ack(FsFileSystem* sd, const char* key) {
    char meta_path[FS_MAX_PATH], meta_buf[256];
    snprintf(meta_path, sizeof(meta_path), OMNI_ROOT "/inbound/%s.json", key);
    if (!fs_read_text_file(sd, meta_path, meta_buf, sizeof(meta_buf))) {
        fs_log(sd, "ACK_FAIL no_meta key=%s", key);
        return;
    }

    char txn_id[64] = {0};
    if (!json_str(meta_buf, "transaction_id", txn_id, sizeof(txn_id))) {
        fs_log(sd, "ACK_FAIL parse_err key=%s", key);
        return;
    }

    char body[128];
    snprintf(body, sizeof(body), "{\"transaction_id\":\"%s\"}", txn_id);
    int status = http_post_json("/api/v1/sync/ack", body, NULL, 0);
    if (status < 200 || status >= 300) {
        fs_log(sd, "ACK_FAIL status=%d key=%s", status, key);
        return;
    }
    fs_log(sd, "ACK_OK key=%s txn=%s", key, txn_id);
}

// ── transport_error_fail ───────────────────────────────────────────────────────
// Reports a permanent inject failure to the server (READY_FOR_RESTORE → FAILED).
// The server removes the transaction from the device's queue.

void transport_error_fail(FsFileSystem* sd, const char* key) {
    char meta_path[FS_MAX_PATH], meta_buf[256];
    snprintf(meta_path, sizeof(meta_path), OMNI_ROOT "/inbound/%s.json", key);
    if (!fs_read_text_file(sd, meta_path, meta_buf, sizeof(meta_buf))) {
        fs_log(sd, "FAIL_REPORT_SKIP no_meta key=%s", key);
        return;
    }

    char txn_id[64] = {0};
    if (!json_str(meta_buf, "transaction_id", txn_id, sizeof(txn_id))) {
        fs_log(sd, "FAIL_REPORT_SKIP parse_err key=%s", key);
        return;
    }

    char body[128];
    snprintf(body, sizeof(body),
             "{\"transaction_id\":\"%s\",\"error_code\":\"inject_fail\"}", txn_id);
    int status = http_post_json("/api/v1/sync/fail", body, NULL, 0);
    if (status < 200 || status >= 300)
        fs_log(sd, "FAIL_REPORT_ERR status=%d key=%s", status, key);
    else
        fs_log(sd, "FAIL_REPORT_OK key=%s txn=%s", key, txn_id);
}
