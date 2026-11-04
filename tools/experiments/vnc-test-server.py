#!/usr/bin/env python3
"""Minimal RFB 3.8 server for testing the Cardputer VNC prototype (app_vnc.cpp) on the LAN.

Not a product: one client at a time, security type None, encodings Raw and Hextile, DesktopSize. Two sources:
  synthetic  a test desktop (windows, a clock, a scrolling log, a moving block, a notepad showing what the
             Cardputer types, marks where it clicks)
  screen     this PC's primary screen with the mouse pointer drawn in; --control also turns the Cardputer's
             keys and taps into real Windows input (SendInput). --control needs --allow <ip>: no password here.
The client can pick the scale (1:1, 1:2, 1:4 of the real screen) with a ClientCutText "nucleo-scale:N"; the server
answers with a DesktopSize rectangle. Every second it prints what it sent; on disconnect a JSON summary goes to --stats.

    python -I tools/experiments/vnc-test-server.py --source screen --control --allow <cardputer ip>

Needs numpy and Pillow (ImageGrab, Windows/macOS).
"""
import argparse, ctypes, json, os, socket, struct, sys, threading, time
import numpy as np
from PIL import Image, ImageDraw, ImageFont

ENC_RAW, ENC_HEXTILE, ENC_DESKTOP_SIZE = 0, 5, -223
TILE = 16
WIN = sys.platform == "win32"
if WIN:
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)   # physical pixels for both the capture and the cursor
    except OSError:
        pass


def recv_exact(s, n):
    b = bytearray()
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise ConnectionError("client closed")
        b += c
    return bytes(b)


# ── Windows input (SendInput) ──────────────────────────────────────────────────────────────────────────────
class _KI(ctypes.Structure):
    _fields_ = [("wVk", ctypes.c_ushort), ("wScan", ctypes.c_ushort), ("dwFlags", ctypes.c_ulong),
                ("time", ctypes.c_ulong), ("dwExtraInfo", ctypes.c_size_t)]


class _MI(ctypes.Structure):
    _fields_ = [("dx", ctypes.c_long), ("dy", ctypes.c_long), ("mouseData", ctypes.c_ulong),
                ("dwFlags", ctypes.c_ulong), ("time", ctypes.c_ulong), ("dwExtraInfo", ctypes.c_size_t)]


class _U(ctypes.Union):
    _fields_ = [("ki", _KI), ("mi", _MI), ("pad", ctypes.c_byte * 32)]


class _IN(ctypes.Structure):
    _fields_ = [("type", ctypes.c_ulong), ("u", _U)]


KEYUP, UNICODE = 0x0002, 0x0004
VK = {0xFF08: 0x08, 0xFF09: 0x09, 0xFF0D: 0x0D, 0xFF1B: 0x1B, 0xFFFF: 0x2E, 0xFF50: 0x24, 0xFF57: 0x23,
      0xFF55: 0x21, 0xFF56: 0x22, 0xFF51: 0x25, 0xFF52: 0x26, 0xFF53: 0x27, 0xFF54: 0x28,
      0xFFE1: 0x10, 0xFFE2: 0x10, 0xFFE3: 0x11, 0xFFE4: 0x11, 0xFFE9: 0x12, 0xFFEA: 0x12, 0xFFEB: 0x5B, 0xFFEC: 0x5C}
VK.update({0xFFBE + i: 0x70 + i for i in range(12)})                       # F1..F12
VK.update({0xFF63: 0x2D, 0xFF61: 0x2C, 0xFF67: 0x5D})                     # Insert, Print, Menu
MODS = {0xFFE3, 0xFFE4, 0xFFE9, 0xFFEA, 0xFFEB, 0xFFEC}


