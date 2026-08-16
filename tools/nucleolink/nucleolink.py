#!/usr/bin/env python3
# NucleoLink — PC companion for the web OS over USB (no Wi-Fi).
#
# Open http://localhost:9100 and you get the FULL NucleoOS web shell, served over the USB cable.
# Every browser request is framed and sent to the device, whose CDC<->127.0.0.1:80 proxy forwards it
# to the on-device httpd (the same server that serves the web OS over Wi-Fi) and streams the reply
# back. Cross-platform (pyserial + stdlib) — Linux/macOS/Windows.
#
# Requires the device flashed with Phase-3 firmware and in USB-web mode (USB app -> key W).
# Run (this machine): /home/index/.espressif/python_env/idf5.3_py3.11_env/bin/python tools/nucleolink/nucleolink.py
import sys, os, time, threading, struct, webbrowser, mimetypes
from pathlib import Path
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial missing — run with the ESP-IDF python (…/idf5.3_py3.11_env/bin/python)")

PORT = 9100
# Static assets (shell + app UIs) are served from a LOCAL copy — the serial link is ~1 KB/s, far too
# slow for shell.js/wasm. Only dynamic /api/* (live device data) crosses the cable. This mirrors the
# device's webfs URL map: "/"→www/shell/, "/apps/<id>/x"→apps/<id>/www/x. The copy is deploy/sd-master
# (assembled by sd_deploy.py, identical to what was flashed to the SD).
REPO = Path(__file__).resolve().parents[2]
LOCAL_ROOT = REPO / "deploy" / "sd-master"
mimetypes.add_type("application/javascript", ".js")
mimetypes.add_type("application/wasm", ".wasm")
mimetypes.add_type("application/manifest+json", ".webmanifest")

def local_file(path: str):
    """Map a URL path to a local file (mirrors the device webfs), or None. Returns (bytes, ctype, gzip)."""
    p = path.split("?", 1)[0].split("#", 1)[0]
    if p == "/" or p == "":
        rel = "www/shell/index.html"
    elif p.startswith("/apps/"):
        parts = p[len("/apps/"):].split("/", 1)          # <id>/<rest>
        if len(parts) != 2 or not parts[1]:
            return None
        rel = f"apps/{parts[0]}/www/{parts[1]}"
    else:
        rel = "www/shell" + p                            # shell root (shell.js, style.css, /i18n/…, /nucleo-i18n.js)
    base = (LOCAL_ROOT / rel).resolve()
    try:
        base.relative_to(LOCAL_ROOT.resolve())           # no path traversal
    except ValueError:
        return None
    ctype = mimetypes.guess_type(str(base))[0] or "application/octet-stream"
    if base.is_file():
        return base.read_bytes(), ctype, False
    gz = base.with_name(base.name + ".gz")               # device serves <file>.gz first
    if gz.is_file():
        return gz.read_bytes(), ctype, True
    return None
HOPHDR = {"connection", "keep-alive", "proxy-authenticate", "proxy-authorization",
          "te", "trailers", "transfer-encoding", "upgrade", "host", "content-length"}

def find_device():
    for p in list_ports.comports():
        blob = " ".join(str(x) for x in (p.description, p.product, p.manufacturer, p.interface) if x)
        if "NucleoOS" in blob or "Web Link" in blob:
            return p.device
    for p in list_ports.comports():
        if "ACM" in p.device or "usbmodem" in p.device:
            return p.device
    return None

class Link:
    """One framed HTTP exchange at a time over the CDC line (the device is single-task)."""
    def __init__(self, port):
        self.port = port
        self.ser = serial.Serial(port, 115200, timeout=15)
        self.lock = threading.Lock()
        time.sleep(0.2); self.ser.reset_input_buffer()

    def _read_exact(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.ser.read(n - len(buf))
            if not chunk:
                raise TimeoutError("device silent")
            buf += chunk
        return buf

    def exchange(self, raw_request: bytes) -> bytes:
        """Send one raw HTTP request, return the raw HTTP response (assembled from chunk frames)."""
        with self.lock:
            self.ser.reset_input_buffer()
            self.ser.write(struct.pack("<I", len(raw_request)))
            self.ser.write(raw_request)
            self.ser.flush()
            out = b""
            while True:
                (clen,) = struct.unpack("<I", self._read_exact(4))
                if clen == 0:
                    break
                out += self._read_exact(clen)
            return out

LINK = None

def build_raw(method, path, headers, body: bytes) -> bytes:
    lines = [f"{method} {path} HTTP/1.1", "Host: 127.0.0.1", "Connection: close"]
    for k, v in headers.items():
        if k.lower() in HOPHDR:
            continue
        lines.append(f"{k}: {v}")
    if body:
        lines.append(f"Content-Length: {len(body)}")
    return ("\r\n".join(lines) + "\r\n\r\n").encode() + (body or b"")

def parse_response(raw: bytes):
    if not raw:
        return 504, [("Content-Type", "text/plain")], b"device silent"
    head, _, body = raw.partition(b"\r\n\r\n")
    lines = head.split(b"\r\n")
    try:
        status = int(lines[0].split(b" ")[1])
    except Exception:
        status = 502
    headers = []
    for ln in lines[1:]:
        if b":" not in ln:
            continue
        k, _, v = ln.partition(b":")
        if k.strip().lower().decode(errors="replace") in HOPHDR:
            continue
        headers.append((k.strip().decode(errors="replace"), v.strip().decode(errors="replace")))
    return status, headers, body

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass

    def _serve_local(self):
        hit = local_file(self.path)
        if not hit:
            return False
        body, ctype, gz = hit
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        if gz:
            self.send_header("Content-Encoding", "gzip")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)
        return True

    def _proxy(self):
        # /ws (live events) is a WebSocket upgrade — not carried over the MVP transport; the shell
        # degrades gracefully to /api/status polling.
        if self.path.startswith("/ws"):
            self.send_response(501); self.send_header("Content-Length", "0"); self.end_headers(); return
        # Static assets from the LOCAL copy (fast). Only /api/* goes to the device over the cable.
        if not self.path.startswith("/api/") and self.command in ("GET", "HEAD") and self._serve_local():
            return
        clen = int(self.headers.get("Content-Length", 0) or 0)
        body = self.rfile.read(clen) if clen else b""
        raw_req = build_raw(self.command, self.path, dict(self.headers), body)
        try:
            status, headers, rbody = parse_response(LINK.exchange(raw_req))
        except Exception as e:
            self.send_response(504); self.send_header("Content-Length", "0"); self.end_headers()
            print("proxy error:", e); return
        self.send_response(status)
        sent_len = False
        for k, v in headers:
            if k.lower() == "content-length": sent_len = True
            self.send_header(k, v)
        if not sent_len:
            self.send_header("Content-Length", str(len(rbody)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(rbody)

    do_GET = do_POST = do_PUT = do_DELETE = do_HEAD = do_PATCH = _proxy

def main():
    global LINK
    port = sys.argv[1] if len(sys.argv) > 1 else find_device()
    if not port:
        sys.exit("no device serial port — is the Cardputer in USB Web mode? (USB app -> W)")
    LINK = Link(port)
    url = f"http://localhost:{PORT}"
    print(f"NucleoLink: device on {port}")
    print(f"NucleoLink: web OS over USB at {url}")
    try: webbrowser.open(url)
    except Exception: pass
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()

if __name__ == "__main__":
    main()
