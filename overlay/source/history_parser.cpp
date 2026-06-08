#include "history_parser.hpp"
#include "parse_helpers.hpp"
#include <cstring>
#include <cstdio>

void parse_history(
    const char* json,
    bool show_name,
    HistoryViewModel& out,
    void (*resolve_name)(const char* tid, char* name, size_t sz))
{
    out = {};

    if (!json || !json[0]) {
        return;  // valid=false, all lines empty
    }
    out.valid = true;

    char tid[17] = {}, at[32] = {};
    int ctr = -1;
    ov_json_str(json, "title_id",         tid, sizeof(tid));
    ov_json_str(json, "completed_at",     at,  sizeof(at));
    ov_json_int(json, "snapshot_counter", &ctr);

    char ts[24] = {};
    if (at[0]) ov_fmt_ts(at, ts, sizeof(ts));

    char snap[20] = {};
    if (ctr >= 0) snprintf(snap, sizeof(snap), "Save #%d", ctr);

    if (show_name) {
        char name[64] = {};
        if (resolve_name && tid[0]) resolve_name(tid, name, sizeof(name));
        snprintf(out.line1, sizeof(out.line1), "%.63s", name);
        snprintf(out.line2, sizeof(out.line2), "%.63s", ts);
        snprintf(out.line3, sizeof(out.line3), "%.63s", snap);
    } else {
        snprintf(out.line1, sizeof(out.line1), "%.63s", ts);
        snprintf(out.line2, sizeof(out.line2), "%.63s", snap);
        // line3 stays empty
    }
}
