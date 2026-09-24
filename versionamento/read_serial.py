import sys
import time
import serial
import serial.tools.list_ports as lp

port = sys.argv[1] if len(sys.argv) > 1 else None
duration = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0

if not port:
    ports = [p.device for p in lp.comports()]
    if not ports:
        print("Nenhuma porta serial encontrada!")
        sys.exit(1)
    port = ports[0]

print(f"Lendo {port} a 115200 (duracao {duration}s)...")
try:
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.5
    s.setDTR(False)
    s.setRTS(False)
    s.open()
except Exception as e:
    print(f"Erro ao abrir {port}: {e}")
    sys.exit(1)

t0 = time.time()
while time.time() - t0 < duration:
    raw = s.readline()
    if raw:
        sys.stdout.buffer.write(raw)
        sys.stdout.buffer.flush()

s.close()


