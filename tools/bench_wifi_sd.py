#!/usr/bin/env python3
import sys, time, hashlib, urllib.request, os

sys.stdout.reconfigure(encoding='utf-8')
base_url = 'http://192.168.15.61'

sizes_mb = [0.5, 1.0, 2.0, 4.0]

print("=================================================================")
print("     MPS3 DAP - TESTE DE UPLOAD & DOWNLOAD WI-FI / SDMMC")
print("=================================================================")

for smb in sizes_mb:
    size_bytes = int(smb * 1024 * 1024)
    data = os.urandom(size_bytes)
    h_orig = hashlib.md5(data).hexdigest()
    
    filename = f"/test_{int(smb*1024)}k.bin"
    url = f"{base_url}/api/upload?path={filename}"
    print(f"\n[+] Testando upload de {smb:.1f} MB ({size_bytes} bytes)...")
    
    t0 = time.perf_counter()
    req = urllib.request.Request(url, data=data, method='PUT')
    with urllib.request.urlopen(req, timeout=30) as resp:
        res_code = resp.status
    t1 = time.perf_counter()
    dur = t1 - t0
    speed_mbs = (size_bytes / (1024 * 1024)) / dur
    print(f"  -> Upload PUT concluido: {dur:.2f}s | {speed_mbs:.2f} MB/s ({speed_mbs*8:.2f} Mbps)")
    
    # Download e verificacao de integridade MD5
    dl_url = f"{base_url}/api/download?path={filename}"
    t0_dl = time.perf_counter()
    with urllib.request.urlopen(dl_url, timeout=30) as resp:
        dl_data = resp.read()
    t1_dl = time.perf_counter()
    dur_dl = t1_dl - t0_dl
    speed_dl_mbs = (len(dl_data) / (1024 * 1024)) / dur_dl
    h_dl = hashlib.md5(dl_data).hexdigest()
    
    md5_ok = (h_orig == h_dl)
    status_str = "INTEGRIDADE PERFEITA (MD5 MATCH)" if md5_ok else "FALHA MD5"
    print(f"  -> Download GET concluido: {dur_dl:.2f}s | {speed_dl_mbs:.2f} MB/s ({speed_dl_mbs*8:.2f} Mbps)")
    print(f"  -> Validacao: {status_str}")
    
    # Remove arquivo de teste
    del_req = urllib.request.Request(f"{base_url}/api/delete?path={filename}", method='DELETE')
    with urllib.request.urlopen(del_req, timeout=5) as del_resp:
        pass

print("\n=================================================================")
print("                     TESTES CONCLUIDOS COM EXITO                 ")
print("=================================================================")

