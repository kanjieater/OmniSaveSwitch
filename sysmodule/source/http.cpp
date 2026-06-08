#define XXH_INLINE_ALL
#include "xxhash.h"
#include <stdio.h>
#include <string.h>
#include <curl/curl.h>
#include "omnisave.h"

#pragma GCC diagnostic ignored "-Wformat-truncation"

char g_http_last_err[CURL_ERROR_SIZE] = {0};

// Session-scoped header lists — strictly one per CURL session, never shared.
static struct curl_slist* s_upload_hdrs   = NULL;
static struct curl_slist* s_download_hdrs = NULL;

static void build_url(char* out, size_t sz, const char* path) {
    snprintf(out, sz, "%s://%s:%u%s",
        g_config.server_scheme, g_config.server_host,
        (unsigned)g_config.server_port, path);
}

// ---------------------------------------------------------------------------
// Write callbacks
// ---------------------------------------------------------------------------

typedef struct { char* buf; size_t sz; size_t pos; bool overflow; } WriteBuf;

static size_t cb_write_buf(char* data, size_t n, size_t m, void* p) {
    WriteBuf* wb = (WriteBuf*)p;
    size_t bytes = n * m;
    if (wb->buf) {
        size_t room = wb->sz - wb->pos - 1;
        if (bytes > room) { wb->overflow = true; return 0; }
        memcpy(wb->buf + wb->pos, data, bytes);
        wb->pos += bytes;
        wb->buf[wb->pos] = '\0';
    }
    return bytes;
}

// Write callback for validated window download.
// Invariant: write offset is always base_offset + stream_pos — never a mutable cursor.
typedef struct {
    FsFile*         f;
    s64             base_offset;          // immutable — set once at window start
    s64             stream_pos;           // total bytes received in this curl chain
    s64             verified_bytes;       // last fully-validated absolute offset
    s64             checkpoint_size;
    s64             checkpoint_bytes_rem; // bytes until next checkpoint boundary
    XXH32_state_t   hash_state;
    const uint32_t* ledger;              // points into ledger->h[] at ledger_start_idx
    int             ledger_idx;
    int             ledger_count;
    s64             total_bytes;
    bool            failed;
} WindowWriteCtx;

static size_t cb_write_window(char* data, size_t n, size_t m, void* p) {
    WindowWriteCtx* ctx = (WindowWriteCtx*)p;
    size_t total_in = n * m;
    if (ctx->failed) return 0;

    fsFileWrite(ctx->f, ctx->base_offset + ctx->stream_pos,
                (const u8*)data, total_in, FsWriteOption_None);
    ctx->stream_pos += (s64)total_in;

    size_t pos = 0;
    while (pos < total_in) {
        if (ctx->ledger_idx >= ctx->ledger_count) {
            ctx->failed = true;
            return 0;
        }
        size_t take = total_in - pos;
        if ((s64)take > ctx->checkpoint_bytes_rem)
            take = (size_t)ctx->checkpoint_bytes_rem;
        XXH32_update(&ctx->hash_state, data + pos, take);
        ctx->checkpoint_bytes_rem -= (s64)take;
        pos += take;

        if (ctx->checkpoint_bytes_rem == 0) {
            uint32_t digest = XXH32_digest(&ctx->hash_state);
            if (digest != ctx->ledger[ctx->ledger_idx]) {
                ctx->failed = true;
                return 0;
            }
            ctx->verified_bytes += ctx->checkpoint_size;
            if (ctx->verified_bytes > ctx->total_bytes)
                ctx->verified_bytes = ctx->total_bytes;
            ctx->ledger_idx++;
            XXH32_reset(&ctx->hash_state, 0);
            s64 rem = ctx->total_bytes - ctx->verified_bytes;
            ctx->checkpoint_bytes_rem = (rem > ctx->checkpoint_size) ? ctx->checkpoint_size : rem;
        }
    }
    return total_in;
}

// ---------------------------------------------------------------------------
// Read callback (file upload)
// ---------------------------------------------------------------------------

typedef struct { FsFile* f; s64 offset; s64 size; } ReadCtx;

static size_t cb_read(char* buf, size_t n, size_t m, void* p) {
    ReadCtx* ctx = (ReadCtx*)p;
    size_t want = n * m;
    s64 left = ctx->size - ctx->offset;
    if (left <= 0) return 0;
    if ((s64)want > left) want = (size_t)left;
    u64 nr = 0;
    if (R_FAILED(fsFileRead(ctx->f, ctx->offset, (u8*)buf, want, FsReadOption_None, &nr)))
        return CURL_READFUNC_ABORT;
    ctx->offset += (s64)nr;
    return (size_t)nr;
}

