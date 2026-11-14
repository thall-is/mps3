import threading
import time
import requests
import json
import os
import sys

TARGET = os.environ.get("MPS3_TARGET", "http://mps3.local")
POLL_INTERVAL_SEC = 0.1  # 100 ms polling

class StatusSampler:
    def __init__(self, target_url):
        self.target_url = target_url
        self.samples = []
        self.running = False
        self.thread = None

    def start(self):
        self.samples = []
        self.running = True
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def stop(self):
        self.running = False
        if self.thread:
            self.thread.join(timeout=2.0)

    def _run(self):
        session = requests.Session()
        while self.running:
            t = time.time()
            try:
                r = session.get(f"{self.target_url}/api/status", timeout=1.0)
                if r.status_code == 200:
                    data = r.json()
                    st = data.get("sd_status", "unknown")
                    self.samples.append((t, st, True))
                else:
                    self.samples.append((t, f"HTTP_{r.status_code}", False))
            except Exception as e:
                self.samples.append((t, f"ERR_{type(e).__name__}", False))
            time.sleep(POLL_INTERVAL_SEC)

def analyze_samples(samples, op_start, op_end, expected_state):
    op_samples = [s for s in samples if op_start <= s[0] <= op_end]
    before_samples = [s for s in samples if s[0] < op_start]
    after_samples = [s for s in samples if s[0] > op_end]

    total_op = len(op_samples)
    if total_op == 0:
        return {
            "total_samples": 0,
            "correct_samples": 0,
            "accuracy_pct": 0.0,
            "distribution": {},
            "detection_delay_ms": None,
            "release_delay_ms": None,
        }

    dist = {}
    correct = 0
    for _, state, ok in op_samples:
        dist[state] = dist.get(state, 0) + 1
        if state == expected_state:
            correct += 1

    first_detected = None
    for s in op_samples:
        if s[1] == expected_state:
            first_detected = s[0]
            break

    last_detected = None
    for s in reversed(op_samples):
        if s[1] == expected_state:
            last_detected = s[0]
            break

    detection_delay_ms = ((first_detected - op_start) * 1000.0) if first_detected else None
    
    # Release delay: after op_end, when did it become 'free'?
    release_delay_ms = None
    if after_samples:
        for s in after_samples:
            if s[1] == "free":
                release_delay_ms = (s[0] - op_end) * 1000.0
                break

    return {
        "total_samples": total_op,
        "correct_samples": correct,
        "accuracy_pct": (correct / total_op) * 100.0,
        "distribution": dist,
        "detection_delay_ms": detection_delay_ms,
        "release_delay_ms": release_delay_ms,
    }