class WinInput:
    def __init__(self):
        self.u = ctypes.windll.user32
        self.mods = set()
        self.mask = 0

    def _send(self, *items):
        arr = (_IN * len(items))(*items)
        self.u.SendInput(len(items), arr, ctypes.sizeof(_IN))

    def _key(self, vk=0, scan=0, flags=0):
        i = _IN(type=1)
        i.u.ki = _KI(vk, scan, flags, 0, 0)
        return i

    def key(self, down, sym):
        up = 0 if down else KEYUP
        if sym in MODS:
            (self.mods.add if down else self.mods.discard)(sym)
        if sym in VK:
            self._send(self._key(vk=VK[sym], flags=up))
            return
        cp = sym if sym < 0x100 else (sym - 0x1000000 if sym >= 0x1000000 else None)
        if cp is None:
            return
        if self.mods & {0xFFE3, 0xFFE4, 0xFFEB, 0xFFEC} or (self.mods & {0xFFE9, 0xFFEA} and chr(cp).isalnum()):
            vk = self.u.VkKeyScanW(ctypes.c_wchar(chr(cp))) & 0xFF   # a chord (Ctrl+C): the key itself
            if vk != 0xFF:
                self._send(self._key(vk=vk, flags=up))
            return
        if down:                                                     # plain text: any character, any layout
            self._send(self._key(scan=cp, flags=UNICODE), self._key(scan=cp, flags=UNICODE | KEYUP))

    def pointer(self, mask, sx, sy):
        self.u.SetCursorPos(int(sx), int(sy))
        flags = 0
        for bit, dn, upf in ((1, 0x0002, 0x0004), (2, 0x0020, 0x0040), (4, 0x0008, 0x0010)):
            if (mask & bit) and not (self.mask & bit):
                flags |= dn
            if not (mask & bit) and (self.mask & bit):
                flags |= upf
        if flags:
            i = _IN(type=0)
            i.u.mi = _MI(0, 0, 0, flags, 0, 0)
            self._send(i)
        for bit, delta in ((8, 120), (16, -120)):
            if (mask & bit) and not (self.mask & bit):
                i = _IN(type=0)
                i.u.mi = _MI(0, 0, ctypes.c_ulong(delta & 0xFFFFFFFF).value, 0x0800, 0, 0)
                self._send(i)
        self.mask = mask


class _PT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]


