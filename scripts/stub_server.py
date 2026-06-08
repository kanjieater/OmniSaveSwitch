#!/usr/bin/env python3
"""
OmniSave stub server — simulates all Switch-facing endpoints for local testing.
Persists uploaded saves to /tmp/omnisave-stub/.

Usage:
    python3 scripts/stub_server.py [--port 8991]
"""
import argparse
import json
import os
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from urllib.parse import urlparse

STUB_ROOT = Path("/tmp/omnisave-stub")
STUB_ROOT.mkdir(parents=True, exist_ok=True)

_snap_counter_lock = threading.Lock()
_snap_counter = 1


def next_snap_id():
    global _snap_counter
    with _snap_counter_lock:
        sid = _snap_counter
        _snap_counter += 1
    return sid


class StubHandler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        print(f"[stub] {self.address_string()} {fmt % args}")

    def send_json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def send_text(self, code, text):
        body = text.encode()
        self.send_response(code)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def read_body(self):
        length = int(self.headers.get("Content-Length", 0))
        return self.rfile.read(length) if length else b""

    # ── Router ──────────────────────────────────────────────────────────────────

    def do_GET(self):
        p = urlparse(self.path).path.rstrip("/")
        parts = p.split("/")

        if p == "/ping":
            return self.send_json(200, {"ok": True})

        # GET /inbound/<device_id>
        if len(parts) == 3 and parts[1] == "inbound":
            return self.handle_inbound(parts[2])

        # GET /upload_status/<key>/<rel_path...>
        if len(parts) >= 4 and parts[1] == "upload_status":
            key = parts[2]
            rel = "/".join(parts[3:])
            return self.handle_upload_status(key, rel)

        # GET /download/<snap_id>/index
        if len(parts) == 4 and parts[1] == "download" and parts[3] == "index":
            return self.handle_download_index(parts[2])

        # GET /download/<snap_id>/data/<rel_path...>
        if len(parts) >= 5 and parts[1] == "download" and parts[3] == "data":
            snap_id = parts[2]
            rel = "/".join(parts[4:])
            return self.handle_download_data(snap_id, rel)

        self.send_json(404, {"error": "not found"})

    def do_PUT(self):
        p = urlparse(self.path).path.rstrip("/")
        parts = p.split("/")

        # PUT /upload/<key>/<rel_path...>
        if len(parts) >= 4 and parts[1] == "upload":
            key = parts[2]
            rel = "/".join(parts[3:])
            return self.handle_upload_file(key, rel)

        self.send_json(404, {"error": "not found"})

    def do_POST(self):
        p = urlparse(self.path).path.rstrip("/")
        parts = p.split("/")

        # POST /upload/<key>/_done
        if len(parts) == 4 and parts[1] == "upload" and parts[3] == "_done":
            return self.handle_upload_done(parts[2])

        # POST /ack/<device_id>/<snap_id>
        if len(parts) == 4 and parts[1] == "ack":
            return self.handle_ack(parts[2], parts[3])

        # POST /sync/baseline  (new spec endpoint — stub only)
        if p == "/sync/baseline":
            return self.handle_baseline()

        self.send_json(404, {"error": "not found"})

    # ── Handlers ────────────────────────────────────────────────────────────────

    def handle_upload_file(self, key, rel):
        dest = STUB_ROOT / "staging" / key / rel
        dest.parent.mkdir(parents=True, exist_ok=True)

        cr = self.headers.get("Content-Range")
        body = self.read_body()

        if cr:
            # Content-Range: bytes start-end/total
            try:
                rng, _ = cr.split("/")
                _, se  = rng.split(" ", 1)
                start, _ = se.split("-")
                offset = int(start)
            except Exception:
                offset = 0
            if offset == 0 or not dest.exists():
                dest.write_bytes(b"\x00" * int(cr.split("/")[1]))
            with open(dest, "r+b") as f:
                f.seek(offset)
                f.write(body)
            # Track how far we've received.
            recv_path = dest.parent / (dest.name + ".recv")
            recv_path.write_text(str(offset + len(body)))
        else:
            dest.write_bytes(body)

        print(f"[stub]   PUT {key}/{rel} ({len(body)} bytes)")
        self.send_json(200, {"ok": True})

    def handle_upload_status(self, key, rel):
        recv_path = STUB_ROOT / "staging" / key / (rel + ".recv")
        received = 0
        if recv_path.exists():
            try:
                received = int(recv_path.read_text().strip())
            except Exception:
                received = 0
        self.send_json(200, {"received_bytes": received})

    def handle_upload_done(self, key):
        staging = STUB_ROOT / "staging" / key
        sid = next_snap_id()
        archive = STUB_ROOT / "archive" / str(sid)
        if staging.exists():
            import shutil
            archive.parent.mkdir(parents=True, exist_ok=True)
            shutil.copytree(str(staging), str(archive))
        print(f"[stub] COMMIT key={key} → snapshot_id={sid}")
        self.send_json(200, {"snapshot_id": sid})

    def handle_inbound(self, device_id):
        # Return any queued inbound entries from STUB_ROOT/inbound/<device_id>/
        inbound_dir = STUB_ROOT / "inbound" / device_id
        lines = []
        if inbound_dir.exists():
            for f in sorted(inbound_dir.iterdir()):
                if f.suffix == ".txt":
                    key, snap_id = f.read_text().strip().split()
                    lines.append(f"{key} {snap_id}")
                    f.unlink()
        self.send_text(200, "\n".join(lines) + "\n" if lines else "")

    def handle_download_index(self, snap_id):
        archive = STUB_ROOT / "archive" / snap_id
        if not archive.exists():
            return self.send_json(404, {"error": "not found"})
        files = [str(f.relative_to(archive))
                 for f in sorted(archive.rglob("*")) if f.is_file()]
        self.send_text(200, "\n".join(files) + "\n")

    def handle_download_data(self, snap_id, rel):
        path = STUB_ROOT / "archive" / snap_id / rel
        if not path.exists() or not path.is_file():
            return self.send_json(404, {"error": "not found"})
        body = path.read_bytes()
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def handle_ack(self, device_id, snap_id):
        print(f"[stub] ACK device={device_id} snap={snap_id}")
        self.send_json(200, {"ok": True})

    def handle_baseline(self):
        body = self.read_body()
        try:
            req = json.loads(body)
        except Exception:
            req = {}
        title_id = req.get("title_id", "unknown")
        lineage_id = f"lineage-{title_id}-{next_snap_id()}"
        print(f"[stub] BASELINE title={title_id} → lineage={lineage_id}")
        self.send_json(200, {"lineage_id": lineage_id, "last_known_head": None})


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8991)
    parser.add_argument("--host", default="0.0.0.0")
    args = parser.parse_args()

    print(f"[stub] OmniSave stub server on {args.host}:{args.port}")
    print(f"[stub] Storing uploads in {STUB_ROOT}")
    server = HTTPServer((args.host, args.port), StubHandler)
    server.serve_forever()


if __name__ == "__main__":
    main()
