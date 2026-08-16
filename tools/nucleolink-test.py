#!/usr/bin/env python3
# NucleoLink — Phase 1 transport test.
# Finds the device's USB-web CDC serial port and exercises the ping/status handshake.
# Run it AFTER the device is in "USB Web" mode (USB app -> press W on the Cardputer).
#
# Needs pyserial. On this machine use the ESP-IDF python that already has it:
#   /home/index/.espressif/python_env/idf5.3_py3.11_env/bin/python tools/nucleolink-test.py
import sys, time
try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial missing — run with the ESP-IDF python (…/idf5.3_py3.11_env/bin/python)")

def find_port():
    # Prefer the TinyUSB CDC our firmware exposes: product string "NucleoOS Web Link".
    cands = list(list_ports.comports())
    for p in cands:
        blob = " ".join(str(x) for x in (p.description, p.product, p.manufacturer, p.interface) if x)
        if "NucleoOS" in blob or "Web Link" in blob:
            return p.device
    # Fallback: any ACM/USB serial (the JTAG console also shows up; the user picks if ambiguous).
    acm = [p.device for p in cands if "ACM" in p.device or "usbmodem" in p.device or "USB" in (p.description or "")]
    return acm[0] if acm else None

def main():
    port = sys.argv[1] if len(sys.argv) > 1 else find_port()
    if not port:
        sys.exit("no serial port found — is the device in USB Web mode? (USB app -> W)")
    print(f"port: {port}")
    with serial.Serial(port, 115200, timeout=2) as s:
        time.sleep(0.3); s.reset_input_buffer()
        for req in ("PING", "GET /api/status", "NONSENSE"):
            s.write((req + "\n").encode())
            s.flush()
            line = s.readline().decode("utf-8", "replace").strip()
            print(f"  -> {req!r:22} <- {line!r}")
            ok = (req == "PING" and line.startswith("PONG")) or \
                 (req.startswith("GET") and '"os":"NucleoOS"' in line) or \
                 (req == "NONSENSE" and "unknown" in line)
            print("     " + ("✓ ok" if ok else "✗ unexpected"))
    print("done.")

if __name__ == "__main__":
    main()
