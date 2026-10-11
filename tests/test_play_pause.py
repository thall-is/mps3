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

    # Check current state with 'status'
    safe_print("CMD", "Sending 'status'")
    ser.write(b"status\n")
    start = time.time()
    while time.time() - start < 1.5:
        line = ser.readline().decode('utf-8', errors='replace')
        if line:
            safe_print("STATUS", line.strip())

    # Send 'exit' to ensure we exit WiFi and enter Player mode
    safe_print("CMD", "Sending 'exit' to enter Player mode")
    ser.write(b"exit\n")
    start = time.time()
    while time.time() - start < 5.0:
        line = ser.readline().decode('utf-8', errors='replace')
        if line:
            safe_print("EXIT_WIFI", line.strip())

    # Wait for track loading and playback
    safe_print("INFO", "Waiting for playback to begin...")
    start = time.time()
    while time.time() - start < 5.0:
        line = ser.readline().decode('utf-8', errors='replace')
        if line:
            safe_print("PLAYING", line.strip())

    # Send 'p' to PAUSE
    safe_print("CMD", "Sending 'p' (PAUSE)")
    ser.write(b"p\n")
    start = time.time()
    while time.time() - start < 4.0:
        line = ser.readline().decode('utf-8', errors='replace')
        if line:
            safe_print("PAUSE", line.strip())

    # Send 'p' to RESUME (UNPAUSE)
    safe_print("CMD", "Sending 'p' (RESUME / UNPAUSE)")
    ser.write(b"p\n")
    start = time.time()
    while time.time() - start < 7.0:
        line = ser.readline().decode('utf-8', errors='replace')
        if line:
            safe_print("RESUME", line.strip())

    # Another cycle: pause and unpause again
    safe_print("CMD", "Second cycle: sending 'p' (PAUSE)")
    ser.write(b"p\n")
    start = time.time()
    while time.time() - start < 3.0:
        line = ser.readline().decode('utf-8', errors='replace')
        if line:
            safe_print("PAUSE2", line.strip())

    safe_print("CMD", "Second cycle: sending 'p' (RESUME)")
    ser.write(b"p\n")
    start = time.time()
    while time.time() - start < 6.0:
        line = ser.readline().decode('utf-8', errors='replace')
        if line:
            safe_print("RESUME2", line.strip())

    ser.close()
    safe_print("DONE", "Test completed.")

if __name__ == '__main__':
    main()

