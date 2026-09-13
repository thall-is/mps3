import ctypes
from ctypes import wintypes
import time
import os
import sys

# Windows API Constants
GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
FILE_SHARE_READ = 0x00000001
FILE_SHARE_WRITE = 0x00000002
CREATE_ALWAYS = 2
OPEN_EXISTING = 3
FILE_ATTRIBUTE_NORMAL = 0x80
FILE_FLAG_NO_BUFFERING = 0x20000000
FILE_FLAG_WRITE_THROUGH = 0x80000000
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)

CreateFileW = kernel32.CreateFileW
CreateFileW.argtypes = [
    wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
    wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE
]
CreateFileW.restype = wintypes.HANDLE

WriteFile = kernel32.WriteFile
WriteFile.argtypes = [
    wintypes.HANDLE, wintypes.LPCVOID, wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID
]
WriteFile.restype = wintypes.BOOL

ReadFile = kernel32.ReadFile
ReadFile.argtypes = [
    wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID
]
ReadFile.restype = wintypes.BOOL

CloseHandle = kernel32.CloseHandle
CloseHandle.argtypes = [wintypes.HANDLE]
CloseHandle.restype = wintypes.BOOL

def run_benchmark(drive="D", size_mb=2):
    filepath = f"{drive}:\\perf_test.tmp"
    chunk_size = 64 * 1024 # 64 KB alinhado a setor (512B)
    total_bytes = size_mb * 1024 * 1024
    num_chunks = total_bytes // chunk_size

    print(f"==================================================")
    print(f"  BENCHMARK HARDWARE REAL USB MSC (SEM CACHE)")
    print(f"  Unidade: {drive}:\\ | Tamanho: {size_mb} MB ({num_chunks} blocos de 64 KB)")
    print(f"==================================================")
    sys.stdout.flush()

    # Buffer alinhado
    buffer = ctypes.create_string_buffer(b"A" * chunk_size)
    bytes_written = wintypes.DWORD()
    bytes_read = wintypes.DWORD()

    # --- 1. ESCRITA DIRETA ---
    print("\n>>> [1/2] TESTANDO GRAVACAO NO CARTAO SD VIA USB...")
    sys.stdout.flush()

    flags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH
    handle = CreateFileW(filepath, GENERIC_WRITE, FILE_SHARE_READ, None, CREATE_ALWAYS, flags, None)
    if handle == INVALID_HANDLE_VALUE:
        err = ctypes.get_last_error()
        print(f"Erro ao abrir arquivo para escrita: Windows error {err}")
        return

    t0 = time.perf_counter()
    last_t = t0
    total_w = 0

    try:
        for i in range(num_chunks):
            success = WriteFile(handle, buffer, chunk_size, ctypes.byref(bytes_written), None)
            if not success:
                print(f"Erro na gravacao do bloco {i}: {ctypes.get_last_error()}")
                break
            total_w += bytes_written.value
            t_now = time.perf_counter()
            if t_now - last_t >= 0.5 or i == num_chunks - 1:
                rate = (total_w / 1024) / (t_now - t0)
                pct = (total_w / total_bytes) * 100
                print(f"  [ESCRITA] {pct:5.1f}% | {total_w // 1024} KB | Taxa: {rate:.1f} KB/s ({rate/1024:.2f} MB/s)")
                sys.stdout.flush()
                last_t = t_now
    finally:
        CloseHandle(handle)

    t_end_w = time.perf_counter()
    dt_w = t_end_w - t0
    rate_w = (total_w / 1024) / dt_w if dt_w > 0 else 0
    print(f"--> ESCRITA FINALIZADA: {total_w/1024:.0f} KB em {dt_w:.2f}s = {rate_w:.1f} KB/s ({rate_w/1024:.2f} MB/s)")
    sys.stdout.flush()

    time.sleep(0.5)

    # --- 2. LEITURA DIRETA ---
    print("\n>>> [2/2] TESTANDO LEITURA DO CARTAO SD VIA USB...")
    sys.stdout.flush()

    flags_r = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING
    handle_r = CreateFileW(filepath, GENERIC_READ, FILE_SHARE_READ, None, OPEN_EXISTING, flags_r, None)
    if handle_r == INVALID_HANDLE_VALUE:
        err = ctypes.get_last_error()
        print(f"Erro ao abrir arquivo para leitura: Windows error {err}")
        return

    t0_r = time.perf_counter()
    last_t_r = t0_r
    total_r = 0

    try:
        for i in range(num_chunks):
            success = ReadFile(handle_r, buffer, chunk_size, ctypes.byref(bytes_read), None)
            if not success or bytes_read.value == 0:
                break
            total_r += bytes_read.value
            t_now = time.perf_counter()
            if t_now - last_t_r >= 0.5 or i == num_chunks - 1:
                rate = (total_r / 1024) / (t_now - t0_r)
                pct = (total_r / total_bytes) * 100
                print(f"  [LEITURA] {pct:5.1f}% | {total_r // 1024} KB | Taxa: {rate:.1f} KB/s ({rate/1024:.2f} MB/s)")
                sys.stdout.flush()
                last_t_r = t_now
    finally:
        CloseHandle(handle_r)

    t_end_r = time.perf_counter()
    dt_r = t_end_r - t0_r
    rate_r = (total_r / 1024) / dt_r if dt_r > 0 else 0
    print(f"--> LEITURA FINALIZADA: {total_r/1024:.0f} KB em {dt_r:.2f}s = {rate_r:.1f} KB/s ({rate_r/1024:.2f} MB/s)")
    sys.stdout.flush()

    try:
        os.remove(filepath)
    except:
        pass

    print(f"\n==================================================")
    print(f"  RESULTADO FINAL DO BENCHMARK:")
    print(f"  - Gravacao: {rate_w:.1f} KB/s ({rate_w/1024:.2f} MB/s)")
    print(f"  - Leitura:  {rate_r:.1f} KB/s ({rate_r/1024:.2f} MB/s)")
    print(f"==================================================")

if __name__ == "__main__":
    drive_letter = sys.argv[1] if len(sys.argv) > 1 else "D"
    run_benchmark(drive_letter, 2)

