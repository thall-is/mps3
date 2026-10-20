import os
import sys
import time
import hashlib

TARGET_DRIVE = "D:"
TEST_FILE = os.path.join(TARGET_DRIVE, ".msc_bench_test.tmp")

def run_benchmark(sizes_mb=[1, 5, 10, 20]):
    print("=" * 70)
    print("  MPS3 USB MSC SPEED BENCHMARK (Hardware In-The-Loop)")
    print(f"  Target Drive: {TARGET_DRIVE} (MicroSD FAT32)")
    print("=" * 70)

    if not os.path.exists(TARGET_DRIVE):
        print(f"[ERRO FATAL] Drive {TARGET_DRIVE} nao encontrado no sistema!")
        sys.exit(1)

    results = []

    for sz_mb in sizes_mb:
        sz_bytes = sz_mb * 1024 * 1024
        print(f"\n[*] Testando bloco de {sz_mb} MB ({sz_bytes} bytes)...")

        # 1. Gera payload em memoria
        payload = os.urandom(sz_bytes)
        expected_sha = hashlib.sha256(payload).hexdigest()

        # 2. Teste de Escrita (Write)
        t0 = time.perf_counter()
        with open(TEST_FILE, "wb") as f:
            f.write(payload)
            f.flush()
            os.fsync(f.fileno())  # Forca descarga fisica no cartao SD
        dt_write = time.perf_counter() - t0
        write_speed = sz_mb / max(dt_write, 0.0001)

        print(f"    [WRITE] {sz_mb} MB em {dt_write:.3f}s -> {write_speed:.2f} MB/s ({write_speed*8:.2f} Mbps)")

        # Pequena pausa para o controlador do SD assentar
        time.sleep(0.2)

        # 3. Teste de Leitura (Read)
        t0 = time.perf_counter()
        read_sha_ctx = hashlib.sha256()
        total_read = 0
        with open(TEST_FILE, "rb") as f:
            while True:
                chunk = f.read(64 * 1024)
                if not chunk:
                    break
                read_sha_ctx.update(chunk)
                total_read += len(chunk)
        dt_read = time.perf_counter() - t0
        read_speed = sz_mb / max(dt_read, 0.0001)
        actual_sha = read_sha_ctx.hexdigest()
        sha_match = (actual_sha == expected_sha)

        print(f"    [READ ] {sz_mb} MB em {dt_read:.3f}s -> {read_speed:.2f} MB/s ({read_speed*8:.2f} Mbps)")
        print(f"    [SHA256] {'MATCH 100%' if sha_match else 'MISMATCH!'} ({actual_sha[:16]}...)")

        if not sha_match:
            print("    [ALERTA CRITICO] Corrupcao de dados detectada na leitura!")

        # 4. Limpa arquivo temporario
        try:
            os.remove(TEST_FILE)
        except Exception:
            pass

        results.append({
            "size_mb": sz_mb,
            "write_s": dt_write,
            "write_mbs": write_speed,
            "read_s": dt_read,
            "read_mbs": read_speed,
            "sha_ok": sha_match
        })

    print("\n" + "=" * 70)
    print("  RESUMO DE DESEMPENHO USB MSC")
    print("=" * 70)
    print(f"{'Tamanho':<10} | {'Escrita (MB/s)':<16} | {'Leitura (MB/s)':<16} | {'Integridade':<12}")
    print("-" * 70)
    for r in results:
        status_str = "OK (Bit-Exact)" if r["sha_ok"] else "FALHOU"
        print(f"{r['size_mb']:>4} MB     | {r['write_mbs']:>8.2f} MB/s     | {r['read_mbs']:>8.2f} MB/s     | {status_str}")
    print("=" * 70)

if __name__ == "__main__":
    run_benchmark([1, 5, 10, 20])

