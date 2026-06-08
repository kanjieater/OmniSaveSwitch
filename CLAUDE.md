# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Also read `.claude/CLAUDE.md` — it contains detailed rules on code style, error handling, and memory constraints that apply to all tasks.

---

## Output Style — Caveman Mode (always active)

Full spec: [`.claude/skills/caveman/SKILL.md`](.claude/skills/caveman/SKILL.md)

**Always on. Full level. No preamble. No filler. Fragments OK.**

- Drop: articles, pleasantries, hedging, trailing summaries
- Code blocks unchanged. Technical terms exact. Errors quoted exact.
- Switch level: `/caveman lite` | `/caveman ultra` | `normal mode` to exit
- Auto-clarity exception: security warnings, irreversible ops — write full prose, then resume caveman

---

## Build & Deploy

```bash
# Sysmodule (requires Docker with devkitPro image)
./build.sh                        # compile only → sysmodule/exefs.nsp
./deploy.sh                       # build + FTP to all switches in OMNISAVE_SWITCHES

# Overlay (Tesla NX overlay, requires Docker + OMNISAVE_SWITCHES)
./build_overlay.sh                # fetch libtesla, compile, deploy → overlay/omnisave.ovl

# C++ tests
docker compose run --rm test-sysmodule    # full suite + diff-cover (CI gate)
```

CI gate: `docker compose run --rm test-sysmodule` with `diff-cover` at 100% on the diff.

### Task completion checklist

| Changed files | Required steps before declaring done |
|---|---|
| `sysmodule/source/**` | 1. `./deploy.sh` (build + FTP to Switch) |
| `overlay/source/**` | 1. `./build_overlay.sh` (build + deploy overlay) |

### Debugging Switch issues

Always fetch Switch logs via FTP when diagnosing sysmodule behavior — do not wait to be asked:

```bash
# OG Switch
curl --disable-epsv ftp://switch.local:5000/switch/omnisave/events.log

# Lite Switch
curl --disable-epsv ftp://lite.local:5000/switch/omnisave/events.log
```

The `events.log` file on the SD card is the primary diagnostic source for FSM state transitions, upload/download progress, and transport errors.

---

## Architecture

**Sysmodule** (`sysmodule/source/`) — C++ background sysmodule on Horizon OS (title ID `0100000000000001`):
- `main.cpp` — entry point, event threads (applet/sleep/IPC), builds `InputSnapshot` each tick, calls `fsm_tick()`
- `fsm.cpp` — tick-based FSM; transitions: `IDLE → UPLOADING → INBOUND_READY → DELIVERING → RETRY_BACKOFF → IDLE`
- `state.cpp` — atomic JSON writes to `sdmc:/switch/omnisave/state/` (device.json, status.json, lineage/)
- `recovery.cpp` — `recovery_sweep()`: validates outbound dirs, purges corrupted entries on boot
- `transport_upload.cpp` — chunked HTTP upload (`PUT /api/v1/sync/sessions/{id}/chunks/{n}`, `POST .../commit`); 4 MB chunks, 3 retries
- `transport_poll.cpp` — inbound polling and download (`GET /api/v1/sync/queue`, `POST .../claim`, `GET .../chunks/{n}`, `POST /ack`); 4 MB download chunks
- `transport.cpp` — shared HTTP helpers used by both upload and poll
- `save_pack.cpp` / `save_pack_read.cpp` — deterministic ZIP(STORE) format
- `save_dump.cpp` — `dump_all_saves()`: bulk export of all account saves to `outbound/` on boot
- `save_ops.cpp` — save FS open (captures Nintendo Account UID), backup, inject
- `config.cpp` — loads `config.ini` (server address)
- `http.cpp` — libcurl wrappers
- `fs_helpers.cpp` — `path_join`, `copy_dir`, `fs_write_text_file`, `fs_log`
- `notif.cpp` — in-game notification display

The sysmodule is **tick-based**: `main.cpp` assembles a frozen `InputSnapshot` and passes it to `fsm_tick()` each loop iteration. The FSM owns all state transitions.

**Overlay** (`overlay/source/`) — Tesla NX overlay (libtesla) for on-Switch UI control. Communicates with the server via HTTP API. Deployed to `switch/.overlays/omnisave.ovl`.

### Key constraints

**Server address is never in source control.** Lives only in `sdmc:/omnisave/config.ini`.

In C++, absolutely no dynamic allocation (`malloc`/`new`) during active gameplay loops. Rely strictly on the pre-allocated inner heap.

Protocol constants (must match server):
- `CHECKPOINT_SIZE = 4 MB` (xxHash32 granularity)
- `WINDOW_SIZE = 64 MB` (HTTP transport unit)
- `MAX_CHECKPOINTS = 256`
- `OMNISAVE_VERSION = "2.0.0"`

### SD card layout (`sdmc:/omnisave/`)
```
config.ini  ← server_address= only required field
```

### Context / Specs

Design decisions, state machines, and feature specs are in `context/` — compressed for agent context windows. **Always read the relevant file(s) there before implementing any feature.**
- `context/switch-client/` — sysmodule state machine, resilience rules, overlay frontend, snapshot ID format
