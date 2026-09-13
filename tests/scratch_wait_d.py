import os
import time

print("Waiting for drive D: to become ready (select USB Storage on the OLED)...")
for i in range(15):
    if os.path.exists("D:\\"):
        try:
            files = os.listdir("D:\\")
            print(f"SUCCESS! Drive D: is READY! Found {len(files)} items: {files[:5]}")
            break
        except Exception as e:
            pass
    time.sleep(1)
else:
    print("Drive D: not mounted yet (device still in menu or DAC mode).")