// ---------------------------------------------------------------------------
// Sleep-abort progress callback
// ---------------------------------------------------------------------------

static int progress_cb(void* p, curl_off_t dt, curl_off_t dn,
                       curl_off_t ut, curl_off_t un) {
    (void)p; (void)dt; (void)dn; (void)ut; (void)un;
    return s_system_sleeping ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Shared curl setup / teardown
// ---------------------------------------------------------------------------

static CURL* make_curl(const char* url, char errbuf[CURL_ERROR_SIZE]) {
    CURL* c = curl_easy_init();
    if (!c) return NULL;
    errbuf[0] = '\0';
    long tls_verify = (g_config.verify_tls && strcmp(g_config.server_scheme, "https") == 0) ? 1L : 0L;
    curl_easy_setopt(c, CURLOPT_URL,              url);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER,      errbuf);
    curl_easy_setopt(c, CURLOPT_USERAGENT,        "OmniSave-Sysmodule/" OMNISAVE_VERSION);
    curl_easy_setopt(c, CURLOPT_IPRESOLVE,        CURL_IPRESOLVE_V4);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER,   tls_verify);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST,   tls_verify ? 2L : 0L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT,   10L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS,       0L);
    return c;
}


static int finish_curl(CURL* c, char errbuf[CURL_ERROR_SIZE]) {
    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    if (rc == CURLE_OK) {
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    } else {
        snprintf(g_http_last_err, sizeof(g_http_last_err),
                 "curl: %s", errbuf[0] ? errbuf : curl_easy_strerror(rc));
    }
    curl_easy_cleanup(c);
    return (int)status;
}

static struct curl_slist* append_auth_header_early(struct curl_slist* hdrs) {
    if (g_config.device_token[0] == '\0') return hdrs;
    char buf[280];
    snprintf(buf, sizeof(buf), "Authorization: Bearer %s", g_config.device_token);
    return curl_slist_append(hdrs, buf);
}

// ---------------------------------------------------------------------------
// Session-scoped handle API
// Create once before window loop; reuse across curl_easy_perform calls;
// libcurl keeps the TCP connection alive when host:port is unchanged.
// ---------------------------------------------------------------------------

CURL* http_upload_handle_create(void) {
    CURL* c = curl_easy_init();
    if (!c) return NULL;
    g_http_last_err[0] = '\0';
    long tls_verify = (g_config.verify_tls && strcmp(g_config.server_scheme, "https") == 0) ? 1L : 0L;
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER,       g_http_last_err);
    curl_easy_setopt(c, CURLOPT_USERAGENT,         "OmniSave-Sysmodule/" OMNISAVE_VERSION);
    curl_easy_setopt(c, CURLOPT_IPRESOLVE,         CURL_IPRESOLVE_V4);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER,    tls_verify);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST,    tls_verify ? 2L : 0L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT,    10L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION,  progress_cb);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS,        0L);
    curl_easy_setopt(c, CURLOPT_UPLOAD,            1L);
    curl_easy_setopt(c, CURLOPT_TCP_NODELAY,       1L);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE,     1L);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPIDLE,      30L);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPINTVL,     10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,           600L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT,   4096L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME,    30L);
    // Static per-session options: callbacks and headers set once, not per window.
    // Callbacks are stateless — all per-window state is passed via READDATA/WRITEDATA.
    curl_easy_setopt(c, CURLOPT_READFUNCTION,      cb_read);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,     cb_write_buf);
    char id_hdr[128];
    snprintf(id_hdr, sizeof(id_hdr), "X-Device-ID: %s", s_device_id);
    s_upload_hdrs = curl_slist_append(NULL, "Expect:");
    s_upload_hdrs = curl_slist_append(s_upload_hdrs, id_hdr);
    s_upload_hdrs = append_auth_header_early(s_upload_hdrs);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER,        s_upload_hdrs);
    return c;
}

CURL* http_download_handle_create(void) {
    CURL* c = curl_easy_init();
    if (!c) return NULL;
    g_http_last_err[0] = '\0';
    long tls_verify = (g_config.verify_tls && strcmp(g_config.server_scheme, "https") == 0) ? 1L : 0L;
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER,      g_http_last_err);
    curl_easy_setopt(c, CURLOPT_USERAGENT,        "OmniSave-Sysmodule/" OMNISAVE_VERSION);
    curl_easy_setopt(c, CURLOPT_IPRESOLVE,        CURL_IPRESOLVE_V4);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER,   tls_verify);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST,   tls_verify ? 2L : 0L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT,   10L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS,       0L);
    curl_easy_setopt(c, CURLOPT_TCP_NODELAY,      1L);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPALIVE,    1L);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPIDLE,     30L);
    curl_easy_setopt(c, CURLOPT_TCP_KEEPINTVL,    10L);
    curl_easy_setopt(c, CURLOPT_BUFFERSIZE,       (long)(2 * 1024 * 1024));
    curl_easy_setopt(c, CURLOPT_TIMEOUT,          600L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT,  4096L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME,   30L);
    // Stateless write callback + session headers set once, not per window.
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,    cb_write_window);
    char id_hdr[128];
    snprintf(id_hdr, sizeof(id_hdr), "X-Device-ID: %s", s_device_id);
    s_download_hdrs = curl_slist_append(NULL, id_hdr);
    s_download_hdrs = append_auth_header_early(s_download_hdrs);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER,       s_download_hdrs);
    return c;
}

