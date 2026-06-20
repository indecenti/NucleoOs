import sys, time
try:
    import serial  # pyserial ships in the ESP-IDF python env (idf_monitor uses it)
except Exception as e:
    print("NO_PYSERIAL", e); sys.exit(3)

port = sys.argv[1] if len(sys.argv) > 1 else 'COM3'
dur  = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
out  = sys.argv[3] if len(sys.argv) > 3 else 'serial_cap.log'

try:
    s = serial.Serial(port, 115200, timeout=0.2)
except Exception as e:
    print("OPEN_FAIL", e); sys.exit(2)

end = time.time() + dur
buf = bytearray()
with open(out, 'wb') as f:
    while time.time() < end:
        data = s.read(4096)
        if data:
            f.write(data); f.flush()
            buf += data
s.close()

text = bytes(buf).decode('utf-8', 'replace')
# Reset/panic signatures. A clean run that does NOT reboot shows none of these AFTER the first boot.
sigs = ['Guru Meditation', 'abort() was called', 'Backtrace:', 'CORRUPT HEAP',
        'assert failed', 'Brownout', 'StoreProhibited', 'LoadProhibited',
        'Stack canary', 'register dump', 'rst:0x']
hits = {}
for g in sigs:
    c = text.count(g)
    if c: hits[g] = c
boots = text.count('ESP-ROM:') + text.count('cpu_start:') + text.count('Boot SW reset')
print("==BYTES==", len(buf))
print("==BOOT_MARKERS==", boots)
print("==CRASH_SIGS==", hits if hits else "none")
