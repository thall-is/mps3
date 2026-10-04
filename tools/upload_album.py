import os
import sys
import time
import urllib.request
import urllib.parse

# Assegura que o console Windows nao quebre com caracteres unicode especiais como \uff1f
if sys.platform.startswith("win"):
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

def safe_str(s):
    return s.encode("ascii", "replace").decode("ascii")

def upload_album(album_dir, base_url="http://192.168.15.61"):
    if not os.path.isdir(album_dir):
        print(f"Erro: Diretorio nao encontrado: {album_dir}")
        return False

    folder_name = os.path.basename(os.path.normpath(album_dir))
    files = sorted([f for f in os.listdir(album_dir) if os.path.isfile(os.path.join(album_dir, f))])
    
    total_files = len(files)
    total_bytes = sum(os.path.getsize(os.path.join(album_dir, f)) for f in files)
    
    print("=" * 60)
    print(f" MPS3 ALBUM UPLOADER: {folder_name}")
    print(f" Destino: {base_url}")
    print(f" Total de faixas: {total_files} | Tamanho total: {total_bytes / (1024*1024):.2f} MB")
    print("=" * 60)

    # Verifica status antes de iniciar
    try:
        req = urllib.request.urlopen(f"{base_url}/api/status", timeout=5)
        print("Status do dispositivo:", req.read().decode())
    except Exception as e:
        print(f"Erro ao conectar com {base_url}: {e}")
        return False

    success_count = 0
    fail_count = 0
    start_all = time.time()
    uploaded_bytes = 0

    for idx, fname in enumerate(files, 1):
        fpath = os.path.join(album_dir, fname)
        fsize = os.path.getsize(fpath)
        
        rel_path = f"{folder_name}/{fname}"
        enc_path = urllib.parse.quote(rel_path)
        url = f"{base_url}/api/upload?path={enc_path}&idx={idx}&count={total_files}&batchTotal={total_bytes}"
        
        display_name = safe_str(fname)
        print(f"\n[{idx}/{total_files}] Enviando: {display_name} ({fsize / (1024*1024):.2f} MB)...")
        t0 = time.time()
        
        try:
            with open(fpath, "rb") as f:
                req = urllib.request.Request(url, data=f, method="PUT")
                req.add_header("Content-Type", "application/octet-stream")
                req.add_header("Content-Length", str(fsize))
                with urllib.request.urlopen(req, timeout=300) as resp:
                    t1 = time.time()
                    dur = max(t1 - t0, 0.001)
                    speed = (fsize / (1024 * 1024)) / dur
                    uploaded_bytes += fsize
                    success_count += 1
                    print(f" -> SUCESSO ({resp.status}) em {dur:.1f}s | Taxa: {speed:.2f} MB/s")
        except Exception as e:
            t1 = time.time()
            dur = max(t1 - t0, 0.001)
            fail_count += 1
            print(f" -> FALHA apos {dur:.1f}s: {e}")

    total_dur = time.time() - start_all
    avg_speed = (uploaded_bytes / (1024 * 1024)) / max(total_dur, 0.001)
    
    print("\n" + "=" * 60)
    print(" RESUMO DA TRANSFERENCIA DO ALBUM")
    print(f" Sucesso: {success_count}/{total_files} faixas")
    print(f" Falhas:  {fail_count}/{total_files}")
    print(f" Total transferido: {uploaded_bytes / (1024*1024):.2f} MB")
    print(f" Tempo total: {total_dur:.1f}s | Taxa media: {avg_speed:.2f} MB/s")
    print("=" * 60)
    
    return fail_count == 0

if __name__ == "__main__":
    album_path = sys.argv[1] if len(sys.argv) > 1 else r"C:\Users\thall\Music\To Pimp A Butterfly"
    target = sys.argv[2] if len(sys.argv) > 2 else "http://192.168.15.61"
    upload_album(album_path, target)
