#!/usr/bin/env python3
"""
Test Suite Automatizada para a Interface Web e Endpoints REST do MPS3.
Valida:
1. Status do Sistema e Semáforo de SD (/api/status)
2. Criação de Diretórios aninhados (/api/mkdir)
3. Upload e Download com HTTP 206 Partial Content (Streaming de Áudio)
4. Mover e Copiar Arquivos (/api/move e /api/copy)
5. Exclusão de Itens (/api/delete)
6. Concorrência: Streaming de Áudio simulado vs Navegação de Pastas (/api/list)
"""

import sys
import time
import json
import urllib.request
import urllib.error
import threading

DEFAULT_HOST = "http://192.168.15.61"
NAV_PORT = None

args = sys.argv[1:]
if "--nav" in args:
    idx = args.index("--nav")
    if idx + 1 < len(args):
        NAV_PORT = args[idx + 1]
        args.pop(idx + 1)
    else:
        NAV_PORT = "COM5"
    args.pop(idx)

if len(args) > 0:
    BASE_URL = args[0].rstrip("/")
    if not BASE_URL.startswith("http://"):
        BASE_URL = "http://" + BASE_URL
else:
    BASE_URL = DEFAULT_HOST

PASS_COUNT = 0
FAIL_COUNT = 0
nav_inst = None

def check_device_health(context=""):
    global nav_inst
    if nav_inst:
        if nav_inst._reboot_detected:
            print(f"\n[ERRO CRITICO FATAL] DISPOSITIVO REINICIOU durante {context}!", flush=True)
            print(f"  Razao do reboot: {nav_inst._reboot_reason}", flush=True)
            print("  Ultimos logs seriais capturados:")
            for l in nav_inst.raw_logs[-30:]:
                print(f"    {l}", flush=True)
            sys.exit(1)
        try:
            ui = nav_inst.query_ui(timeout=1.0)
            if not ui.get('wifi'):
                print(f"\n[ERRO CRITICO FATAL] Dispositivo SAIU DO MODO WI-FI durante {context}!", flush=True)
                print(f"  Estado atual da UI: {ui}", flush=True)
                sys.exit(1)
        except Exception:
            pass

def test_assert(name, condition, details=""):
    global PASS_COUNT, FAIL_COUNT
    if condition:
        print(f"  [PASS] {name} {details}", flush=True)
        PASS_COUNT += 1
        return True
    else:
        print(f"  [FAIL] {name} - {details}", flush=True)
        FAIL_COUNT += 1
        check_device_health(f"teste '{name}'")
        return False

