import esptool, time, sys
port = sys.argv[1] if len(sys.argv) > 1 else 'COM3'
dur  = float(sys.argv[2]) if len(sys.argv) > 2 else 12.0
esp = esptool.detect_chip(port)     # connects (download mode), USB-JTAG aware
esp.hard_reset()                    # proper reset -> RUN the app (not download)
s = esp._port
s.timeout = 0.1
buf = bytearray(); end = time.time() + dur
while time.time() < end:
    d = s.read(4096)
    if d: buf += d
try:
    s.close()
except Exception:
    pass
t = bytes(buf).decode('utf-8', 'replace')
print("BYTES", len(buf))
print("----LOG----")
print(t[-4800:])
