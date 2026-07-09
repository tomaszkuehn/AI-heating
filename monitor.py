import serial, time, sys
port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0
s = serial.Serial(port, 115200, timeout=0.5)
# Reset ESP32 via auto-reset (RTS pulses EN)
s.dtr = False
s.rts = True          # EN low -> hold in reset
time.sleep(0.1)
s.rts = False         # EN high -> boot
s.dtr = False         # GPIO0 high -> normal boot (not download)
end = time.time() + secs
while time.time() < end:
    data = s.read(2048)
    if data:
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()
s.close()