def http_get(path, headers=None, timeout=5):
    url = f"{BASE_URL}{path}"
    req = urllib.request.Request(url, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, resp.headers, resp.read()
    except urllib.error.HTTPError as e:
        return e.code, e.headers, e.read()
    except Exception as e:
        check_device_health(f"GET {path}")
        return 0, {}, str(e).encode()

def http_post(path, data_bytes, content_type="application/json", timeout=5):
    url = f"{BASE_URL}{path}"
    headers = {"Content-Type": content_type}
    req = urllib.request.Request(url, data=data_bytes, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, resp.headers, resp.read()
    except urllib.error.HTTPError as e:
        return e.code, e.headers, e.read()
    except Exception as e:
        check_device_health(f"POST {path}")
        return 0, {}, str(e).encode()

def http_put(path, data_bytes, content_type="application/octet-stream", timeout=15):
    url = f"{BASE_URL}{path}"
    headers = {"Content-Type": content_type}
    req = urllib.request.Request(url, data=data_bytes, headers=headers, method="PUT")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, resp.headers, resp.read()
    except urllib.error.HTTPError as e:
        return e.code, e.headers, e.read()
    except Exception as e:
        check_device_health(f"PUT {path}")
        return 0, {}, str(e).encode()

def http_delete(path, timeout=5):
    url = f"{BASE_URL}{path}"
    req = urllib.request.Request(url, method="DELETE")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, resp.headers, resp.read()
    except urllib.error.HTTPError as e:
        return e.code, e.headers, e.read()
    except Exception as e:
        check_device_health(f"DELETE {path}")
        return 0, {}, str(e).encode()

def capture_screen(label):
    if nav_inst:
        try:
            path = f"tools/screen_{label}.png"
            nav_inst.screenshot(path)
            ui = nav_inst.query_ui()
            print(f"  [OLED MONITOR] Screenshot capturada: {path} (modo={ui.get('mode_name')}, wifi={ui.get('wifi')})", flush=True)
            if not ui.get('wifi'):
                print(f"\n[ERRO CRITICO FATAL] Dispositivo SAIU DO MODO WI-FI na captura {label}!", flush=True)
                sys.exit(1)
        except Exception as e:
            print(f"  [OLED MONITOR AVISO] Falha ao capturar tela ({e})", flush=True)
            check_device_health(f"captura {label}")

def run_tests():
    global nav_inst
    if NAV_PORT:
        print(f"[*] Modo HIL ativado com porta serial {NAV_PORT}. Monitorando tela OLED...", flush=True)
        try:
            from mps3_nav import Mps3Nav
            nav_inst = Mps3Nav(NAV_PORT)
            nav_inst.open()
            nav_inst.send_cmd("wifi")
            time.sleep(2.5)
            capture_screen("01_wifi_ready")
        except Exception as e:
            print(f"[!] Aviso: Nao foi possivel iniciar Mps3Nav ({e}). Continuando...")
            nav_inst = None

    try:
        return _run_tests_body()
    finally:
        if nav_inst:
            try:
                capture_screen("final_state")
                nav_inst.close()
            except Exception:
                pass

def _run_tests_body():
    print(f"=== INICIANDO BATERIA DE TESTES WEB MPS3 [{BASE_URL}] ===")

    # Teste 1: /api/status e Semáforo
    print("\n[1] Verificando /api/status e Semáforo do Cartão SD...")
    code, hdrs, body = http_get("/api/status")
    if test_assert("Status HTTP 200", code == 200, f"code={code}"):
        try:
            status_data = json.loads(body.decode())
            test_assert("Bateria presente", "battery_percent" in status_data, f"{status_data.get('battery_percent')}%")
            test_assert("Espaço do SD", "total" in status_data and "free" in status_data, f"Total: {status_data.get('total')} B")
            test_assert("Semáforo SD ('sd_status')", "sd_status" in status_data, f"Estado: {status_data.get('sd_status')}")
        except Exception as e:
            test_assert("JSON parse", False, str(e))

    # Teste 2: Criação de Pastas (/api/mkdir)
    print("\n[2] Testando Criação de Diretórios (/api/mkdir)...")
    dir_target = "/test_web_suite"
    payload = json.dumps({"path": dir_target}).encode()
    code, hdrs, body = http_post("/api/mkdir", payload)
    test_assert("Criar pasta raiz de testes", code == 200, f"code={code}")

    sub_target = "/test_web_suite/sub_nested"
    payload = json.dumps({"path": sub_target}).encode()
    code, hdrs, body = http_post("/api/mkdir", payload)
    test_assert("Criar subpasta aninhada", code == 200, f"code={code}")
    capture_screen("02_after_mkdir")

    # Teste 3: Listagem de Pastas (/api/list)
    print("\n[3] Testando Listagem de Diretório (/api/list)...")
    code, hdrs, body = http_get(f"/api/list?path={urllib.parse.quote(dir_target)}")
    if test_assert("Listar /test_web_suite", code == 200):
        try:
            data = json.loads(body.decode())
            entries = data.get("entries", [])
            sub_found = any(e.get("name") == "sub_nested" and e.get("dir") for e in entries)
            test_assert("Subpasta encontrada na lista", sub_found)
        except Exception as e:
            test_assert("Parse listagem", False, str(e))

    # Teste 4: Upload de Arquivo (/api/upload)
    print("\n[4] Testando Upload de Arquivo (/api/upload)...")
    sample_content = b"MPS3_AUDIO_STREAM_TEST_DATA_" * 1024 # ~29 KB
    file_target = f"{dir_target}/test_stream.bin"
    upload_url = f"/api/upload?path={urllib.parse.quote(file_target)}"
    code, hdrs, body = http_put(upload_url, sample_content)
    test_assert("Upload de arquivo de teste", code == 200, f"code={code}")
    capture_screen("03_after_upload")

    # Teste 5: Download & HTTP 206 Partial Content
    print("\n[5] Testando Streaming de Áudio com HTTP 206 Range Request...")
    down_url = f"/api/download?path={urllib.parse.quote(file_target)}"
    range_headers = {"Range": "bytes=0-1023"}
    code, hdrs, body = http_get(down_url, headers=range_headers)
    test_assert("HTTP 206 Partial Content", code == 206, f"code={code}")
    content_len = hdrs.get("Content-Length")
    test_assert("Header Content-Length == 1024", content_len == "1024", f"got {content_len}")
    content_range = hdrs.get("Content-Range", "")
    test_assert("Header Content-Range presente", "bytes 0-1023/" in content_range, f"got {content_range}")
    test_assert("Conteúdo do Range confere", len(body) == 1024 and body == sample_content[:1024])

    # Teste 6: Cópia de Arquivo (/api/copy)
    print("\n[6] Testando Cópia de Arquivo (/api/copy)...")
    copy_dest = f"{dir_target}/test_stream_copied.bin"
    copy_payload = json.dumps({"src_path": file_target, "dest_path": copy_dest}).encode()
    code, hdrs, body = http_post("/api/copy", copy_payload)
    test_assert("Copiar arquivo", code == 200, f"code={code}")

    # Teste 7: Mover / Renomear (/api/move)
    print("\n[7] Testando Mover Arquivo (/api/move)...")
    moved_dest = f"{sub_target}/moved_stream.bin"
    move_payload = json.dumps({"old_path": copy_dest, "new_path": moved_dest}).encode()
    code, hdrs, body = http_post("/api/move", move_payload)
    test_assert("Mover arquivo para subpasta", code == 200, f"code={code}")

    # Teste 8: Extração de Capa de Álbum (/api/cover)
    print("\n[8] Testando Endpoint de Capa de Álbum (/api/cover)...")
    # Sub-teste 8.1: Capa avulsa no diretório (cover.jpg)
    jpeg_dummy = b"\xff\xd8\xff\xe0\x00\x10JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00" + b"\x55" * 512 + b"\xff\xd9"
    cover_file_target = f"{dir_target}/cover.jpg"
    code, _, _ = http_put(f"/api/upload?path={urllib.parse.quote(cover_file_target)}", jpeg_dummy)
    test_assert("Upload de cover.jpg avulso", code == 200)

    # Requisita /api/cover usando o caminho de um arquivo dentro da mesma pasta
    cover_req_url = f"/api/cover?path={urllib.parse.quote(file_target)}"
    code, hdrs, body = http_get(cover_req_url)
    test_assert("Capa avulsa via /api/cover", code == 200, f"code={code}")
    test_assert("Content-Type image/jpeg", "image/jpeg" in hdrs.get("Content-Type", ""))
    test_assert("Magic JPEG bytes confere", body.startswith(b"\xff\xd8") and body.endswith(b"\xff\xd9"))

    # Sub-teste 8.2: Capa embutida em arquivo FLAC real (se disponível localmente)
    flac_sample_path = r"C:\Users\thall\Downloads\$uicideboy$ - Antarctica [Explicit] (2016)\01 - Antarctica [Explicit].flac"
    import os
    if os.path.exists(flac_sample_path):
        with open(flac_sample_path, "rb") as f:
            flac_header_data = f.read(90000) # primeiros 90KB contém bloco PICTURE completo de 79KB
        flac_upload_path = f"{dir_target}/sample_embedded.flac"
        code, _, _ = http_put(f"/api/upload?path={urllib.parse.quote(flac_upload_path)}", flac_header_data)
        if test_assert("Upload amostra FLAC com capa embutida", code == 200):
            code_cov, hdrs_cov, body_cov = http_get(f"/api/cover?path={urllib.parse.quote(flac_upload_path)}")
            test_assert("Capa embutida FLAC extraída via /api/cover", code_cov == 200, f"code={code_cov}")
            test_assert("Capa FLAC Content-Type image/jpeg", "image/jpeg" in hdrs_cov.get("Content-Type", ""))
            test_assert("Capa FLAC tamanho real (~79KB)", len(body_cov) > 70000, f"tam={len(body_cov)} B")
            test_assert("Capa FLAC bytes JPEG", body_cov.startswith(b"\xff\xd8"))

    # Sub-teste 8.3: Arquivo sem capa retorna 404 limpo
    code_none, _, _ = http_get(f"/api/cover?path={urllib.parse.quote('/non_existent_folder/track.mp3')}")
    test_assert("Arquivo sem capa retorna 404 limpo", code_none == 404, f"code={code_none}")

    # Teste 9: Pastas com Caracteres Especiais ($)
    print("\n[9] Testando Pastas com Caracteres Especiais ($uicideboy$)...")
    special_folder = f"{dir_target}/$uicideboy$ - Teste [Explicit]"
    code, _, _ = http_post("/api/mkdir", json.dumps({"path": special_folder}).encode())
    test_assert("Criar pasta com '$' no nome", code == 200)

    code, _, body = http_get(f"/api/list?path={urllib.parse.quote(special_folder)}")
    test_assert("Listar pasta com '$' no nome", code == 200)

    # Teste 10: Concorrência - Streaming de Áudio vs Navegação (/api/list)
    print("\n[10] Testando Concorrência (Reprodução vs Navegação no Cartão SD)...")
    audio_streaming_active = True
    stream_bytes_read = 0
    stream_errors = 0

    def background_streamer():
        nonlocal stream_bytes_read, stream_errors
        try:
            url = f"{BASE_URL}{down_url}"
            req = urllib.request.Request(url)
            with urllib.request.urlopen(req, timeout=10) as resp:
                while audio_streaming_active:
                    chunk = resp.read(1024)
                    if not chunk:
                        break
                    stream_bytes_read += len(chunk)
                    time.sleep(0.02) # Simula consumo de áudio em 44.1kHz ~44 KB/s
        except Exception:
            stream_errors += 1

    stream_thread = threading.Thread(target=background_streamer)
    stream_thread.daemon = True
    stream_thread.start()

    # Enquanto o áudio está tocando em segundo plano, faz requisições de listagem de pastas
    concurrent_list_ok = 0
    for i in range(5):
        t0 = time.time()
        c, _, _ = http_get("/api/list?path=/")
        lat = (time.time() - t0) * 1000
        if c == 200:
            concurrent_list_ok += 1
        if i == 2:
            capture_screen("04_during_concurrency")
        time.sleep(0.05)

    audio_streaming_active = False
    stream_thread.join(timeout=2)
    test_assert("Concorrência: Navegação (/api/list) respondeu 5/5 durante áudio", concurrent_list_ok == 5, f"{concurrent_list_ok}/5 OK")

    # Teste 11: Exclusão Limpa (/api/delete)
    print("\n[11] Limpando dados de teste (/api/delete)...")
    code, _, _ = http_delete(f"/api/delete?path={urllib.parse.quote(special_folder)}")
    test_assert("Excluir pasta com '$'", code == 200)

    code, _, _ = http_delete(f"/api/delete?path={urllib.parse.quote(cover_file_target)}")
    test_assert("Excluir cover.jpg de teste", code == 200)

    code, _, _ = http_delete(f"/api/delete?path={urllib.parse.quote(file_target)}")
    test_assert("Excluir arquivo de teste", code == 200)

    code, _, _ = http_delete(f"/api/delete?path={urllib.parse.quote(moved_dest)}")
    test_assert("Excluir arquivo movido", code == 200)

    code, _, _ = http_delete(f"/api/delete?path={urllib.parse.quote(sub_target)}")
    test_assert("Excluir subpasta", code == 200)

    code, _, _ = http_delete(f"/api/delete?path={urllib.parse.quote(dir_target)}")
    test_assert("Excluir diretório raiz de testes", code == 200)

    print("\n" + "=" * 50)
    print(f"RESULTADO FINAL: {PASS_COUNT} PASSOU | {FAIL_COUNT} FALHOU")
    print("=" * 50)
    return FAIL_COUNT == 0

if __name__ == "__main__":
    success = run_tests()
    sys.exit(0 if success else 1)
