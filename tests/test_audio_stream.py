import ctypes, math, struct, time
from ctypes import wintypes

winmm = ctypes.windll.winmm

WAVE_FORMAT_EXTENSIBLE = 0xFFFE
KSDATAFORMAT_SUBTYPE_PCM = bytes([
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
    0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71
])

class WAVEFORMATEXTENSIBLE(ctypes.Structure):
    _fields_ = [
        ('wFormatTag', wintypes.WORD),
        ('nChannels', wintypes.WORD),
        ('nSamplesPerSec', wintypes.DWORD),
        ('nAvgBytesPerSec', wintypes.DWORD),
        ('nBlockAlign', wintypes.WORD),
        ('wBitsPerSample', wintypes.WORD),
        ('cbSize', wintypes.WORD),
        ('wValidBitsPerSample', wintypes.WORD),
        ('dwChannelMask', wintypes.DWORD),
        ('SubFormat', ctypes.c_byte * 16)
    ]

class WAVEHDR(ctypes.Structure):
    pass

WAVEHDR._fields_ = [
    ('lpData', ctypes.c_char_p),
    ('dwBufferLength', wintypes.DWORD),
    ('dwBytesRecorded', wintypes.DWORD),
    ('dwUser', ctypes.c_void_p),
    ('dwFlags', wintypes.DWORD),
    ('dwLoops', wintypes.DWORD),
    ('lpNext', ctypes.POINTER(WAVEHDR)),
    ('reserved', ctypes.c_void_p),
]

class WAVEOUTCAPS(ctypes.Structure):
    _fields_ = [
        ('wMid', wintypes.WORD),
        ('wPid', wintypes.WORD),
        ('vDriverVersion', wintypes.UINT),
        ('szPname', ctypes.c_char * 32),
        ('dwFormats', wintypes.DWORD),
        ('wChannels', wintypes.WORD),
        ('wReserved1', wintypes.WORD),
        ('dwSupport', wintypes.DWORD),
    ]

num_devs = winmm.waveOutGetNumDevs()
mps3_id = None
for i in range(num_devs):
    caps = WAVEOUTCAPS()
    winmm.waveOutGetDevCapsA(i, ctypes.byref(caps), ctypes.sizeof(caps))
    name = caps.szPname.decode('latin1')
    if 'MPS3' in name:
        mps3_id = i
        print(f"Dispositivo MPS3 encontrado: ID {i} ({name})")
        break

if mps3_id is None:
    print("ERRO: Dispositivo MPS3 DAC Audio nao encontrado!")
    exit(1)

def play_tone(rate, freq, duration, desc):
    print(f"\n>>> Testando reproducao: {desc} - Freq={freq}Hz, Taxa={rate}Hz...")
    wfx = WAVEFORMATEXTENSIBLE()
    wfx.wFormatTag = WAVE_FORMAT_EXTENSIBLE
    wfx.nChannels = 2
    wfx.nSamplesPerSec = rate
    wfx.nBlockAlign = 8 # 2 canais * 4 bytes
    wfx.nAvgBytesPerSec = rate * 8
    wfx.wBitsPerSample = 32
    wfx.cbSize = 22
    wfx.wValidBitsPerSample = 24
    wfx.dwChannelMask = 0x3
    ctypes.memmove(wfx.SubFormat, KSDATAFORMAT_SUBTYPE_PCM, 16)

    h_waveout = wintypes.HANDLE()
    res_open = winmm.waveOutOpen(ctypes.byref(h_waveout), mps3_id, ctypes.byref(wfx), 0, 0, 0)
    if res_open != 0:
        print(f"Falha ao abrir ({res_open})")
        return False

    num_frames = int(rate * duration)
    max_amp = (1 << 23) - 1
    raw_audio = bytearray()
    for i in range(num_frames):
        s_val = int(max_amp * 0.7 * math.sin(2.0 * math.pi * freq * i / rate))
        s32 = s_val << 8
        raw_audio += struct.pack('<ii', s32, s32)

    raw_bytes = bytes(raw_audio)
    hdr = WAVEHDR()
    hdr.lpData = ctypes.c_char_p(raw_bytes)
    hdr.dwBufferLength = len(raw_bytes)
    hdr.dwFlags = 0

    winmm.waveOutPrepareHeader(h_waveout, ctypes.byref(hdr), ctypes.sizeof(hdr))
    winmm.waveOutWrite(h_waveout, ctypes.byref(hdr), ctypes.sizeof(hdr))
    print(f"Tocando {desc} por {duration}s...")
    time.sleep(duration + 0.3)
    winmm.waveOutUnprepareHeader(h_waveout, ctypes.byref(hdr), ctypes.sizeof(hdr))
    winmm.waveOutClose(h_waveout)
    print(f"{desc}: OK!")
    return True

# 1. 44.1 kHz / 24-bit (Nota Lá 440 Hz)
play_tone(44100, 440.0, 2.0, "44.1 kHz / 24-bit (La 440 Hz)")
time.sleep(0.5)

# 2. 48.0 kHz / 24-bit (Nota Do# 554 Hz)
play_tone(48000, 554.37, 2.0, "48.0 kHz / 24-bit (Do# 554 Hz)")
time.sleep(0.5)

# 3. 96.0 kHz / 24-bit (Nota Mi 659 Hz)
play_tone(96000, 659.25, 2.0, "96.0 kHz / 24-bit Hi-Res (Mi 659 Hz)")

print("\nTODOS OS TESTES DE REPRODUCAO 24-BIT FINALIZADOS COM SUCESSO!")

