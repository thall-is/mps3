import os
import sys
import time
import hashlib
import argparse
import ctypes
from ctypes import wintypes

# Win32 API flags para ignorar completamente o cache de RAM do Windows
GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
OPEN_EXISTING = 3
CREATE_ALWAYS = 2
FILE_SHARE_READ = 1
FILE_SHARE_WRITE = 2
FILE_FLAG_NO_BUFFERING = 0x20000000
FILE_FLAG_WRITE_THROUGH = 0x80000000
FILE_ATTRIBUTE_NORMAL = 0x80
INVALID_HANDLE_VALUE = -1

kernel32 = ctypes.windll.kernel32

def format_bytes(size):
    if size >= 1024 * 1024:
        return f"{size / (1024 * 1024):.2f} MB"
    elif size >= 1024:
        return f"{size / 1024:.2f} KB"
    else:
        return f"{size} B"

def format_speed(speed_bytes_per_sec):
    mb_s = speed_bytes_per_sec / (1024 * 1024)
    kb_s = speed_bytes_per_sec / 1024
    if mb_s >= 1.0:
        return f"{mb_s:.2f} MB/s ({kb_s:.1f} KB/s)"
    else:
        return f"{kb_s:.1f} KB/s"

def generate_test_data(size):
    # Gera dados pseudo-aleatorios com padrao
    chunk = os.urandom(64 * 1024) # 64 KB
    full_chunks = size // len(chunk)
    remainder = size % len(chunk)
    return chunk * full_chunks + chunk[:remainder]

def read_file_no_cache(filepath, file_size):
    """Le o arquivo usando FILE_FLAG_NO_BUFFERING para forcar leitura fisica do USB sem cache do Windows."""
    handle = kernel32.CreateFileW(
        filepath,
        GENERIC_READ,
        FILE_SHARE_READ,
        None,
        OPEN_EXISTING,
        FILE_FLAG_NO_BUFFERING,
        None
    )
    if handle == INVALID_HANDLE_VALUE:
        # Fallback para leitura normal se no_buffering falhar
        with open(filepath, "rb") as f:
            return f.read()
    
    try:
        buffer_size = 64 * 1024 # 64 KB alinhado
        buf = ctypes.create_string_buffer(buffer_size)
        bytes_read = wintypes.DWORD()
        data = bytearray()
        remaining = file_size
        
        while remaining > 0:
            to_read = buffer_size
            success = kernel32.ReadFile(handle, buf, to_read, ctypes.byref(bytes_read), None)
            if not success or bytes_read.value == 0:
                break
            actual = min(bytes_read.value, remaining)
            data.extend(buf.raw[:actual])
            remaining -= actual
            
        return bytes(data)
    finally:
        kernel32.CloseHandle(handle)

