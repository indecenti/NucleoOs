import serial, time, sys, datetime
OUT = r"G:\Nucleo\_scratch\com3_log.txt"
DUR = int(sys.argv[1]) if len(sys.argv) > 1 else 180
s = serial.Serial()
s.port = "COM3"; s.baudrate = 115200
s.dtr = False; s.rts = False          # no reset / no download on open
s.timeout = 0.2
s.open()
end = time.time() + DUR
with open(OUT, "w", encoding="utf-8", errors="replace") as f:
    f.write("# capture start %s\n" % datetime.datetime.now().isoformat()); f.flush()
    line = bytearray()
    while time.time() < end:
        n = s.in_waiting
        if n:
            chunk = s.read(n)
            for b in chunk:
                if b == 0x0A:
                    f.write(line.decode("utf-8", "replace") + "\n"); f.flush()
                    line = bytearray()
                elif b != 0x0D:
                    line.append(b)
        else:
            time.sleep(0.05)
    if line:
        f.write(line.decode("utf-8", "replace") + "\n")
    f.write("# capture end %s\n" % datetime.datetime.now().isoformat())
s.close()