class Desktop:
    """What the server shows. region(x, y, w, h) -> RGB888 (h, w, 3) in framebuffer coordinates."""

    def __init__(self, source, size, control):
        self.source = source
        self.inp = WinInput() if (control and WIN) else None
        self.typed, self.clicks = "", []
        self.lock = threading.Lock()
        try:
            self.font = ImageFont.truetype("consola.ttf", 14)
            self.big = ImageFont.truetype("consola.ttf", 28)
        except OSError:
            self.font = self.big = ImageFont.load_default()
        self.t0 = time.time()
        if source == "screen":
            from PIL import ImageGrab
            self.grab = ImageGrab.grab
            self.sw, self.sh = self.grab().size
        else:
            self.sw, self.sh = size
        self.set_scale(1)

    def set_scale(self, n):
        self.scale = n if self.source == "screen" else 1
        self.w, self.h = self.sw // self.scale, self.sh // self.scale

    def key(self, down, sym):
        if self.inp:
            self.inp.key(down, sym)
        if not down:
            return
        with self.lock:
            if sym == 0xFF08:
                self.typed = self.typed[:-1]
            elif sym == 0xFF0D:
                self.typed += "\n"
            elif sym < 0x100 or sym >= 0x1000000:
                self.typed += chr(sym if sym < 0x100 else sym - 0x1000000)
            self.typed = self.typed[-600:]

    def pointer(self, mask, x, y):
        if self.inp:
            self.inp.pointer(mask, x * self.scale, y * self.scale)
        if mask:
            with self.lock:
                self.clicks = (self.clicks + [(x, y, mask, time.time())])[-12:]

    def region(self, x, y, w, h):
        if self.source == "screen":
            s = self.scale
            img = self.grab(bbox=(x * s, y * s, (x + w) * s, (y + h) * s)).convert("RGB")
            if s != 1:
                img = img.resize((w, h), Image.BOX)
            if WIN:                                   # the pointer is not in a GDI capture: draw an arrow
                p = _PT()
                ctypes.windll.user32.GetCursorPos(ctypes.byref(p))
                cx, cy = p.x / s - x, p.y / s - y
                if -12 < cx < w and -18 < cy < h:
                    dr = ImageDraw.Draw(img)
                    arrow = [(cx, cy), (cx, cy + 16), (cx + 4, cy + 12), (cx + 7, cy + 18), (cx + 9, cy + 17),
                             (cx + 6, cy + 11), (cx + 11, cy + 11)]
                    dr.polygon(arrow, fill=(255, 255, 255), outline=(0, 0, 0))
            return np.asarray(img)
        return self.synthetic()[y:y + h, x:x + w]

    def synthetic(self):
        t = time.time() - self.t0
        img = Image.new("RGB", (self.w, self.h), (32, 60, 92))
        d = ImageDraw.Draw(img)
        d.rectangle([0, self.h - 28, self.w, self.h], fill=(24, 24, 28))
        d.text((10, self.h - 22), "NucleoOS VNC test desktop", fill=(220, 220, 220), font=self.font)
        d.text((self.w - 110, self.h - 22), time.strftime("%H:%M:%S"), fill=(255, 255, 255), font=self.font)

        def window(x, y, w, h, title):
            d.rectangle([x, y, x + w, y + h], fill=(240, 240, 236), outline=(90, 90, 90))
            d.rectangle([x, y, x + w, y + 22], fill=(60, 110, 200))
            d.text((x + 8, y + 4), title, fill=(255, 255, 255), font=self.font)

        window(8, 8, 300, 220, "Notepad - type on the Cardputer")
        with self.lock:
            lines = self.typed.split("\n")[-10:]
            clicks = list(self.clicks)
        for i, ln in enumerate(lines):
            d.text((16, 36 + i * 18), ln[-36:], fill=(20, 20, 20), font=self.font)
        window(330, 8, 360, 300, "Log")
        n = int(t * 4)
        for i in range(15):
            k = n - 14 + i
            d.text((338, 36 + i * 18), f"{k:06d} event {k * 7919 % 100000:05d} ok", fill=(30, 80, 30), font=self.font)
        window(8, 250, 300, 120, "Clock")
        d.text((40, 290), time.strftime("%H:%M:%S"), fill=(10, 10, 10), font=self.big)
        x = int(340 + (np.sin(t * 1.5) * 0.5 + 0.5) * 300)
        d.rectangle([330, 330, 690, 470], fill=(250, 250, 250), outline=(90, 90, 90))
        d.rectangle([x, 360, x + 40, 440], fill=(220, 60, 60))
        yy, xx = np.mgrid[0:200, 0:300]
        grad = np.stack([(xx * 255 // 300), (yy * 255 // 200), ((xx + yy) * 255 // 500)], -1).astype(np.uint8)
        img.paste(Image.fromarray(grad), (720, 8))
        for (cx, cy, m, ct) in clicks:
            if time.time() - ct < 5:
                d.ellipse([cx - 6, cy - 6, cx + 6, cy + 6], outline=(255, 0, 255) if m & 4 else (255, 200, 0), width=3)
        return np.asarray(img)


class PixelFormat:
    def __init__(self, raw=None):
        if raw is None:   # server default: 32 bpp, little endian, RGB888
            raw = struct.pack(">BBBBHHHBBB3x", 32, 24, 0, 1, 255, 255, 255, 16, 8, 0)
        (self.bpp, self.depth, self.be, self.tc, self.rmax, self.gmax, self.bmax,
         self.rs, self.gs, self.bs) = struct.unpack(">BBBBHHHBBB3x", raw)
        self.raw = raw

    def convert(self, rgb):
        r = rgb[..., 0].astype(np.uint32) * self.rmax // 255
        g = rgb[..., 1].astype(np.uint32) * self.gmax // 255
        b = rgb[..., 2].astype(np.uint32) * self.bmax // 255
        v = (r << self.rs) | (g << self.gs) | (b << self.bs)
        if self.bpp == 8:
            return v.astype(np.uint8)
        if self.bpp == 16:
            return v.astype(">u2" if self.be else "<u2")
        return v.astype(">u4" if self.be else "<u4")


def vnc_response(password, challenge):
    """RFC 6143 7.2.2: DES-ECB of the challenge, key = the password's first 8 bytes, each byte bit-reversed."""
    import warnings
    warnings.filterwarnings("ignore")
    from cryptography.hazmat.primitives.ciphers import Cipher, modes
    try:
        from cryptography.hazmat.decrepit.ciphers.algorithms import TripleDES
    except ImportError:
        from cryptography.hazmat.primitives.ciphers.algorithms import TripleDES
    key = bytes(int(f"{b:08b}"[::-1], 2) for b in password.encode("latin-1")[:8].ljust(8, b"\0"))
    return Cipher(TripleDES(key), modes.ECB()).encryptor().update(challenge)


def hextile_tile(t, bpp_bytes):
    """One tile as Hextile bytes (always sends its own background: simple, a few bytes more)."""
    h, w = t.shape
    raw_len = 1 + w * h * bpp_bytes
    flat = t.reshape(-1)
    vals, counts = np.unique(flat, return_counts=True)
    bg = vals[np.argmax(counts)]
    bgb = np.array([bg], dtype=t.dtype).tobytes()
    if len(vals) == 1:
        return b"\x02" + bgb
    if len(vals) > 24:                    # photo-like: raw is smaller than the runs anyway
        return b"\x01" + t.tobytes()
    runs = []   # (colour, x, y, w) — horizontal runs of non-background pixels
    for y in range(h):
        row = t[y]
        x = 0
        while x < w:
            if row[x] == bg:
                x += 1
                continue
            c = row[x]
            x1 = x + 1
            while x1 < w and row[x1] == c:
                x1 += 1
            runs.append((c, x, y, x1 - x))
            x = x1
        if len(runs) > 255:
            return b"\x01" + t.tobytes()
    if len(vals) == 2:
        fg = runs[0][0]
        body = b"".join(bytes([(x << 4) | y, ((rw - 1) << 4)]) for (_, x, y, rw) in runs)
        out = b"\x0e" + bgb + np.array([fg], dtype=t.dtype).tobytes() + bytes([len(runs)]) + body
    else:
        body = b"".join(np.array([c], dtype=t.dtype).tobytes() + bytes([(x << 4) | y, ((rw - 1) << 4)])
                        for (c, x, y, rw) in runs)
        out = b"\x1a" + bgb + bytes([len(runs)]) + body
    return out if len(out) < raw_len else b"\x01" + t.tobytes()


class Client:
    def __init__(self, sock, desk, args):
        self.s, self.desk, self.args = sock, desk, args
        self.pf = PixelFormat()
        self.encs = [ENC_RAW]
        self.last = None            # converted framebuffer as the client has it (for incremental updates)
        self.pending = None         # (incremental, x, y, w, h)
        self.resized = False        # a DesktopSize rectangle is owed to the client
        self.cv = threading.Condition()
        self.stats = {"bytes": 0, "updates": 0, "rects": 0, "keys": 0, "clicks": 0, "requests": 0, "capture_ms": 0}
        self.sec = dict(self.stats)
        self.alive = True

    def send(self, b):
        self.s.sendall(b)
        self.stats["bytes"] += len(b)

    def handshake(self):
        self.s.sendall(b"RFB 003.008\n")
        print("client version", recv_exact(self.s, 12))
        if self.args.password:                     # VNC authentication: DES of a random challenge
            self.s.sendall(b"\x01\x02")
            if recv_exact(self.s, 1) != b"\x02":
                raise ConnectionError("client did not pick VNC authentication")
            ch = os.urandom(16)
            self.s.sendall(ch)
            if recv_exact(self.s, 16) != vnc_response(self.args.password, ch):
                why = b"wrong password"
                self.s.sendall(struct.pack(">II", 1, len(why)) + why)
                raise ConnectionError("wrong password")
            print("password ok")
        else:
            self.s.sendall(b"\x01\x01")           # one security type: None
            recv_exact(self.s, 1)
        self.s.sendall(struct.pack(">I", 0))       # SecurityResult OK
        recv_exact(self.s, 1)
        name = b"NucleoOS test desktop"
        self.s.sendall(struct.pack(">HH", self.desk.w, self.desk.h) + self.pf.raw + struct.pack(">I", len(name)) + name)

    def reader(self):
        try:
            while True:
                t = recv_exact(self.s, 1)[0]
                if t == 0:
                    self.pf = PixelFormat(recv_exact(self.s, 19)[3:])
                    self.last = None
                    print(f"SetPixelFormat bpp={self.pf.bpp} be={self.pf.be}")
                elif t == 2:
                    n = struct.unpack(">xH", recv_exact(self.s, 3))[0]
                    self.encs = list(struct.unpack(f">{n}i", recv_exact(self.s, 4 * n)))
                    print("SetEncodings", self.encs)
                elif t == 3:
                    inc, x, y, w, h = struct.unpack(">BHHHH", recv_exact(self.s, 9))
                    self.stats["requests"] += 1
                    with self.cv:
                        self.pending = (inc, x, y, w, h)
                        self.cv.notify()
                elif t == 4:
                    down, sym = struct.unpack(">B2xI", recv_exact(self.s, 7))
                    self.stats["keys"] += down
                    self.desk.key(down, sym)
                elif t == 5:
                    mask, x, y = struct.unpack(">BHH", recv_exact(self.s, 5))
                    if mask & 7:
                        self.stats["clicks"] += 1
                    self.desk.pointer(mask, x, y)
                elif t == 6:
                    n = struct.unpack(">3xI", recv_exact(self.s, 7))[0]
                    text = recv_exact(self.s, n).decode("latin-1")
                    if text.startswith("nucleo-scale:") and ENC_DESKTOP_SIZE in self.encs:
                        sc = int(text.split(":")[1])
                        if sc in (1, 2, 4):           # answered even when unchanged: the client learns the scale
                            with self.cv:
                                self.desk.set_scale(sc)
                                self.last = None
                                self.resized = True
                                self.cv.notify()
                            print(f"scale 1:{sc} -> {self.desk.w}x{self.desk.h}")
                else:
                    raise ConnectionError(f"unknown client message {t}")
        except (ConnectionError, OSError) as e:
            print("reader:", e)
        finally:
            self.alive = False
            with self.cv:
                self.cv.notify()

    def rects_for(self, cur, last, ox, oy, inc):
        """Changed tiles of the requested region, merged into runs per tile row (absolute coordinates)."""
        h, w = cur.shape
        if not inc or last is None:
            return [(ox, oy, w, h)]
        out = []
        for ty in range(0, h, TILE):
            th = min(TILE, h - ty)
            a, b = cur[ty:ty + th], last[ty:ty + th]
            diff = (a != b)
            cols = [diff[:, tx:tx + TILE].any() for tx in range(0, w, TILE)]
            run = None
            for i, ch in enumerate(cols + [False]):
                if ch and run is None:
                    run = i
                elif not ch and run is not None:
                    x0, x1 = run * TILE, min(i * TILE, w)
                    out.append((ox + x0, oy + ty, x1 - x0, th))
                    run = None
        return out

    def encode(self, cur, ox, oy, rx, ry, rw, rh):
        region = cur[ry - oy:ry - oy + rh, rx - ox:rx - ox + rw]
        if ENC_HEXTILE in self.encs:
            parts = [struct.pack(">HHHHi", rx, ry, rw, rh, ENC_HEXTILE)]
            bpp_bytes = self.pf.bpp // 8
            for ty in range(0, rh, TILE):
                for tx in range(0, rw, TILE):
                    parts.append(hextile_tile(region[ty:ty + TILE, tx:tx + TILE], bpp_bytes))
            return b"".join(parts)
        return struct.pack(">HHHHi", rx, ry, rw, rh, ENC_RAW) + np.ascontiguousarray(region).tobytes()

    def writer(self):
        period = 1.0 / self.args.fps
        while self.alive:
            with self.cv:
                while self.alive and self.pending is None and not self.resized:
                    self.cv.wait(0.5)
                if not self.alive:
                    break
                if self.resized:           # the size changed: tell the client, it asks for a full view next
                    self.resized = False
                    self.pending = None
                    self.send(struct.pack(">BxH", 0, 1) + struct.pack(">HHHHi", 0, 0, self.desk.w, self.desk.h, ENC_DESKTOP_SIZE))
                    self.stats["updates"] += 1
                    continue
                inc, x, y, w, h = self.pending
                self.pending = None
            W, H = self.desk.w, self.desk.h
            x, y = min(x, W - 1), min(y, H - 1)
            w, h = min(w, W - x), min(h, H - y)
            superseded = False
            while self.alive:
                t0 = time.time()
                cur = self.pf.convert(self.desk.region(x, y, w, h))
                self.stats["capture_ms"] += int((time.time() - t0) * 1000)
                last = None
                if self.last is not None and self.last.shape == (H, W):
                    last = self.last[y:y + h, x:x + w]
                rects = self.rects_for(cur, last, x, y, inc)
                if rects:
                    break
                if self.pending is not None or self.resized:   # newer request / new size: close this one empty
                    superseded = True
                    break
                time.sleep(period)          # incremental with nothing new: hold the request
            if not self.alive:
                break
            if superseded:
                self.send(struct.pack(">BxH", 0, 0))
                self.stats["updates"] += 1
                continue
            body = b"".join(self.encode(cur, x, y, *r) for r in rects)
            self.send(struct.pack(">BxH", 0, len(rects)) + body)
            if self.last is None or self.last.shape != (H, W):
                self.last = np.zeros((H, W), dtype=cur.dtype)
            self.last[y:y + h, x:x + w] = cur
            self.stats["updates"] += 1
            self.stats["rects"] += len(rects)
            time.sleep(period)

    def ticker(self):
        t0 = time.time()
        while self.alive:
            time.sleep(1)
            d = {k: self.stats[k] - self.sec[k] for k in self.stats}
            self.sec = dict(self.stats)
            print(f"[{time.time() - t0:6.1f}s] {d['bytes'] / 1024:7.1f} KB/s  {d['updates']:3d} upd/s  "
                  f"{d['rects']:3d} rect/s  capture {d['capture_ms']:4d} ms  keys {self.stats['keys']}  clicks {self.stats['clicks']}")

    def run(self):
        t0 = time.time()
        self.handshake()
        threading.Thread(target=self.reader, daemon=True).start()
        threading.Thread(target=self.ticker, daemon=True).start()
        self.writer()
        dur = time.time() - t0
        summary = dict(self.stats, seconds=round(dur, 1), kb_per_s=round(self.stats["bytes"] / 1024 / max(dur, 1e-6), 1),
                       updates_per_s=round(self.stats["updates"] / max(dur, 1e-6), 2), bpp=self.pf.bpp,
                       encodings=self.encs, source=self.args.source, size=f"{self.desk.w}x{self.desk.h}")
        print("summary", summary)
        if self.args.stats:
            with open(self.args.stats, "a", encoding="utf-8") as f:
                f.write(json.dumps(summary) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=5900)
    ap.add_argument("--size", default="1024x768", help="synthetic desktop size")
    ap.add_argument("--source", choices=["synthetic", "screen"], default="synthetic")
    ap.add_argument("--control", action="store_true", help="screen: turn the client's keys and taps into real input")
    ap.add_argument("--allow", default="", help="comma-separated client IPs; required with --control")
    ap.add_argument("--fps", type=float, default=20.0, help="cap on frames built per second")
    ap.add_argument("--stats", default="", help="append a JSON summary per session to this file")
    ap.add_argument("--once", action="store_true", help="exit after the first session (tests)")
    ap.add_argument("--password", default="", help="ask the client for this VNC password (8 characters are used)")
    args = ap.parse_args()
    allow = {a.strip() for a in args.allow.split(",") if a.strip()}
    if args.control and not allow:
        sys.exit("--control needs --allow <ip>: VNC passwords are weak, the address list is the real fence")
    w, h = (int(v) for v in args.size.lower().split("x"))
    desk = Desktop(args.source, (w, h), args.control)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):   # Windows: SO_REUSEADDR would let two servers share the port
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
    else:
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", args.port))
    srv.listen(1)
    print(f"VNC test server on :{args.port}, {desk.w}x{desk.h}, source {args.source}"
          f"{', CONTROL on' if args.control else ''}{', allow ' + ','.join(sorted(allow)) if allow else ''}")
    while True:
        s, addr = srv.accept()
        if allow and addr[0] not in allow:
            print("refused", addr[0])
            s.close()
            continue
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        print("client", addr)
        try:
            Client(s, desk, args).run()
        except (ConnectionError, OSError) as e:
            print("session ended:", e)
        except Exception:
            import traceback
            traceback.print_exc()
        finally:
            s.close()
        if args.once:
            break


if __name__ == "__main__":
    main()
