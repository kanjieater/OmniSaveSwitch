#include "status_parser.hpp"
#include "parse_helpers.hpp"
#include <cstring>
#include <cstdio>

void parse_status(
    const char* json,
    bool offline,
    StatusViewModel& out,
    void (*resolve_name)(const char* tid, char* name, size_t sz))
{
    out = {};

    if (offline) {
        snprintf(out.label, sizeof(out.label), "Sysmodule offline");
        return;
    }

    char fsm[32] = {};
    ov_json_str(json, "fsm_state", fsm, sizeof(fsm));
    if (!fsm[0]) snprintf(fsm, sizeof(fsm), "IDLE");

    if (strcmp(fsm, "IDLE") == 0) {
        snprintf(out.label, sizeof(out.label), "Up to Date");

    } else if (strcmp(fsm, "RETRY_BACKOFF") == 0) {
        snprintf(out.label,     sizeof(out.label),     "Network Issue");
        snprintf(out.secondary, sizeof(out.secondary), "Retrying Shortly...");

    } else if (strcmp(fsm, "UPLOADING") == 0 || strcmp(fsm, "DOWNLOADING") == 0) {
        const bool up = strcmp(fsm, "UPLOADING") == 0;
        snprintf(out.label, sizeof(out.label), "%s", up ? "Backing Up" : "Downloading");
        char tid[17] = {};
        ov_json_str(json, "title_id", tid, sizeof(tid));
        if (resolve_name && tid[0])
            resolve_name(tid, out.secondary, sizeof(out.secondary));
        int pct = -1;
        if (ov_json_int(json, "progress_pct", &pct) && pct >= 0)
            snprintf(out.progress, sizeof(out.progress), "%d%%", pct);

    } else if (strcmp(fsm, "INBOUND_READY") == 0 || strcmp(fsm, "DELIVERING") == 0) {
        snprintf(out.label, sizeof(out.label), "Applying Save");
        char tid[17] = {};
        ov_json_str(json, "title_id", tid, sizeof(tid));
        if (resolve_name && tid[0])
            resolve_name(tid, out.secondary, sizeof(out.secondary));

    } else {
        snprintf(out.label, sizeof(out.label), "%.63s", fsm);
    }
}