void http_handle_close(CURL* c) {
    curl_slist_free_all(s_upload_hdrs);   s_upload_hdrs   = NULL;
    curl_slist_free_all(s_download_hdrs); s_download_hdrs = NULL;
    curl_easy_cleanup(c);
}

static struct curl_slist* append_device_id(struct curl_slist* hdrs) {
    char buf[128];
    snprintf(buf, sizeof(buf), "X-Device-ID: %s", s_device_id);
    return curl_slist_append(hdrs, buf);
}

static struct curl_slist* append_auth_header(struct curl_slist* hdrs) {
    if (g_config.device_token[0] == '\0') return hdrs;
    char buf[280];
    snprintf(buf, sizeof(buf), "Authorization: Bearer %s", g_config.device_token);
    return curl_slist_append(hdrs, buf);
}

// Find first occurrence of "key": N and return N, or def on failure.
static long long json_int64(const char* buf, const char* key, long long def) {
    if (!buf) return def;
    const char* p = strstr(buf, key);
    if (!p) return def;
    p += strlen(key);
    while (*p == ':' || *p == ' ') p++;
    long long val = 0;
    if (sscanf(p, "%lld", &val) != 1) return def;
    return val;
}

// ---------------------------------------------------------------------------
// V2: Window upload
// ---------------------------------------------------------------------------

// PUT raw bytes from file at offset.
// c must be a handle from http_upload_handle_create() — reused across windows.
// Returns server_verified_bytes on success, -1 on transport/parse err, -2 on sleep.
s64 http_put_window(CURL* c, const char* url_path, FsFileSystem* sd,
                    const char* sd_path, s64 offset, s64 window_len,
                    char* resp_buf, int resp_sz) {
    FsFile file;
    if (R_FAILED(fsFsOpenFile(sd, sd_path, FsOpenMode_Read, &file))) return -1;

    char full_path[512];
    snprintf(full_path, sizeof(full_path), "%s?offset=%lld", url_path, (long long)offset);
    char url[512];
    build_url(url, sizeof(url), full_path);

    g_http_last_err[0] = '\0';

    ReadCtx rctx = { &file, offset, offset + window_len };
    WriteBuf wb   = { resp_buf, (size_t)resp_sz, 0, false };
    if (resp_buf) resp_buf[0] = '\0';

    curl_easy_setopt(c, CURLOPT_URL,              url);
    curl_easy_setopt(c, CURLOPT_READDATA,         &rctx);
    curl_easy_setopt(c, CURLOPT_INFILESIZE_LARGE, (curl_off_t)window_len);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,        &wb);

    CURLcode rc = curl_easy_perform(c);
    long st = 0;
    if (rc == CURLE_OK)
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &st);
    else if (!g_http_last_err[0])
        snprintf(g_http_last_err, sizeof(g_http_last_err), "curl: %s",
                 curl_easy_strerror(rc));
    fsFileClose(&file);

    if (s_system_sleeping) return -2;
    if (rc != CURLE_OK || st < 200 || st >= 300) return -1;

    long long svb = json_int64(resp_buf, "\"server_verified_bytes\"", -1);
    if (svb < 0) return -1;
    return (s64)svb;
}

// ---------------------------------------------------------------------------
// V2: Validated window download
// ---------------------------------------------------------------------------

