#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "omnisave.h"

OmniSaveConfig g_config;
bool g_debug = false;

static void parse_server_address(const char* val) {
    strncpy(g_config.server_scheme, "https", sizeof(g_config.server_scheme) - 1);
    g_config.server_scheme[sizeof(g_config.server_scheme) - 1] = '\0';
    g_config.server_port = 443;

    const char* p = val;
    const char* scheme_end = strstr(p, "://");
    if (scheme_end) {
        size_t slen = (size_t)(scheme_end - p);
        if (slen < sizeof(g_config.server_scheme)) {
            strncpy(g_config.server_scheme, p, slen);
            g_config.server_scheme[slen] = '\0';
        }
        if (strcmp(g_config.server_scheme, "http") == 0)
            g_config.server_port = 80;
        p = scheme_end + 3;
    }

    const char* host_end = p;
    while (*host_end && *host_end != ':' && *host_end != '/') host_end++;
    size_t hlen = (size_t)(host_end - p);
    if (hlen >= sizeof(g_config.server_host)) hlen = sizeof(g_config.server_host) - 1;
    if (hlen > 0) memcpy(g_config.server_host, p, hlen);
    g_config.server_host[hlen] = '\0';

    if (*host_end == ':') {
        long port = strtol(host_end + 1, NULL, 10);
        if (port > 0 && port <= 65535)
            g_config.server_port = (u16)port;
    }
}

void config_apply_device_suffix(char* buf, size_t buf_sz) {
    if (g_config.device_suffix[0] == '\0') return;
    size_t id_len = strnlen(buf, buf_sz);
    if (id_len < buf_sz - 1)
        snprintf(buf + id_len, buf_sz - id_len, "-%s", g_config.device_suffix);
}

void config_load(FsFileSystem* fs) {
    strncpy(g_config.server_scheme, "https", sizeof(g_config.server_scheme) - 1);
    g_config.server_host[0]    = '\0';
    g_config.server_port       = DEFAULT_SERVER_PORT;
    g_config.device_token[0]   = '\0';
    g_config.device_suffix[0]  = '\0';
    g_config.verify_tls        = true;

    FsFile file;
    Result rc = fsFsOpenFile(fs, OMNI_ROOT "/config.ini", FsOpenMode_Read, &file);
    if (R_FAILED(rc)) {
        fs_write_text_file(fs, OMNI_ROOT "/config.ini",
            "# OmniSave Configuration\n"
            "# server_address=https://your-server.example.com\n"
            "server_address=\n"
            "# device_token=sk_device_...  (generate in OmniSave web UI after first sync)\n"
            "device_token=\n"
            "# nickname=My Switch  (bootstrap hint; server auto-labels on first registration)\n"
            "nickname=\n"
            "# verbose_notifications=0  (1 = also show conflict/sweep events)\n"
            "verbose_notifications=0\n"
            "# verify_tls=0  (set 0 if using a self-signed cert with https)\n"
            "# device_suffix=  (optional; append to device ID to distinguish SysNAND from EmuNAND)\n"
            "# Example for EmuNAND: device_suffix=emu\n"
            "device_suffix=\n");
        return;
    }

    s64 file_size = 0;
    fsFileGetSize(&file, &file_size);
    if (file_size <= 0 || file_size > 4096) { fsFileClose(&file); return; }

    char buf[4097];
    u64 bytes_read = 0;
    rc = fsFileRead(&file, 0, buf, (u64)file_size, FsReadOption_None, &bytes_read);
    fsFileClose(&file);
    if (R_FAILED(rc)) return;
    buf[bytes_read] = '\0';

    char* line = buf;
    while (line && *line) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char* cr = strchr(line, '\r');
        if (cr) *cr = '\0';
        if (*line && *line != '#') {
            char* eq = strchr(line, '=');
            if (eq) {
                *eq = '\0';
                const char* v = eq + 1;
                if (strcmp(line, "server_address") == 0 && v[0] != '\0')
                    parse_server_address(v);
                else if (strcmp(line, "nickname") == 0 && v[0] != '\0') {
                    strncpy(s_device_nickname_hint, v, sizeof(s_device_nickname_hint) - 1);
                    s_device_nickname_hint[sizeof(s_device_nickname_hint) - 1] = '\0';
                } else if (strcmp(line, "device_token") == 0 && v[0] != '\0') {
                    strncpy(g_config.device_token, v, sizeof(g_config.device_token) - 1);
                    g_config.device_token[sizeof(g_config.device_token) - 1] = '\0';
                } else if (strcmp(line, "verbose_notifications") == 0)
                    g_verbose_notif = (v[0] == '1');
                else if (strcmp(line, "debug") == 0)
                    g_debug = (v[0] == '1');
                else if (strcmp(line, "verify_tls") == 0)
                    g_config.verify_tls = (v[0] != '0');
                else if (strcmp(line, "device_suffix") == 0 && v[0] != '\0') {
                    strncpy(g_config.device_suffix, v, sizeof(g_config.device_suffix) - 1);
                    g_config.device_suffix[sizeof(g_config.device_suffix) - 1] = '\0';
                }
            }
        }
        line = nl ? nl + 1 : NULL;
    }
}

void config_set_device_token(FsFileSystem* fs, const char* token) {
    char buf[4097] = {0};
    FsFile file;
    if (R_SUCCEEDED(fsFsOpenFile(fs, OMNI_ROOT "/config.ini", FsOpenMode_Read, &file))) {
        s64 file_size = 0;
        fsFileGetSize(&file, &file_size);
        if (file_size > 0 && file_size < 4096) {
            u64 bytes_read = 0;
            fsFileRead(&file, 0, buf, (u64)file_size, FsReadOption_None, &bytes_read);
            buf[bytes_read] = '\0';
        }
        fsFileClose(&file);
    }

    char out[4097] = {0};
    int pos = 0;
    bool replaced = false;

    const char* line = buf;
    while (line && *line) {
        const char* nl = strchr(line, '\n');
        size_t line_len = nl ? (size_t)(nl - line) : strlen(line);
        while (line_len > 0 && line[line_len - 1] == '\r') line_len--;

        if (!replaced && line_len >= 13 && strncmp(line, "device_token=", 13) == 0) {
            int n = snprintf(out + pos, (size_t)(sizeof(out) - pos),
                             "device_token=%s\n", token);
            if (n > 0) pos += n;
            replaced = true;
        } else if (line_len > 0 && pos + (int)line_len + 1 < (int)sizeof(out)) {
            memcpy(out + pos, line, line_len);
            pos += (int)line_len;
            out[pos++] = '\n';
        }
        line = nl ? nl + 1 : NULL;
    }

    if (!replaced) {
        int n = snprintf(out + pos, (size_t)(sizeof(out) - pos),
                         "device_token=%s\n", token);
        if (n > 0) pos += n;
    }
    if (pos < (int)sizeof(out)) out[pos] = '\0';

    fs_write_text_file(fs, OMNI_ROOT "/config.ini", out);
}
