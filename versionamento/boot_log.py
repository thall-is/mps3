import sys
import time
import serial
import serial.tools.list_ports as lp

ports = [p.device for p in lp.comports()]
if not ports:
    print("Nenhuma porta serial encontrada!")
    sys.exit(1)

port = ports[0]
print(f"Abrindo {port} e resetando ESP32...")
s = serial.Serial(port, 115200, timeout=0.5)

# Reset via DTR/RTS
s.setDTR(False)
s.setRTS(True)
time.sleep(0.1)
s.setRTS(False)
time.sleep(0.1)

t0 = time.time()
while time.time() - t0 < 8:
    raw = s.readline()
    if raw:
        sys.stdout.buffer.write(raw)
        sys.stdout.buffer.flush()

s.close()