def run_tests():
    print("=" * 70)
    print("  AVALIACAO DE PRECISAO DO SEMAFORO DO CARTAO SD NO MPS3")
    print(f"  Target: {TARGET} | Amostragem: a cada {POLL_INTERVAL_SEC*1000:.0f} ms")
    print("=" * 70)

    sampler = StatusSampler(TARGET)

    # -------------------------------------------------------------
    # Teste 1: Estado Ocioso (Repouso)
    # -------------------------------------------------------------
    print("\n[TESTE 1] Repouso (Cartão SD ocioso durante 3 segundos)...")
    sampler.start()
    t_start = time.time()
    time.sleep(3.0)
    t_end = time.time()
    sampler.stop()

    res1 = analyze_samples(sampler.samples, t_start, t_end, "free")
    print(f"  -> Total amostras: {res1['total_samples']}")
    print(f"  -> Distribuicao: {res1['distribution']}")
    print(f"  -> Precisao: {res1['accuracy_pct']:.1f}% correto como 'free'")

    # -------------------------------------------------------------
    # Teste 2: Leitura Pesada no SD (POST /api/sync/verify em arquivo de 63 MB)
    # -------------------------------------------------------------
    verify_target = "Podcasts/Atila Iamarino/O problema das ervas medicinais - PODCAST Nao Ficcao.mp3"
    print(f"\n[TESTE 2] Leitura Fisica Intensa no SD (Releitura SHA-256 de 62.93 MB)...")
    sampler.start()
    time.sleep(0.5)
    t_start = time.time()
    r = requests.post(f"{TARGET}/api/sync/verify", json={"path": verify_target}, timeout=30)
    t_end = time.time()
    time.sleep(0.5)
    sampler.stop()

    dt = t_end - t_start
    print(f"  -> Releitura física do SD concluída em {dt:.2f}s (Status: {r.status_code})")
    res2 = analyze_samples(sampler.samples, t_start, t_end, "reading")
    print(f"  -> Total amostras coletadas durante a leitura do SD: {res2['total_samples']}")
    print(f"  -> Distribuicao de estados reportados pelo firmware: {res2['distribution']}")
    print(f"  -> Precisao do status 'reading': {res2['accuracy_pct']:.1f}%")
    if "free" in res2['distribution']:
        print(f"  ⚠️  FALHA DE FIDELIDADE: O SD estava fisicamente em leitura intensa, mas o firmware reportou 'free' em {res2['distribution']['free']} amostras!")

    # -------------------------------------------------------------
    # Teste 3: Gravacao Longa no SD (Upload de 4 MB)
    # -------------------------------------------------------------
    payload_4mb = os.urandom(4 * 1024 * 1024)
    print(f"\n[TESTE 3] Gravacao Continua no SD (Upload de 4.0 MB)...")
    sampler.start()
    time.sleep(0.5)
    t_start = time.time()
    r = requests.put(
        f"{TARGET}/api/upload?path=%2FPodcasts%2F.staging%2Ftest_semaforo_4mb.bin",
        data=payload_4mb,
        headers={"Content-Type": "application/octet-stream", "Connection": "close"},
        timeout=30
    )
    t_end = time.time()
    time.sleep(0.8)
    sampler.stop()

    dt = t_end - t_start
    speed = 4.0 / max(dt, 0.001)
    print(f"  -> Upload concluido em {dt:.2f}s ({speed:.2f} MB/s) (Status: {r.status_code})")
    res3 = analyze_samples(sampler.samples, t_start, t_end, "writing")
    print(f"  -> Total amostras durante a gravacao: {res3['total_samples']}")
    print(f"  -> Distribuicao de estados: {res3['distribution']}")
    print(f"  -> Precisao do status 'writing': {res3['accuracy_pct']:.1f}%")
    print(f"  -> Latencia para detectar 'writing': {res3['detection_delay_ms']} ms")
    print(f"  -> Latencia para retornar a 'free': {res3['release_delay_ms']} ms")

    # Limpa arquivo
    requests.delete(f"{TARGET}/api/delete?path=%2FPodcasts%2F.staging%2Ftest_semaforo_4mb.bin", timeout=5)

    # -------------------------------------------------------------
    # Teste 4: Gravacao Rapida no SD (Upload de 256 KB)
    # -------------------------------------------------------------
    payload_256k = os.urandom(256 * 1024)
    print(f"\n[TESTE 4] Gravacao Rapida no SD (Upload de 256 KB)...")
    sampler.start()
    time.sleep(0.3)
    t_start = time.time()
    r = requests.put(
        f"{TARGET}/api/upload?path=%2FPodcasts%2F.staging%2Ftest_semaforo_256k.bin",
        data=payload_256k,
        headers={"Content-Type": "application/octet-stream", "Connection": "close"},
        timeout=10
    )
    t_end = time.time()
    time.sleep(0.8)
    sampler.stop()

    dt = t_end - t_start
    print(f"  -> Upload rapido concluido em {dt*1000:.0f} ms (Status: {r.status_code})")
    res4 = analyze_samples(sampler.samples, t_start, t_end, "writing")
    print(f"  -> Total amostras durante os {dt*1000:.0f} ms: {res4['total_samples']}")
    print(f"  -> Distribuicao: {res4['distribution']}")
    print(f"  -> Detectado como 'writing'? {'SIM' if res4['correct_samples'] > 0 else 'NAO'}")
    print(f"  -> Impacto na pagina web (polling de 8000 ms): Probabilidade da pagina ver este upload = {(dt / 8.0) * 100:.1f}% (quase invisivel!)")

    # Limpa arquivo
    requests.delete(f"{TARGET}/api/delete?path=%2FPodcasts%2F.staging%2Ftest_semaforo_256k.bin", timeout=5)

    # -------------------------------------------------------------
    # Teste 5: Download / Streaming HTTP do SD (GET /api/download)
    # -------------------------------------------------------------
    print(f"\n[TESTE 5] Streaming / Download de Arquivo do SD (GET /api/download)...")
    dl_path = urllib_quote = requests.utils.quote("/Podcasts/Atila Iamarino/O problema das ervas medicinais - PODCAST Nao Ficcao.mp3")
    sampler.start()
    time.sleep(0.3)
    t_start = time.time()
    bytes_read = 0
    with requests.get(f"{TARGET}/api/download?path={dl_path}", stream=True, timeout=10) as resp:
        for chunk in resp.iter_content(chunk_size=64*1024):
            bytes_read += len(chunk)
            if bytes_read >= 5 * 1024 * 1024:  # Le 5 MB
                break
    t_end = time.time()
    time.sleep(0.5)
    sampler.stop()

    dt = t_end - t_start
    print(f"  -> 5.0 MB lidos do SD em {dt:.2f}s ({5.0/dt:.2f} MB/s)")
    res5 = analyze_samples(sampler.samples, t_start, t_end, "reading")
    print(f"  -> Total amostras durante a leitura streaming: {res5['total_samples']}")
    print(f"  -> Distribuicao de estados reportados pelo firmware: {res5['distribution']}")
    print(f"  -> Precisao do status 'reading': {res5['accuracy_pct']:.1f}%")
    if "free" in res5['distribution']:
        print(f"  ⚠️  FALHA DE FIDELIDADE: O SD estava transmitindo dados a {5.0/dt:.2f} MB/s, mas o firmware reportou 'free'!")

    # -------------------------------------------------------------
    # Resumo Executivo
    # -------------------------------------------------------------
    print("\n" + "=" * 70)
    print("  RESUMO EXECUTIVO DA FIDELIDADE DO SEMAFORO ATUAL")
    print("=" * 70)
    print(f"  1. Repouso (Idle)               : {res1['accuracy_pct']:.1f}% correto")
    print(f"  2. Leitura Fisica no SD (Verify): {res2['accuracy_pct']:.1f}% correto (Esperava 'reading', recebeu {list(res2['distribution'].keys())})")
    print(f"  3. Gravacao Longa (4 MB)        : {res3['accuracy_pct']:.1f}% correto ('writing')")
    print(f"  4. Gravacao Curta (256 KB)      : {'Detectado' if res4['correct_samples'] > 0 else 'Nao detectado'} ({res4['total_samples']} amostras em {dt*1000:.0f}ms)")
    print(f"  5. Streaming/Download (5 MB)    : {res5['accuracy_pct']:.1f}% correto (Esperava 'reading', recebeu {list(res5['distribution'].keys())})")
    print("=" * 70)

if __name__ == "__main__":
    run_tests()

