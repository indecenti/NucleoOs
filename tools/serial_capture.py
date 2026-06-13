import sys, time, serial
port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
secs = int(sys.argv[2]) if len(sys.argv) > 2 else 80
out  = sys.argv[3] if len(sys.argv) > 3 else r"G:\Nucleo\firmware\serial_cap.log"
# Open WITHOUT toggling DTR/RTS so we don't reset the device (USB-Serial-JTAG ignores them anyway).
s = serial.Serial()
s.port = port
s.baudrate = 115200
s.dtr = False
s.rts = False
s.timeout = 1
s.open()
t0 = time.time()
with open(out, "w", encoding="utf-8", errors="replace") as f:
    f.write("=== capture start %s ===\n" % time.strftime("%H:%M:%S"))
    f.flush()
    while time.time() - t0 < secs:
        try:
            line = s.readline()
        except Exception as e:
            f.write("[read error] %s\n" % e); f.flush(); break
        if line:
            f.write(line.decode("utf-8", "replace"))
            f.flush()
    f.write("\n=== capture end %s ===\n" % time.strftime("%H:%M:%S"))
s.close()