def test_file_transfer(drive_letter, test_sizes_mb=[1, 5, 10]):
    test_dir = os.path.join(drive_letter, "_benchmark_test")
    
    print("=" * 72)
    print(f"      MPS3 USB MASS STORAGE (MSC) HARDWARE PERFORMANCE TEST")
    print(f"      Unidade de teste: {drive_letter}")
    print(f"      Modo: Leitura e Escrita Diretas (Zero-Cache no Host)")
    print("=" * 72)

    if not os.path.exists(drive_letter):
        print(f"[ERRO] A unidade {drive_letter} nao esta acessivel no Windows!")
        return False

    os.makedirs(test_dir, exist_ok=True)
    results = []

    try:
        # --- TESTE 1: ARQUIVOS SEQUENCIAIS (VAZAO BRUTA) ---
        print("\n[FASE 1] Teste de Vazao Sequencial (Escrita Fisica e Leitura Sem Cache):")
        print("-" * 72)
        
        for size_mb in test_sizes_mb:
            size_bytes = size_mb * 1024 * 1024
            filename = os.path.join(test_dir, f"test_{size_mb}mb.bin")
            
            print(f"-> Gerando dados ({size_mb} MB)...", end="", flush=True)
            data = generate_test_data(size_bytes)
            data_hash = hashlib.sha256(data).hexdigest()
            print(" Pronto.")

            # Escrita Direta com fsync (PC -> ESP32 SD)
            print(f"-> Gravando {size_mb} MB ({filename})...", end="", flush=True)
            t0 = time.perf_counter()
            with open(filename, "wb") as f:
                chunk_size = 64 * 1024
                for offset in range(0, size_bytes, chunk_size):
                    f.write(data[offset:offset + chunk_size])
                f.flush()
                os.fsync(f.fileno()) # Forca commit fisico no cartao SD
            t1 = time.perf_counter()
            write_time = t1 - t0
            write_speed = size_bytes / write_time if write_time > 0 else 0
            print(f" Concluido! ({write_time:.2f}s | {format_speed(write_speed)})")

            # Leitura Direta Fisica (ESP32 SD -> PC) sem cache do Windows
            print(f"-> Lendo {size_mb} MB fisicamente do USB (Direct I/O)...", end="", flush=True)
            t2 = time.perf_counter()
            read_bytes = read_file_no_cache(filename, size_bytes)
            t3 = time.perf_counter()
            read_time = t3 - t2
            read_speed = size_bytes / read_time if read_time > 0 else 0
            
            read_hash = hashlib.sha256(read_bytes).hexdigest()
            hash_ok = (data_hash == read_hash)
            
            status_str = "SHA-256 INTEGRO" if hash_ok else "ERRO DE HASH!"
            print(f" Concluido! ({read_time:.2f}s | {format_speed(read_speed)} | {status_str})")

            results.append({
                "test": f"Sequencial {size_mb} MB",
                "size_mb": size_mb,
                "write_speed": write_speed,
                "read_speed": read_speed,
                "integrity": hash_ok
            })

        # --- TESTE 2: MULTIPLOS ARQUIVOS (LATENCIA DE SISTEMA DE ARQUIVOS) ---
        print("\n[FASE 2] Teste de Arquivos Curtos (30 arquivos de 64 KB):")
        print("-" * 72)
        num_small_files = 30
        small_size = 64 * 1024 # 64 KB
        total_small_bytes = num_small_files * small_size
        small_payload = generate_test_data(small_size)
        small_hash = hashlib.sha256(small_payload).hexdigest()

        print(f"-> Gravando {num_small_files} arquivos de 64 KB...", end="", flush=True)
        t_w0 = time.perf_counter()
        for i in range(num_small_files):
            fname = os.path.join(test_dir, f"small_{i:02d}.bin")
            with open(fname, "wb") as f:
                f.write(small_payload)
                f.flush()
                os.fsync(f.fileno())
        t_w1 = time.perf_counter()
        small_write_time = t_w1 - t_w0
        small_write_speed = total_small_bytes / small_write_time if small_write_time > 0 else 0
        print(f" Concluido! ({small_write_time:.2f}s | {format_speed(small_write_speed)})")

        print(f"-> Lendo {num_small_files} arquivos com Direct I/O...", end="", flush=True)
        t_r0 = time.perf_counter()
        all_small_ok = True
        for i in range(num_small_files):
            fname = os.path.join(test_dir, f"small_{i:02d}.bin")
            content = read_file_no_cache(fname, small_size)
            if hashlib.sha256(content).hexdigest() != small_hash:
                all_small_ok = False
        t_r1 = time.perf_counter()
        small_read_time = t_r1 - t_r0
        small_read_speed = total_small_bytes / small_read_time if small_read_time > 0 else 0
        status_str = "TODOS 100% OK" if all_small_ok else "ERRO"
        print(f" Concluido! ({small_read_time:.2f}s | {format_speed(small_read_speed)} | {status_str})")

        results.append({
            "test": f"Pequenos (30x64KB)",
            "size_mb": total_small_bytes / (1024 * 1024),
            "write_speed": small_write_speed,
            "read_speed": small_read_speed,
            "integrity": all_small_ok
        })

    finally:
        print("\n-> Limpando arquivos temporarios de teste...", end="", flush=True)
        for root, dirs, files in os.walk(test_dir, topdown=False):
            for file in files:
                try:
                    os.remove(os.path.join(root, file))
                except Exception:
                    pass
            for d in dirs:
                try:
                    os.rmdir(os.path.join(root, d))
                except Exception:
                    pass
        try:
            os.rmdir(test_dir)
        except Exception:
            pass
        print(" Limpeza concluida.")

    # --- TABELA DE RESULTADOS ---
    print("\n" + "=" * 72)
    print("                     RELATORIO COMPARATIVO DE PERFORMANCE")
    print("=" * 72)
    print(f"{'Cenario de Teste':<22} | {'Gravacao no SD (PC->ESP)':<24} | {'Leitura do SD (ESP->PC)':<24} | {'Integridade'}")
    print("-" * 72)
    for r in results:
        print(f"{r['test']:<22} | {format_speed(r['write_speed']):<24} | {format_speed(r['read_speed']):<24} | {'APROVADO' if r['integrity'] else 'REPROVADO'}")
    print("=" * 72)
    return True

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="MPS3 USB MSC Transfer Benchmark")
    parser.add_argument("--drive", default="D:\\", help="Letra da unidade (ex: D:\\)")
    parser.add_argument("--sizes", nargs="+", type=int, default=[1, 5, 10], help="Tamanhos em MB para teste")
    args = parser.parse_args()

    drive = args.drive
    if not drive.endswith("\\"):
        drive += "\\"
    
    test_file_transfer(drive, args.sizes)

