#pragma once
// Minimal JSON extraction and timestamp formatting for the overlay.
// All functions are file-scope static to avoid ODR conflicts.

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cstdlib>

static inline bool ov_json_str(const char* json, const char* key, char* out, size_t sz) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char* p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ') p++;
    if (*p != '"') return false;
    p++;
    const char* end = strchr(p, '"');
    if (!end) return false;
    size_t len = (size_t)(end - p);
    if (len >= sz) len = sz - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

static inline bool ov_json_int(const char* json, const char* key, int* out) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char* p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ') p++;
    if (*p == 'n') return false;
    char* endp;
    long v = strtol(p, &endp, 10);
    if (endp == p) return false;
    *out = (int)v;
    return true;
}

static const char* const OV_MONTHS[12] = {
    "Jan","Feb","Mar","Apr","May","Jun",
    "Jul","Aug","Sep","Oct","Nov","Dec"
};

// "2026-06-10T14:30:00Z" -> "Jun 10, 14:30"
static inline void ov_fmt_ts(const char* iso, char* out, size_t sz) {
    int mo = 0, d = 0, h = 0, m = 0;
    if (sscanf(iso, "%*d-%d-%dT%d:%d", &mo, &d, &h, &m) == 4 && mo >= 1 && mo <= 12)
        snprintf(out, sz, "%s %d, %02d:%02d", OV_MONTHS[mo - 1], d, h, m);
    else
        snprintf(out, sz, "%.16s", iso);
}