// GET bytes at offset with inline xxHash32 validation.
// c must be a handle from http_download_handle_create() — reused across windows.
// Returns new verified_bytes (unchanged on hash mismatch — caller truncates to it).
// Returns -1 on transport err, -2 on sleep abort.
s64 http_get_window_validated(CURL* c, const char* url_base_path,
                               FsFileSystem* sd, const char* sd_path,
                               s64 verified_bytes, s64 window_size,
                               const CheckpointLedger* ledger,
                               s64 checkpoint_size, s64 total_bytes) {
    FsFile dest;
    if (R_FAILED(fsFsOpenFile(sd, sd_path, FsOpenMode_Write | FsOpenMode_Append, &dest))) return -1;

    s64 req_length = total_bytes - verified_bytes;
    if (req_length > window_size) req_length = window_size;

    char url[512];
    snprintf(url, sizeof(url), "%s://%s:%u%s?offset=%lld&length=%lld",
             g_config.server_scheme, g_config.server_host,
             (unsigned)g_config.server_port, url_base_path,
             (long long)verified_bytes, (long long)req_length);

    int ledger_start_idx = (int)(verified_bytes / checkpoint_size);
    if (ledger_start_idx < 0 || ledger_start_idx > ledger->count) {
        fsFileClose(&dest);
        return -1;
    }
    s64 first_cp_rem = total_bytes - verified_bytes;
    if (first_cp_rem > checkpoint_size) first_cp_rem = checkpoint_size;

    WindowWriteCtx wctx;
    memset(&wctx, 0, sizeof(wctx));
    wctx.f                    = &dest;
    wctx.base_offset          = verified_bytes;
    wctx.verified_bytes       = verified_bytes;
    wctx.checkpoint_size      = checkpoint_size;
    wctx.checkpoint_bytes_rem = first_cp_rem;
    wctx.ledger               = ledger->h + ledger_start_idx;
    wctx.ledger_count         = ledger->count - ledger_start_idx;
    wctx.total_bytes          = total_bytes;
    XXH32_reset(&wctx.hash_state, 0);

    g_http_last_err[0] = '\0';

    curl_easy_setopt(c, CURLOPT_URL,       url);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &wctx);

    CURLcode rc = curl_easy_perform(c);
    long st = 0;
    if (rc == CURLE_OK)
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &st);
    else if (!g_http_last_err[0])
        snprintf(g_http_last_err, sizeof(g_http_last_err), "curl: %s",
                 curl_easy_strerror(rc));
    fsFsCommit(sd);   // flush FAT dir entry while handle is open — close alone does not update it
    fsFileClose(&dest);

    if (s_system_sleeping) return -2;
    if (rc != CURLE_OK || st < 200 || st >= 300) return -1;
    return wctx.verified_bytes;
}

// ---------------------------------------------------------------------------
// Simple helpers
// ---------------------------------------------------------------------------

int http_post_json(const char* url_path, const char* json_body,
                   char* resp_buf, int resp_sz) {
    char url[512];
    build_url(url, sizeof(url), url_path);

    char errbuf[CURL_ERROR_SIZE];
    CURL* c = make_curl(url, errbuf);
    if (!c) return 0;

    WriteBuf wb = { resp_buf, (size_t)resp_sz, 0, false };
    if (resp_buf) resp_buf[0] = '\0';

    struct curl_slist* hdrs = curl_slist_append(NULL, "Content-Type: application/json");
    hdrs = append_device_id(hdrs);
    hdrs = append_auth_header(hdrs);

    curl_easy_setopt(c, CURLOPT_POST,          1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS,    json_body);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER,    hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, cb_write_buf);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,     &wb);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,       30L);

    int status = finish_curl(c, errbuf);
    curl_slist_free_all(hdrs);
    return status;
}

int http_post_empty(const char* url_path, char* resp_buf, int resp_sz) {
    char url[512];
    build_url(url, sizeof(url), url_path);

    char errbuf[CURL_ERROR_SIZE];
    CURL* c = make_curl(url, errbuf);
    if (!c) return 0;

    WriteBuf wb = { resp_buf, (size_t)resp_sz, 0, false };
    if (resp_buf) resp_buf[0] = '\0';

    struct curl_slist* hdrs = append_device_id(NULL);
    hdrs = append_auth_header(hdrs);
    curl_easy_setopt(c, CURLOPT_POST,          1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, 0L);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER,    hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, cb_write_buf);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,     &wb);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,       30L);

    int status = finish_curl(c, errbuf);
    curl_slist_free_all(hdrs);
    return status;
}

int http_get_body(const char* url_path, char* resp_buf, int resp_sz) {
    char url[512];
    build_url(url, sizeof(url), url_path);

    char errbuf[CURL_ERROR_SIZE];
    CURL* c = make_curl(url, errbuf);
    if (!c) return 0;

    WriteBuf wb = { resp_buf, (size_t)resp_sz, 0, false };
    if (resp_buf) resp_buf[0] = '\0';

    struct curl_slist* hdrs = append_device_id(NULL);
    hdrs = append_auth_header(hdrs);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER,    hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, cb_write_buf);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,     &wb);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,       30L);

    int status = finish_curl(c, errbuf);
    curl_slist_free_all(hdrs);
    return status;
}

int http_ping(const char* path) {
    return http_get_body(path, NULL, 0);
}
