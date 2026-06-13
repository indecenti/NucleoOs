#!/usr/bin/env python3
"""Capture the Cardputer's boot log over USB-Serial/JTAG.
Opens the port first, resets the chip to RUN, then reads for a few seconds.
Usage: python tools/serial_boot.py [COMx] [seconds]
"""
import sys, time, serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 12.0

s = serial.Serial(port, 115200, timeout=0.2)
# USB-Serial/JTAG: keep the boot pin released (RTS low), pulse the reset line (DTR).
s.setRTS(False)
s.setDTR(False)
time.sleep(0.2)
s.setDTR(True)        # assert reset
time.sleep(0.2)
s.setDTR(False)       # release -> boot to RUN
buf = b""
t = time.time()
while time.time() - t < secs:
    data = s.read(4096)
    if data:
        buf += data
s.close()
sys.stdout.write(buf.decode("utf-8", "replace"))
