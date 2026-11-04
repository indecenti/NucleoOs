#!/usr/bin/env python3
"""A deliberately broken RFB server: one malformed answer per connection, to check that the Cardputer VNC client
(app_vnc.cpp) ends the session with an error and never writes out of bounds, hangs or crashes.

The bench card sets /sd/data/vnc/vnc.json {"host": "<this PC>", "auto": true, "retries": N} so the app runs the
sessions back to back and writes /sd/data/vnc/sessions.log; a short log (or a reboot in the middle) is a failure.

    python -I tools/experiments/vnc-hostile-server.py --allow <cardputer ip>
"""
import argparse, os, socket, struct, time

W, H = 1024, 768


def pf32():
    return struct.pack(">BBBBHHHBBB3x", 32, 24, 0, 1, 255, 255, 255, 16, 8, 0)


def hello(s, w=W, h=H, name=b"hostile", name_len=None, version=b"RFB 003.008\n", sec=b"\x01\x01"):
    s.sendall(version)
    s.recv(12)
    s.sendall(sec)
    s.recv(1)
    s.sendall(struct.pack(">I", 0))
    s.recv(1)
    s.sendall(struct.pack(">HH", w, h) + pf32() + struct.pack(">I", len(name) if name_len is None else name_len) + name)
    time.sleep(0.3)                       # let SetPixelFormat / SetEncodings / the first request arrive


def update(rects):
    return struct.pack(">BxH", 0, len(rects)) + b"".join(rects)


def rect(x, y, w, h, enc, body=b""):
    return struct.pack(">HHHHi", x, y, w, h, enc) + body


CASES = []


def case(fn):
    CASES.append(fn)
    return fn


@case
def baseline(s):
    """a correct small update, then the server closes"""
    hello(s)
    s.sendall(update([rect(0, 0, 16, 16, 5, b"\x02\x00\x1f")]))
    time.sleep(1)


@case
def rect_outside(s):
    """a rectangle past the right edge of the desktop"""
    hello(s)
    s.sendall(update([rect(1000, 0, 500, 16, 0, b"\x00" * (500 * 16 * 2))]))


@case
def rect_y_overflow(s):
    """y + h wraps a 16-bit field"""
    hello(s)
    s.sendall(update([rect(0, 65000, 16, 1000, 0, b"\x00" * 64)]))


@case
def raw_short(s):
    """a big raw rectangle announced, a few bytes sent, then silence (read deadline)"""
    hello(s)
    s.sendall(update([rect(0, 0, 320, 240, 0, b"\x00" * 1000)]))
    time.sleep(12)


@case
def trickle(s):
    """an update that arrives one byte every 5 s (the whole-update deadline, 20 s)"""
    hello(s)
    data = update([rect(0, 0, 320, 240, 0, b"\x00" * (320 * 240 * 2))])
    for b in data[:10]:
        s.sendall(bytes([b]))
        time.sleep(5)


@case
def hextile_wild_subrects(s):
    """subrectangles that poke out of their tile (must be clipped), then an unknown message"""
    hello(s)
    tile = b"\x1a" + b"\x00\x1f" + bytes([3]) + b"\xf8\x00" + bytes([0xFF, 0xFF]) + b"\x07\xe0" + bytes([0xF0, 0x0F]) + b"\xff\xff" + bytes([0x0F, 0xF0])
    s.sendall(update([rect(0, 0, 16, 16, 5, tile)]))
    s.sendall(bytes([200]) + b"\x00" * 32)
    time.sleep(1)


@case
def hextile_edge_tile(s):
    """a 5x3 rectangle at the very corner, tiles smaller than 16, subrects past them"""
    hello(s)
    tile = b"\x0e" + b"\x00\x1f" + b"\xff\xff" + bytes([2]) + bytes([0x44, 0xFF]) + bytes([0x00, 0xFF])
    s.sendall(update([rect(W - 5, H - 3, 5, 3, 5, tile)]))
    time.sleep(1)


@case
def desktop_size_zero(s):
    """DesktopSize to 0x0"""
    hello(s)
    s.sendall(update([rect(0, 0, 0, 0, -223)]))


@case
def desktop_size_huge(s):
    """DesktopSize to 65535x65535"""
    hello(s)
    s.sendall(update([rect(0, 0, 65535, 65535, -223)]))


@case
def cut_text_huge(s):
    """server cut text of 4 GB"""
    hello(s)
    s.sendall(struct.pack(">B3xI", 3, 0xFFFFFFFF) + b"A" * 100)


@case
def name_huge(s):
    """a desktop name of 2 GB in ServerInit"""
    hello(s, name_len=0x7FFFFFFF)


@case
def size_zero(s):
    """a 0x0 desktop in ServerInit"""
    hello(s, w=0, h=0)


@case
def size_huge(s):
    """a 65535x65535 desktop in ServerInit"""
    hello(s, w=65535, h=65535)


@case
def garbage(s):
    """10 KB of random bytes after the handshake"""
    hello(s)
    s.sendall(os.urandom(10240))
    time.sleep(1)


@case
def many_empty_rects(s):
    """65535 empty rectangles in one update"""
    hello(s)
    s.sendall(update([rect(0, 0, 0, 0, 0)] * 65535))
    time.sleep(2)


@case
def colour_map(s):
    """a colour map with 65535 entries announced, nothing behind it"""
    hello(s)
    s.sendall(struct.pack(">BxHH", 1, 0, 65535))


@case
def not_vnc(s):
    """an HTTP server answering"""
    s.sendall(b"HTTP/1.1 400 Bad Request\r\n\r\n")


@case
def refused_huge_reason(s):
    """3.8 security list of 0 types with a 4 GB reason"""
    s.sendall(b"RFB 003.008\n")
    s.recv(12)
    s.sendall(b"\x00" + struct.pack(">I", 0xFFFFFFFF))


@case
def unknown_security(s):
    """only security type 18 (TLS) offered"""
    s.sendall(b"RFB 003.008\n")
    s.recv(12)
    s.sendall(b"\x01\x12")
    time.sleep(1)


@case
def unknown_encoding(s):
    """a rectangle in an encoding that was not asked for (ZRLE)"""
    hello(s)
    s.sendall(update([rect(0, 0, 16, 16, 16, b"\x00" * 64)]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=5900)
    ap.add_argument("--allow", required=True, help="the Cardputer's IP")
    args = ap.parse_args()
    srv = socket.socket()
    if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
    srv.bind(("0.0.0.0", args.port))
    srv.listen(1)
    print(f"hostile VNC server on :{args.port}, {len(CASES)} cases (vnc.json retries = {len(CASES) - 1})")
    for i, fn in enumerate(CASES, 1):
        while True:
            s, addr = srv.accept()
            if addr[0] == args.allow:
                break
            s.close()
        s.settimeout(30)
        print(f"case {i:2d} {fn.__name__}: {fn.__doc__}", flush=True)
        t0 = time.time()
        try:
            fn(s)
            s.settimeout(25)
            while s.recv(4096):            # until the client gives up and closes
                pass
        except (OSError, ConnectionError) as e:
            print("   ", type(e).__name__, e)
        print(f"    client left after {time.time() - t0:.1f}s", flush=True)
        s.close()
    print("all cases served")


if __name__ == "__main__":
    main()
