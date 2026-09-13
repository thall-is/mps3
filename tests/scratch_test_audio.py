import math
import struct
import time
import ctypes
from ctypes import wintypes

# WinMM definitions
winmm = ctypes.windll.winmm

class WAVEFORMATEX(ctypes.Structure):
    _fields_ = [
        ('wFormatTag', wintypes.WORD),
        ('nChannels', wintypes.WORD),
        ('nSamplesPerSec', wintypes.DWORD),
        ('nAvgBytesPerSec', wintypes.DWORD),
        ('nBlockAlign', wintypes.WORD),
        ('wBitsPerSample', wintypes.WORD),
        ('cbSize', wintypes.WORD),
    ]

class WAVEHDR(ctypes.Structure):
    pass

WAVEHDR._fields_ = [
    ('lpData', ctypes.POINTER(ctypes.c_char)),
    ('dwBufferLength', wintypes.DWORD),
    ('dwBytesRecorded', wintypes.DWORD),
    ('dwUser', ctypes.c_void_p),
    ('dwFlags', wintypes.DWORD),
    ('dwLoops', wintypes.DWORD),
    ('lpNext', ctypes.POINTER(WAVEHDR)),
    ('reserved', ctypes.c_void_p),
]

# Find MPS3 device ID
class WAVEOUTCAPS(ctypes.Structure):
    _fields_ = [
        ('wMid', wintypes.WORD),
        ('wPid', wintypes.WORD),
        ('vDriverVersion', wintypes.UINT),
        ('szPname', wintypes.WCHAR * 32),
        ('dwFormats', wintypes.DWORD),
        ('wChannels', wintypes.WORD),
        ('wReserved1', wintypes.WORD),
        ('dwSupport', wintypes.DWORD),
    ]

num_devs = winmm.waveOutGetNumDevs()
mps3_id = None
for i in range(num_devs):
    caps = WAVEOUTCAPS()
    if winmm.waveOutGetDevCapsW(i, ctypes.byref(caps), ctypes.sizeof(caps)) == 0:
        print(f"Device {i}: {caps.szPname}")
        if "MPS3" in caps.szPname:
            mps3_id = i

if mps3_id is None:
    print("MPS3 Audio device not found!")
    exit(1)

print(f"Testing playback to MPS3 Audio on device {mps3_id}...")

wfx = WAVEFORMATEX()
wfx.wFormatTag = 1 # WAVE_FORMAT_PCM
wfx.nChannels = 2
wfx.nSamplesPerSec = 48000
wfx.wBitsPerSample = 16
wfx.nBlockAlign = 4
wfx.nAvgBytesPerSec = 48000 * 4
wfx.cbSize = 0

h_waveout = wintypes.HANDLE()
res = winmm.waveOutOpen(ctypes.byref(h_waveout), mps3_id, ctypes.byref(wfx), 0, 0, 0)
if res != 0:
    print(f"waveOutOpen failed: {res}")
    exit(1)

# Generate 2 seconds of 440Hz sine wave
duration = 2.0
sample_rate = 48000
freq = 440.0
num_samples = int(duration * sample_rate)
raw_bytes = bytearray()
for i in range(num_samples):
    val = int(16384 * math.sin(2.0 * math.pi * freq * (i / sample_rate)))
    raw_bytes.extend(struct.pack('<hh', val, val))

buf = ctypes.create_string_buffer(bytes(raw_bytes))
hdr = WAVEHDR()
hdr.lpData = ctypes.cast(buf, ctypes.POINTER(ctypes.c_char))
hdr.dwBufferLength = len(raw_bytes)
hdr.dwFlags = 0

winmm.waveOutPrepareHeader(h_waveout, ctypes.byref(hdr), ctypes.sizeof(hdr))
res = winmm.waveOutWrite(h_waveout, ctypes.byref(hdr), ctypes.sizeof(hdr))
print(f"waveOutWrite returned: {res}")

time.sleep(2.5)
winmm.waveOutUnprepareHeader(h_waveout, ctypes.byref(hdr), ctypes.sizeof(hdr))
winmm.waveOutClose(h_waveout)
print("Playback test finished.")

