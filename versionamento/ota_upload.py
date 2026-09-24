#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ota_upload.py — Utilitario de Upload OTA para o MPS3 (ESP32-S3)
Transmite arquivos de firmware .bin via Wi-Fi (HTTP POST /api/ota),
com busca e vinculacao automatica via endereco MAC de hardware,
barra de progresso, validacao pre-upload, checagem de bateria
e confirmacao de comutacao de particao dual-bank apos reinicializacao.
"""

import sys
import os
import time
import json
import socket
import argparse
import subprocess
import re
from urllib.parse import urlparse
import http.client
from concurrent.futures import ThreadPoolExecutor

DEFAULT_TARGET_MAC = "28:84:85:52:35:84"
DEFAULT_AP_MAC = "28:84:85:52:35:85"
DEFAULT_PORT = 80
CHUNK_SIZE = 4096
ESP_IMAGE_MAGIC = 0xE9
ESP_APP_DESC_MAGIC = 0xABCD5432

CACHE_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".mps3_last_ip")


def normalize_mac(mac):
    """Normaliza endereco MAC para formato xx:xx:xx:xx:xx:xx em minusculas."""
    if not mac:
        return ""
    s = str(mac).strip().lower()
    parts = re.split(r"[:-]", s)
    if len(parts) == 6 and all(len(p) <= 2 and p for p in parts):
        return ":".join(p.zfill(2) for p in parts)
    clean = re.sub(r"[^0-9a-fA-F]", "", s)
    if len(clean) == 12:
        return ":".join(clean[i:i+2] for i in range(0, 12, 2))
    return s


def is_esp32_mac_pair(mac1, mac2):
    """Verifica se dois MACs correspondem ao par STA/AP de um ESP32 (diferem em +/-1 no ultimo byte)."""
    n1 = normalize_mac(mac1)
    n2 = normalize_mac(mac2)
    if not n1 or not n2:
        return False
    parts1 = n1.split(":")
    parts2 = n2.split(":")
    if len(parts1) != 6 or len(parts2) != 6:
        return False
    if parts1[:5] != parts2[:5]:
        return False
    try:
        b1 = int(parts1[5], 16)
        b2 = int(parts2[5], 16)
        return abs(b1 - b2) == 1
    except ValueError:
        return False


def matches_target_mac(mac_to_test, target_mac):
    """Compara dois enderecos MAC, aceitando correspondencia exata ou par STA/AP do ESP32."""
    if not mac_to_test or not target_mac:
        return False
    n_test = normalize_mac(mac_to_test)
    n_target = normalize_mac(target_mac)
    if n_test == n_target:
        return True
    default_sta = normalize_mac(DEFAULT_TARGET_MAC)
    default_ap = normalize_mac(DEFAULT_AP_MAC)
    if {n_test, n_target} == {default_sta, default_ap}:
        return True
    return is_esp32_mac_pair(n_test, n_target)


def get_cached_ip():
    """Recupera o ultimo IP salvo em cache local."""
    try:
        if os.path.isfile(CACHE_FILE):
            with open(CACHE_FILE, "r", encoding="utf-8") as f:
                ip = f.read().strip()
                if ip:
                    return ip
    except Exception:
        pass
    return None


def save_cached_ip(ip):
    """Salva IP confirmado em cache local para acelerar buscas futuras."""
    try:
        with open(CACHE_FILE, "w", encoding="utf-8") as f:
            f.write(ip.strip() + "\n")
    except Exception:
        pass


def get_arp_table():
    """Consulta a tabela ARP do sistema operacional (Windows/Linux) e retorna lista de (ip, mac)."""
    entries = []
    try:
        output = subprocess.check_output(["arp", "-a"], stderr=subprocess.DEVNULL, text=True, errors="ignore")
    except Exception:
        return entries

    ip_regex = re.compile(r"(\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}\b)")
    mac_regex = re.compile(r"([0-9a-fA-F]{2}[:-][0-9a-fA-F]{2}[:-][0-9a-fA-F]{2}[:-][0-9a-fA-F]{2}[:-][0-9a-fA-F]{2}[:-][0-9a-fA-F]{2})")

    for line in output.splitlines():
        ip_m = ip_regex.search(line)
        mac_m = mac_regex.search(line)
        if ip_m and mac_m:
            ip = ip_m.group(1)
            mac = normalize_mac(mac_m.group(1))
            entries.append((ip, mac))
    return entries


def get_local_subnets():
    """Detecta as sub-redes IPv4 locais ativas da maquina (ex: 192.168.1.)."""
    subnets = set()

    # 1. Socket UDP conectado para rota externa
    s = None
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        parts = ip.split(".")
        if len(parts) == 4 and not ip.startswith("127.") and not ip.startswith("169.254."):
            subnets.add(f"{parts[0]}.{parts[1]}.{parts[2]}.")
    except Exception:
        pass
    finally:
        if s:
            try:
                s.close()
            except Exception:
                pass

    # 2. Hostname local
    try:
        hostname = socket.gethostname()
        for info in socket.getaddrinfo(hostname, None, socket.AF_INET):
            ip = info[4][0]
            parts = ip.split(".")
            if len(parts) == 4 and not ip.startswith("127.") and not ip.startswith("169.254."):
                subnets.add(f"{parts[0]}.{parts[1]}.{parts[2]}.")
    except Exception:
        pass

    # 3. Interfaces identificadas pelo comando arp -a
    try:
        output = subprocess.check_output(["arp", "-a"], stderr=subprocess.DEVNULL, text=True, errors="ignore")
        for m in re.finditer(r"Interface:\s*(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})", output, re.IGNORECASE):
            ip = m.group(1)
            parts = ip.split(".")
            if len(parts) == 4 and not ip.startswith("127.") and not ip.startswith("169.254."):
                subnets.add(f"{parts[0]}.{parts[1]}.{parts[2]}.")
    except Exception:
        pass

    # 4. Interfaces identificadas via ip -4 addr show (Linux)
    try:
        output = subprocess.check_output(["ip", "-4", "addr", "show"], stderr=subprocess.DEVNULL, text=True, errors="ignore")
        for m in re.finditer(r"inet\s+(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})/\d+", output):
            ip = m.group(1)
            parts = ip.split(".")
            if len(parts) == 4 and not ip.startswith("127.") and not ip.startswith("169.254."):
                subnets.add(f"{parts[0]}.{parts[1]}.{parts[2]}.")
    except Exception:
        pass

    return sorted(list(subnets))


def _ping_single_host(ip, is_win):
    """Envia probe rapido (UDP + ping) para forcar o SO a resolver ARP."""
    s = None
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(0.01)
        s.sendto(b"", (ip, 80))
    except Exception:
        pass
    finally:
        if s:
            try:
                s.close()
            except Exception:
                pass

    cmd = ["ping", "-n", "1", "-w", "150", ip] if is_win else ["ping", "-c", "1", "-W", "1", ip]
    try:
        subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=0.4)
    except Exception:
        pass


def ping_sweep_subnet(subnet_prefix):
    """Dispara ping sweep multithread rapido numa sub-rede /24."""
    is_win = (sys.platform == "win32")
    ips = [f"{subnet_prefix}{i}" for i in range(1, 255)]
    with ThreadPoolExecutor(max_workers=60) as executor:
        list(executor.map(lambda ip: _ping_single_host(ip, is_win), ips))


def parse_target_host(target_str, default_port=DEFAULT_PORT):
    """Extrai host e porta de uma string de alvo (IP, host ou URL)."""
    if not target_str.startswith("http://") and not target_str.startswith("https://"):
        target_str = "http://" + target_str
    parsed = urlparse(target_str)
    host = parsed.hostname or "127.0.0.1"
    port = parsed.port or default_port
    return host, port


def validate_local_binary(bin_path):
    """Valida se o arquivo local e uma imagem ESP32 valida do projeto mps3."""
    if not os.path.isfile(bin_path):
        raise FileNotFoundError(f"Arquivo binario nao encontrado: {bin_path}")

    file_size = os.path.getsize(bin_path)
    if file_size < 288:
        raise ValueError(f"Arquivo muito pequeno ({file_size} bytes). Nao e um binario ESP32 valido.")

    with open(bin_path, "rb") as f:
        header = f.read(288)

    magic = header[0]
    if magic != ESP_IMAGE_MAGIC:
        raise ValueError(f"Cabecalho ESP32 invalido: 0x{magic:02X} (esperado 0x{ESP_IMAGE_MAGIC:02X})")

    # esp_app_desc_t no offset 0x20
    magic_word = int.from_bytes(header[0x20:0x24], "little")
    if magic_word != ESP_APP_DESC_MAGIC:
        raise ValueError(f"Magic word esp_app_desc invalido: 0x{magic_word:08X}")

    version = header[0x30:0x50].split(b"\x00")[0].decode("latin1", errors="ignore")
    project = header[0x50:0x70].split(b"\x00")[0].decode("latin1", errors="ignore")
    build_time = header[0x70:0x80].split(b"\x00")[0].decode("latin1", errors="ignore")
    build_date = header[0x80:0x90].split(b"\x00")[0].decode("latin1", errors="ignore")

    if project != "mps3":
        print(f"[!] AVISO: Projeto no cabecalho e '{project}' (esperado 'mps3'). Prosseguindo com cautela...")

    return {
        "size": file_size,
        "project": project,
        "version": version,
        "build_date": build_date,
        "build_time": build_time
    }


def query_device_ota(host, port=DEFAULT_PORT, timeout=2.0):
    """Consulta GET /api/ota do dispositivo e retorna os dados em dict, ou None se falhar."""
    conn = None
    try:
        conn = http.client.HTTPConnection(host, port, timeout=timeout)
        conn.request("GET", "/api/ota")
        resp = conn.getresponse()
        if resp.status == 200:
            data = resp.read().decode("utf-8", errors="ignore")
            return json.loads(data)
    except Exception:
        return None
    finally:
        if conn:
            conn.close()
    return None


def discover_device(target_arg=None, target_mac=DEFAULT_TARGET_MAC, port=DEFAULT_PORT, probe_timeout=1.5):
    """Algoritmo de busca em cascata: alvo explicito -> ARP -> ping sweep -> fallback -> cache."""
    # Passo 1: Alvo fornecido explicitamente na CLI
    if target_arg:
        h, p = parse_target_host(target_arg, port)
        print(f"[*] Destino fornecido: http://{h}:{p}")
        info = query_device_ota(h, p, timeout=3.0)
        if info:
            dev_mac = info.get("mac")
            if dev_mac:
                print(f"[+] Dispositivo localizado em {h} (MAC confirmado: {dev_mac})")
            else:
                print(f"[+] Dispositivo localizado em {h}")
            save_cached_ip(h)
            return h, p, info
        print(f"[!] ERRO: Nao foi possivel conectar a http://{h}:{p}/api/ota")
        return None, None, None

    print(f"[*] Procurando dispositivo com MAC {target_mac} na rede local...")

    # Passo 2: Consulta a tabela ARP do sistema (tenta todas as entradas compativeis)
    arp_entries = get_arp_table()
    for ip, mac in arp_entries:
        if matches_target_mac(mac, target_mac):
            print(f"[*] MAC encontrado no cache ARP local: {ip}. Validando resposta...")
            info = query_device_ota(ip, port, timeout=probe_timeout)
            if info:
                confirmed_mac = info.get("mac") or target_mac
                print(f"[+] Dispositivo localizado em {ip} (MAC confirmado: {confirmed_mac})")
                save_cached_ip(ip)
                return ip, port, info
            else:
                print(f"[!] Dispositivo em {ip} nao respondeu em /api/ota.")

    # Passo 3: Se nao achou no ARP, executa ping sweep rapido nas sub-redes ativas
    print("[*] MAC nao encontrado no cache ARP local. Disparando varredura rapida de rede...")
    subnets = get_local_subnets()
    for sub in subnets:
        print(f"    - Varrendo sub-rede {sub}0/24...")
        ping_sweep_subnet(sub)

    # Re-ler tabela ARP apos a varredura
    time.sleep(0.15)
    arp_entries = get_arp_table()
    for ip, mac in arp_entries:
        if matches_target_mac(mac, target_mac):
            print(f"[*] MAC localizado apos varredura: {ip}. Validando comunicacao...")
            info = query_device_ota(ip, port, timeout=probe_timeout)
            if info:
                confirmed_mac = info.get("mac") or target_mac
                print(f"[+] Dispositivo localizado em {ip} (MAC confirmado: {confirmed_mac})")
                save_cached_ip(ip)
                return ip, port, info
            else:
                print(f"[!] Dispositivo em {ip} nao respondeu em /api/ota.")

    # Passo 4: Rotas de contingencia (fallback na ordem especificada: mps3.local, 192.168.4.1, cache)
    print("[*] Tentando rotas de contingencia (fallback)...")
    fallback_candidates = ["mps3.local", "192.168.4.1"]
    cached_ip = get_cached_ip()
    if cached_ip:
        fallback_candidates.append(cached_ip)

    seen = set()
    for cand in fallback_candidates:
        if cand in seen:
            continue
        seen.add(cand)
        h, p = parse_target_host(cand, port)
        print(f"    - Testando {h}:{p} ...", end=" ", flush=True)
        info = query_device_ota(h, p, timeout=probe_timeout)
        if info:
            dev_mac = info.get("mac")
            if dev_mac and target_mac and not matches_target_mac(dev_mac, target_mac):
                print(f"respondendo, mas MAC ({dev_mac}) difere de ({target_mac})")
                continue
            print("OK!")
            confirmed_mac = dev_mac or target_mac
            print(f"[+] Dispositivo localizado em {h} (MAC confirmado: {confirmed_mac})")
            save_cached_ip(h)
            return h, p, info
        print("sem resposta")

    return None, None, None


def format_bytes(num_bytes):
    """Formata bytes para exibicao legivel."""
    if num_bytes < 1024:
        return f"{num_bytes} B"
    elif num_bytes < 1024 * 1024:
        return f"{num_bytes / 1024:.1f} KB"
    else:
        return f"{num_bytes / (1024 * 1024):.2f} MB"


def render_progress_bar(sent, total, start_time, bar_len=30):
    """Gera string de barra de progresso no terminal."""
    fraction = min(1.0, sent / total) if total > 0 else 0
    pct = fraction * 100.0
    filled = int(round(bar_len * fraction))
    bar = "=" * filled + (">" if filled < bar_len else "")
    bar = bar.ljust(bar_len)

    elapsed = time.time() - start_time
    speed = (sent / elapsed) if elapsed > 0 else 0  # bytes/s
    speed_kb = speed / 1024.0

    if speed > 0 and sent < total:
        eta_sec = int((total - sent) / speed)
        eta_str = f"{eta_sec // 60:02d}:{eta_sec % 60:02d}"
    else:
        eta_str = "00:00"

    sent_kb = sent / 1024.0
    total_kb = total / 1024.0

    line = f"\r  [{bar}] {pct:5.1f}% | {sent_kb:6.0f}/{total_kb:6.0f} KB | {speed_kb:5.1f} KB/s | ETA: {eta_str}"
    sys.stdout.write(line)
    sys.stdout.flush()


def upload_ota(bin_path, host, port=DEFAULT_PORT, timeout=60.0):
    """Realiza o streaming de upload via POST /api/ota."""
    file_size = os.path.getsize(bin_path)
    start_time = time.time()
    conn = None

    try:
        conn = http.client.HTTPConnection(host, port, timeout=timeout)
        conn.putrequest("POST", "/api/ota")
        conn.putheader("Content-Type", "application/octet-stream")
        conn.putheader("Content-Length", str(file_size))
        conn.putheader("User-Agent", "mps3-ota-uploader/1.0")
        conn.endheaders()

        sent = 0
        early_socket_err = None

        with open(bin_path, "rb") as f:
            while True:
                chunk = f.read(CHUNK_SIZE)
                if not chunk:
                    break
                try:
                    conn.send(chunk)
                except (ConnectionResetError, BrokenPipeError, socket.error) as se:
                    early_socket_err = se
                    break
                sent += len(chunk)
                render_progress_bar(sent, file_size, start_time)

        sys.stdout.write("\n")
        sys.stdout.flush()

        total_time = max(0.1, time.time() - start_time)
        avg_speed_kb = (sent / total_time) / 1024.0

        try:
            resp = conn.getresponse()
            body = resp.read().decode("utf-8", errors="ignore")
        except Exception as re:
            if early_socket_err:
                return False, f"Conexao interrompida pelo dispositivo ({early_socket_err})", total_time, avg_speed_kb
            return False, f"Erro ao receber resposta do dispositivo ({re})", total_time, avg_speed_kb

        if resp.status == 200:
            return True, body, total_time, avg_speed_kb
        else:
            err_msg = body
            try:
                err_json = json.loads(body)
                if "error" in err_json:
                    err_msg = err_json["error"]
            except Exception:
                pass
            return False, f"HTTP {resp.status}: {err_msg}", total_time, avg_speed_kb

    except Exception as e:
        total_time = max(0.1, time.time() - start_time)
        return False, f"Falha de comunicacao: {e}", total_time, 0.0
    finally:
        if conn:
            try:
                conn.close()
            except Exception:
                pass


def wait_for_reboot(host, port=DEFAULT_PORT, old_partition=None, max_wait=30):
    """Aguarda o ESP32 reiniciar e reconectar, confirmando a comutacao de particao."""
    print("\n[*] Aguardando reinicializacao do ESP32-S3 (8 a 15 segundos)...")
    time.sleep(6.0)  # Pausa inicial necessaria para o ESP32 descarregar tarefas e iniciar reset

    start_wait = time.time()
    attempt = 1
    spinner = ["|", "/", "-", "\\"]

    while (time.time() - start_wait) < max_wait:
        elapsed = int(time.time() - start_wait)
        char = spinner[attempt % len(spinner)]
        sys.stdout.write(f"\r  [{char}] Verificando retorno do dispositivo... ({elapsed}s decorridos)")
        sys.stdout.flush()

        info = query_device_ota(host, port, timeout=1.5)
        if info:
            new_part = info.get("running_partition", "desconhecida")
            new_ver = info.get("running_version", "desconhecida")

            # Se ainda estiver na mesma particao e decorreu pouco tempo, aguarda o reboot efetivar
            if old_partition and new_part == old_partition and elapsed < 10:
                time.sleep(1.0)
                attempt += 1
                continue

            sys.stdout.write("\r" + " " * 70 + "\r")
            sys.stdout.flush()

            print("==================================================")
            print("  DISPOSITIVO MPS3 ONLINE E OPERACIONAL!")
            print("==================================================")
            print(f"  Particao anterior: {old_partition or 'desconhecida'}")
            print(f"  Particao atual:    {new_part}")
            print(f"  Versao ativa:      {new_ver}")

            if old_partition and new_part != old_partition:
                print("  Status:            COMUTACAO BEM-SUCEDIDA (Dual-Bank OK)")
                print("==================================================")
                return True, info
            elif old_partition and new_part == old_partition:
                print("  [!] AVISO: Particao ativa nao mudou. Verifique possivel rollback.")
                print("==================================================")
                return False, info
            else:
                print("  Status:            DISPOSITIVO ATIVO")
                print("==================================================")
                return True, info

        time.sleep(1.0)
        attempt += 1

    sys.stdout.write("\r" + " " * 70 + "\r")
    print(f"[!] AVISO: Tempo limite de reconexao ({max_wait}s) esgotado.")
    print("    O firmware foi gravado, mas o dispositivo ainda nao respondeu via Wi-Fi.")
    return False, None


def main():
    parser = argparse.ArgumentParser(
        description="Utilitario de Upload OTA para o MPS3 (ESP32-S3)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""Exemplos:
  python ota_upload.py binarios/mps3_20260924_140150/mps3.bin
  python ota_upload.py binarios/mps3_20260924_140150/mps3.bin 192.168.1.100
  python ota_upload.py build/mps3.bin --mac 28:84:85:52:35:84
"""
    )
    parser.add_argument("binary", help="Caminho para o arquivo .bin do firmware")
    parser.add_argument("target", nargs="?", default=None, help="IP ou hostname do MPS3 (opcional; se omitido, busca automatica por MAC)")
    parser.add_argument("--mac", default=DEFAULT_TARGET_MAC, help=f"Endereco MAC alvo do dispositivo MPS3 (padrao: {DEFAULT_TARGET_MAC})")
    parser.add_argument("-p", "--port", type=int, default=DEFAULT_PORT, help=f"Porta HTTP (padrao: {DEFAULT_PORT})")
    parser.add_argument("-f", "--force", "-y", "--yes", action="store_true", help="Ignora avisos de bateria baixa sem pedir confirmacao interativa")
    parser.add_argument("-t", "--timeout", type=float, default=60.0, help="Timeout do upload em segundos (padrao: 60s)")
    parser.add_argument("--max-wait", type=int, default=45, help="Tempo maximo de espera pela reconexao pos-reboot em segundos (padrao: 45s)")

    args = parser.parse_args()

    # 1. Validar arquivo local
    try:
        bin_meta = validate_local_binary(args.binary)
    except Exception as e:
        print(f"[!] ERRO no arquivo binario: {e}")
        return 1

    # 2. Determinar destino (IP / Host) via busca em cascata ou alvo explicito
    host, port, device_info = discover_device(args.target, target_mac=args.mac, port=args.port)
    if not host or not device_info:
        print(f"[!] ERRO: Nenhum dispositivo MPS3 encontrado.")
        if not args.target:
            print(f"    MAC alvo: {args.mac}")
            print("    Certifique-se de que o MPS3 esta ligado e conectado a mesma rede Wi-Fi.")
            print("    Voce tambem pode informar o IP diretamente: python ota_upload.py <binario> <IP>")
        return 1

    # 3. Informacoes do dispositivo e checagens
    old_partition = device_info.get("running_partition", "desconhecida")
    next_partition = device_info.get("next_partition", "desconhecida")
    running_ver = device_info.get("running_version", "desconhecida")
    bat_pct = device_info.get("battery_percent", 0)
    charging = device_info.get("battery_charging", False)
    in_progress = device_info.get("ota_in_progress", False)
    device_mac = device_info.get("mac", "nao informado")

    print("==================================================")
    print("  STATUS DO DISPOSITIVO MPS3")
    print("==================================================")
    print(f"  Versao em execucao: {running_ver}")
    print(f"  Particao ativa:     {old_partition}")
    print(f"  Particao alvo:      {next_partition}")
    print(f"  Endereco MAC:       {device_mac}")
    print(f"  Nivel de bateria:   {bat_pct}% (Carregando: {'Sim' if charging else 'Nao'})")
    print("--------------------------------------------------")
    print(f"  Firmware local:     {os.path.basename(args.binary)}")
    print(f"  Versao do binario:  {bin_meta['version']}")
    print(f"  Tamanho:            {format_bytes(bin_meta['size'])}")
    print(f"  Compilado em:       {bin_meta['build_date']} {bin_meta['build_time']}")
    print("==================================================")

    if in_progress:
        print("[!] ERRO: Uma atualizacao OTA ja esta em andamento no dispositivo.")
        return 1

    # Checagem de bateria baixa (< 15% sem carregador)
    if bat_pct < 15 and not charging:
        print("\n[!] ALERTA CRITICO: Bateria muito baixa (< 15%) e sem carregador!")
        print("    O processo de gravacao na flash pode consumir bateria e desligar o aparelho.")
        if not args.force:
            try:
                resp = input("    Deseja continuar mesmo assim? [s/N]: ").strip().lower()
                if resp not in ["s", "sim", "y", "yes"]:
                    print("[*] Operacao cancelada pelo usuario.")
                    return 1
            except (KeyboardInterrupt, EOFError):
                print("\n[*] Cancelado.")
                return 1

    # 4. Upload OTA via streaming POST
    print(f"\n[*] Iniciando transmissao de firmware para http://{host}:{port}/api/ota ...")
    success, msg, total_time, avg_speed = upload_ota(args.binary, host, port, timeout=args.timeout)

    if not success:
        print(f"\n[!] Falha no upload OTA: {msg}")
        return 1

    print("==================================================")
    print("  UPLOAD CONCLUIDO COM SUCESSO!")
    print(f"  Tempo total: {total_time:.1f}s | Velocidade media: {avg_speed:.1f} KB/s")
    print("  O ESP32-S3 confirmou a gravacao e esta reiniciando...")
    print("==================================================")

    # 5. Polling de retorno e comutacao de particao
    reboot_ok, _ = wait_for_reboot(host, port, old_partition=old_partition, max_wait=args.max_wait)
    if not reboot_ok:
        return 2

    return 0


if __name__ == "__main__":
    sys.exit(main())
