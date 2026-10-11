import serial
import time
import sys

def safe_print(prefix, text):
    try:
        sys.stdout.buffer.write(f"[{prefix}] {text}\n".encode('utf-8', errors='replace'))
        sys.stdout.buffer.flush()
    except Exception:
        pass

def main():
    safe_print("INIT", "Opening COM5...")
    try:
        ser = serial.Serial('COM5', 115200, timeout=0.2)
    except Exception as e:
        safe_print("ERR", f"Failed to open COM5: {e}")
        return

    # Check playing state for 4 seconds
    safe_print("STATE", "Verifying current playback...")
    start = time.time()
    while time.time() - start < 4.0:
        line = ser.readline().decode('utf-8', errors='replace')
        if line:
            safe_print("PLAY", line.strip())

    # Send Pause
    safe_print("ACTION", "Sending 'p' to PAUSE")
    ser.write(b"p\n")
    start = time.time()
    while time.time() - start < 3.0:
        line = ser.readline().decode('utf-8', errors='replace')
        if line:
            safe_print("PAUSED", line.strip())

    # Send Play / Unpause
    safe_print("ACTION", "Sending 'p' to UNPAUSE / RESUME")
    ser.write(b"p\n")
    start = time.time()
    while time.time() - start < 8.0:
        line = ser.readline().decode('utf-8', errors='replace')
        if line:
            safe_print("RESUMED", line.strip())

    ser.close()
    safe_print("DONE", "Test complete!")

if __name__ == '__main__':
    main()

