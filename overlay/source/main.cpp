#define TESLA_INIT_IMPL
#include <tesla.hpp>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>

#define BASE      "sdmc:/switch/omnisave"
#define STATUS    BASE "/state/status.json"
#define BACKUP    BASE "/state/last_backup.json"
#define RESTORE   BASE "/state/last_restore.json"
#define PAIRING   BASE "/state/pairing.json"
#define SIG_BATCH BASE "/signals/batch_backup.request"
#define ERRORS    BASE "/errors"
#define EVENTS    BASE "/state/events.json"

// ── JSON helpers ───────────────────────────────────────────────────────────────

static bool slurp(const char* path, char* buf, size_t sz) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    size_t n = fread(buf, 1, sz - 1, f);
    fclose(f);
    buf[n] = '\0';
    return n > 0;
}

static bool file_exists(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    fclose(f);
    return true;
}

static bool json_str(const char* json, const char* key, char* out, size_t sz) {
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

static bool json_int(const char* json, const char* key, int* out) {
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

// ── Timestamp formatting ───────────────────────────────────────────────────────

static const char* MONTHS[12] = {
    "Jan","Feb","Mar","Apr","May","Jun",
    "Jul","Aug","Sep","Oct","Nov","Dec"
};

// "2026-06-10T14:30:00Z" -> "Jun 10, 14:30"
static void fmt_ts(const char* iso, char* out, size_t sz) {
    int mo=0, d=0, h=0, m=0;
    if (sscanf(iso, "%*d-%d-%dT%d:%d", &mo, &d, &h, &m) == 4 && mo >= 1 && mo <= 12)
        snprintf(out, sz, "%s %d, %02d:%02d", MONTHS[mo-1], d, h, m);
    else
        snprintf(out, sz, "%.16s", iso);
}

// ── Game name lookup + cache ───────────────────────────────────────────────────

static NsApplicationControlData g_ctrl;  // ~147 KB — global to avoid stack overflow

static bool get_game_name(const char* tid_hex, char* out, size_t sz) {
    if (!tid_hex || !tid_hex[0]) return false;
    u64 tid = strtoull(tid_hex, nullptr, 16);
    if (!tid) return false;
    u64 actual = 0;
    if (R_FAILED(nsGetApplicationControlData(NsApplicationControlSource_Storage,
                 tid, &g_ctrl, sizeof(g_ctrl), &actual)))
        return false;
    for (int i = 0; i < 16; i++) {
        if (g_ctrl.nacp.lang[i].name[0]) {
            snprintf(out, sz, "%s", g_ctrl.nacp.lang[i].name);
            return true;
        }
    }
    return false;
}

static char s_cached_tid[17]  = {};
static char s_cached_name[64] = {};

static void resolve_name(const char* tid, char* out, size_t sz) {
    if (!tid || !tid[0]) { out[0] = '\0'; return; }
    if (strcmp(tid, s_cached_tid) == 0) {
        snprintf(out, sz, "%s", s_cached_name);
        return;
    }
    snprintf(s_cached_tid, sizeof(s_cached_tid), "%.16s", tid);
    s_cached_name[0] = '\0';
    get_game_name(tid, s_cached_name, sizeof(s_cached_name));
    snprintf(out, sz, "%s", s_cached_name);
}

// ── Error list ─────────────────────────────────────────────────────────────────

static int collect_errors(char out[][128], int max) {
    DIR* d = opendir(ERRORS);
    if (!d) return 0;
    int n = 0;
    struct dirent* ent;
    while (n < max && (ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        snprintf(out[n], 128, "%.127s", ent->d_name);
        char* dot = strrchr(out[n], '.');
        if (dot) *dot = '\0';
        for (char* c = out[n]; *c; c++)
            if (*c == '_') *c = ' ';
        n++;
    }
    closedir(d);
    return n;
}

// ── Event log ─────────────────────────────────────────────────────────────────

struct EvEntry { char ts[24]; char msg[64]; };

static int collect_events(EvEntry* out, int max) {
    char buf[4096] = {};
    if (!slurp(EVENTS, buf, sizeof(buf))) return 0;
    const char* p = strchr(buf, '[');
    if (!p) return 0;
    int n = 0;
    while (n < max) {
        const char* obj_start = strchr(p, '{');
        if (!obj_start) break;
        const char* obj_end = strchr(obj_start, '}');
        if (!obj_end) break;
        size_t obj_len = (size_t)(obj_end - obj_start + 1);
        if (obj_len < 256) {
            char obj[256] = {};
            memcpy(obj, obj_start, obj_len);
            json_str(obj, "ts",  out[n].ts,  sizeof(out[n].ts));
            json_str(obj, "msg", out[n].msg, sizeof(out[n].msg));
            if (out[n].msg[0]) n++;
        }
        p = obj_end + 1;
    }
    return n;
}

// ── FlexItem: collapses to zero height when text and value are both empty ──────

class FlexItem : public tsl::elm::ListItem {
public:
    FlexItem() : tsl::elm::ListItem("") {}

    virtual void layout(u16 px, u16 py, u16 pw, u16 ph) override {
        u16 h = (m_text.empty() && m_value.empty())
                ? 0
                : tsl::style::ListItemDefaultHeight;
        this->setBoundaries(this->getX(), this->getY(), this->getWidth(), h);
    }

    virtual void draw(tsl::gfx::Renderer* renderer) override {
        if (m_text.empty() && m_value.empty()) return;
        tsl::elm::ListItem::draw(renderer);
    }

    virtual Element* requestFocus(Element* oldFocus, tsl::FocusDirection direction) override {
        if (m_text.empty() && m_value.empty()) return nullptr;
        return tsl::elm::ListItem::requestFocus(oldFocus, direction);
    }
};

// ── Statics shared across GUI lifetime ────────────────────────────────────────

static u32 s_frame    = 0;
static u8  s_hist_tck = 0;

// ── OmniSaveGui ───────────────────────────────────────────────────────────────

class OmniSaveGui : public tsl::Gui {
    tsl::elm::List*     m_list    = nullptr;
    bool                m_in_game = false;
    char                m_running_tid[17] = {};
    char                m_bk_path[128]    = {};
    char                m_rs_path[128]    = {};

    // Pairing code (shown at top when device is unpaired)
    FlexItem*           m_pair_hdr = nullptr;
    FlexItem*           m_pair     = nullptr;

    // Status section (live, ~1 Hz)
    tsl::elm::ListItem* m_s1   = nullptr;  // FSM label — always populated
    FlexItem*           m_s2   = nullptr;  // game name or secondary text
    FlexItem*           m_s3   = nullptr;  // progress %

    // Last Backup — 4 lines (layout differs by mode)
    FlexItem*           m_bk1  = nullptr;
    FlexItem*           m_bk2  = nullptr;
    FlexItem*           m_bk3  = nullptr;
    FlexItem*           m_bk4  = nullptr;  // username

    // Last Restore — 4 lines (layout differs by mode)
    FlexItem*           m_rs1  = nullptr;
    FlexItem*           m_rs2  = nullptr;
    FlexItem*           m_rs3  = nullptr;
    FlexItem*           m_rs4  = nullptr;  // username

    // Action (home mode only)
    tsl::elm::ListItem* m_act  = nullptr;
    bool                m_act_queued = false;

    // Recent Events
    EvEntry             m_ev[10]       = {};
    int                 m_ev_n         = 0;
    FlexItem*           m_ev_items[10] = {};

    // ── Content helpers ──────────────────────────────────────────────────

    void apply_status(const char* buf, bool offline) {
        m_s2->setText(""); m_s2->setValue("");
        m_s3->setText(""); m_s3->setValue("");

        if (offline) {
            m_s1->setText("Sysmodule offline");
        } else {
            char fsm[32] = {};
            json_str(buf, "fsm_state", fsm, sizeof(fsm));
            if (!fsm[0]) snprintf(fsm, sizeof(fsm), "IDLE");

            if (strcmp(fsm, "IDLE") == 0) {
                m_s1->setText("Up to Date");
            } else if (strcmp(fsm, "RETRY_BACKOFF") == 0) {
                m_s1->setText("Network Issue");
                m_s2->setText("Retrying Shortly...");
            } else if (strcmp(fsm, "READ_ONLY") == 0) {
                m_s1->setText("Paused: Storage Full");
                m_s2->setText("(SD Card >95%)");
            } else if (strcmp(fsm, "UPLOADING") == 0 || strcmp(fsm, "DOWNLOADING") == 0) {
                m_s1->setText(strcmp(fsm, "UPLOADING") == 0 ? "Backing Up" : "Downloading");
                char tid[17] = {}, name[64] = {};
                json_str(buf, "title_id", tid, sizeof(tid));
                resolve_name(tid, name, sizeof(name));
                if (!name[0] && tid[0]) snprintf(name, sizeof(name), "%.8s...", tid);
                m_s2->setText(std::string(name));
                int pct = -1;
                if (json_int(buf, "progress_pct", &pct) && pct >= 0) {
                    char s[12];
                    snprintf(s, sizeof(s), "%d%%", pct);
                    m_s3->setText(std::string(s));
                }
            } else if (strcmp(fsm, "INBOUND_READY") == 0 || strcmp(fsm, "DELIVERING") == 0) {
                m_s1->setText("Applying Save");
                char tid[17] = {}, name[64] = {};
                json_str(buf, "title_id", tid, sizeof(tid));
                resolve_name(tid, name, sizeof(name));
                if (!name[0] && tid[0]) snprintf(name, sizeof(name), "%.8s...", tid);
                m_s2->setText(std::string(name));
            } else {
                // Unknown or partial FSM state from concurrent write — keep last valid label.
            }
        }

        if (m_list) m_list->invalidate();
    }

    // Home mode  (show_name=true):  game name → timestamp → Save #N → username
    // In-game mode (show_name=false): timestamp → Save #N → username → (empty)
    void apply_history(FlexItem* i1, FlexItem* i2, FlexItem* i3, FlexItem* i4,
                       const char* path, bool show_name) {
        char buf[512] = {};
        if (!slurp(path, buf, sizeof(buf))) {
            i1->setText(""); i1->setValue("");
            i2->setText(""); i2->setValue("");
            i3->setText(""); i3->setValue("");
            i4->setText(""); i4->setValue("");
            return;
        }
        char tid[17]={}, at[32]={}, user[33]={};
        int ctr = -1;
        json_str(buf, "title_id",         tid,  sizeof(tid));
        json_str(buf, "completed_at",     at,   sizeof(at));
        json_int(buf, "snapshot_counter", &ctr);
        json_str(buf, "username",         user, sizeof(user));

        char ts[24] = {};
        if (at[0]) fmt_ts(at, ts, sizeof(ts));

        char snap[20] = {};
        if (ctr >= 0) snprintf(snap, sizeof(snap), "Save #%d", ctr);

        if (show_name) {
            char name[64] = {};
            resolve_name(tid, name, sizeof(name));
            if (!name[0] && tid[0]) snprintf(name, sizeof(name), "%.8s...", tid);
            i1->setText(std::string(name)); i1->setValue("");
            i2->setText(std::string(ts));   i2->setValue("");
            i3->setText(std::string(snap)); i3->setValue("");
            i4->setText(std::string(user)); i4->setValue("");
        } else {
            i1->setText(std::string(ts));   i1->setValue("");
            i2->setText(std::string(snap)); i2->setValue("");
            i3->setText(std::string(user)); i3->setValue("");
            i4->setText("");                i4->setValue("");
        }
    }

    void apply_events() {
        m_ev_n = collect_events(m_ev, 10);
        for (int i = 0; i < 10; i++) {
            if (i < m_ev_n && m_ev[i].msg[0]) {
                char ts[24] = {};
                if (m_ev[i].ts[0]) fmt_ts(m_ev[i].ts, ts, sizeof(ts));
                m_ev_items[i]->setText(std::string(m_ev[i].msg));
                m_ev_items[i]->setValue(std::string(ts));
            } else {
                m_ev_items[i]->setText("");
                m_ev_items[i]->setValue("");
            }
        }
        if (m_list) m_list->invalidate();
    }

public:
    virtual tsl::elm::Element* createUI() override {
        // Detect if a game is running; get its title ID for per-game history.
        u64 pid = 0;
        if (R_SUCCEEDED(pmdmntGetApplicationProcessId(&pid)) && pid) {
            u64 tid64 = 0;
            if (R_SUCCEEDED(pminfoGetProgramId(&tid64, pid)) && tid64) {
                m_in_game = true;
                snprintf(m_running_tid, sizeof(m_running_tid), "%016llX",
                         (unsigned long long)tid64);
            }
        }

        if (m_in_game) {
            snprintf(m_bk_path, sizeof(m_bk_path),
                     BASE "/state/last_backup_%s.json",  m_running_tid);
            snprintf(m_rs_path, sizeof(m_rs_path),
                     BASE "/state/last_restore_%s.json", m_running_tid);
        } else {
            snprintf(m_bk_path, sizeof(m_bk_path), "%s", BACKUP);
            snprintf(m_rs_path, sizeof(m_rs_path), "%s", RESTORE);
        }

        auto* frame = new tsl::elm::OverlayFrame("OmniSave", "v1.0.0");
        m_list = new tsl::elm::List();

        // ── Pairing code (both collapse when paired) ───────────────────
        m_pair_hdr = new FlexItem();
        m_pair     = new FlexItem();
        m_list->addItem(m_pair_hdr);
        m_list->addItem(m_pair);

        // ── Current Status ─────────────────────────────────────────────
        m_list->addItem(new tsl::elm::CategoryHeader("Current Status"));
        m_s1 = new tsl::elm::ListItem(""); m_list->addItem(m_s1);
        m_s2 = new FlexItem();              m_list->addItem(m_s2);
        m_s3 = new FlexItem();              m_list->addItem(m_s3);

        // ── Last Backup ────────────────────────────────────────────────
        m_list->addItem(new tsl::elm::CategoryHeader(
            m_in_game ? "Last Backup (This Game)" : "Last Backup"));
        m_bk1 = new FlexItem(); m_list->addItem(m_bk1);
        m_bk2 = new FlexItem(); m_list->addItem(m_bk2);
        m_bk3 = new FlexItem(); m_list->addItem(m_bk3);
        m_bk4 = new FlexItem(); m_list->addItem(m_bk4);

        // ── Last Restore ───────────────────────────────────────────────
        m_list->addItem(new tsl::elm::CategoryHeader(
            m_in_game ? "Last Restore (This Game)" : "Last Restore"));
        m_rs1 = new FlexItem(); m_list->addItem(m_rs1);
        m_rs2 = new FlexItem(); m_list->addItem(m_rs2);
        m_rs3 = new FlexItem(); m_list->addItem(m_rs3);
        m_rs4 = new FlexItem(); m_list->addItem(m_rs4);

        // ── Actions (home mode only — save FS is locked while game is running) ──
        if (!m_in_game) {
            m_list->addItem(new tsl::elm::CategoryHeader("Actions"));
            m_act_queued = file_exists(SIG_BATCH);
            m_act = new tsl::elm::ListItem("Backup All (Slow)");
            m_act->setValue(m_act_queued ? "Queued..." : "A: start");
            m_act->setClickListener([this](u64 keys) -> bool {
                if ((keys & HidNpadButton_A) && !m_act_queued) {
                    FILE* f = fopen(SIG_BATCH, "w");
                    if (f) { fputc('1', f); fclose(f); }
                    m_act->setValue("Queued...");
                    m_act_queued = true;
                }
                return false;
            });
            m_list->addItem(m_act);
        }

        // ── Errors (static — read once on open) ───────────────────────
        char errs[6][128];
        int err_n = collect_errors(errs, 6);
        if (err_n > 0) {
            m_list->addItem(new tsl::elm::CategoryHeader("Errors"));
            for (int i = 0; i < err_n; i++)
                m_list->addItem(new tsl::elm::ListItem(std::string(errs[i])));
        }

        // ── Recent Events ──────────────────────────────────────────────
        m_list->addItem(new tsl::elm::CategoryHeader("Recent Events"));
        for (int i = 0; i < 10; i++) {
            m_ev_items[i] = new FlexItem();
            m_list->addItem(m_ev_items[i]);
        }

        frame->setContent(m_list);

        // Initial pairing code check
        {
            char pair_buf[64] = {};
            char code[7] = {};
            if (slurp(PAIRING, pair_buf, sizeof(pair_buf)) &&
                json_str(pair_buf, "code", code, sizeof(code)) && code[0]) {
                m_pair_hdr->setText("Pair code");
                m_pair->setText(code);
            }
        }

        char st[1024] = {};
        bool ok = slurp(STATUS, st, sizeof(st));
        apply_status(st, !ok);
        apply_history(m_bk1, m_bk2, m_bk3, m_bk4, m_bk_path, !m_in_game);
        apply_history(m_rs1, m_rs2, m_rs3, m_rs4, m_rs_path, !m_in_game);
        apply_events();

        return frame;
    }

    // ~1 Hz status refresh; ~30 s history + events refresh.
    // NEVER calls changeTo — stack stays at depth 1, B always works.
    virtual void update() override {
        if ((++s_frame & 63) != 0) return;

        // Pairing code — refresh every tick (needs to appear/disappear promptly)
        {
            char pair_buf[64] = {};
            char code[7] = {};
            if (slurp(PAIRING, pair_buf, sizeof(pair_buf)) &&
                json_str(pair_buf, "code", code, sizeof(code)) && code[0]) {
                m_pair_hdr->setText("Pair code");
                m_pair->setText(code);
            } else {
                m_pair_hdr->setText("");
                m_pair->setText("");
            }
        }

        char st[1024] = {};
        bool ok = slurp(STATUS, st, sizeof(st));
        apply_status(st, !ok);

        if (++s_hist_tck >= 30) {
            s_hist_tck = 0;
            apply_history(m_bk1, m_bk2, m_bk3, m_bk4, m_bk_path, !m_in_game);
            apply_history(m_rs1, m_rs2, m_rs3, m_rs4, m_rs_path, !m_in_game);
            apply_events();
        }
    }

    virtual bool handleInput(u64 keysDown, u64 keysHeld,
                             const HidTouchState& touchPos,
                             HidAnalogStickState joyStickPosLeft,
                             HidAnalogStickState joyStickPosRight) override {
        (void)keysHeld; (void)touchPos; (void)joyStickPosLeft; (void)joyStickPosRight;
        if (keysDown & HidNpadButton_B) {
            tsl::goBack();
            return true;
        }
        return false;
    }
};

// ── OmniSaveOverlay ───────────────────────────────────────────────────────────

class OmniSaveOverlay : public tsl::Overlay {
public:
    virtual void initServices() override {
        fsdevMountSdmc();
        nsInitialize();
        pmdmntInitialize();
        pminfoInitialize();
    }
    virtual void exitServices() override {
        pminfoExit();
        pmdmntExit();
        nsExit();
        fsdevUnmountAll();
    }
    // No onShow() override: single GUI from loadInitialGui() persists and
    // updates itself in-place. Stack depth is always 1; B always closes cleanly.
    virtual std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<OmniSaveGui>();
    }
};

int main(int argc, char** argv) {
    return tsl::loop<OmniSaveOverlay>(argc, argv);
}
