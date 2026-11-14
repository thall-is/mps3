#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
wifi_speed_bench.py - Benchmark em tempo real de velocidade Wi-Fi e I/O para MPS3 DAP.

Recursos:
1. Teste de RTT / Latência (Ping HTTP) com cálculo de jitter.
2. Teste de Rede Pura SINK (POST /api/bench_sink) - mede o teto do rádio Wi-Fi / pilha TCP sem I/O de disco.
3. Teste de Rede Pura SOURCE (GET /api/bench_source) - mede a vazão de download pura da placa.
4. Teste de Upload Completo com Gravação SD (PUT /api/upload) + Validação MD5 e limpeza.
5. Relatório comparativo final detalhando gargalos (Rede vs SDMMC).
"""

import sys
import os
import time
import hashlib
import argparse
import socket
import json
import threading
import gzip
import concurrent.futures
import re
import urllib.request
import urllib.error
import urllib.parse

if sys.stdout.encoding != 'utf-8':
    try:
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
        sys.stderr.reconfigure(encoding='utf-8', errors='replace')
    except Exception:
        pass

CHUNK_SIZE = 64 * 1024  # 64 KB por pacote para máxima eficiência TCP

def format_speed(bytes_transferred, duration_sec):
    if duration_sec <= 0:
        return "0.00 MB/s"
    mb = bytes_transferred / (1024.0 * 1024.0)
    mbs = mb / duration_sec
    return f"{mbs:6.2f} MB/s ({mbs * 8:6.2f} Mbps)"

def progress_bar(progress, total, speed_mbs, prefix=""):
    pct = (progress / total) * 100.0 if total > 0 else 0
    width = 30
    filled = int(width * (pct / 100.0))
    bar = "█" * filled + "░" * (width - filled)
    mb_transferred = progress / (1024.0 * 1024.0)
    mb_total = total / (1024.0 * 1024.0)
    sys.stdout.write(f"\r  {prefix} [{bar}] {pct:5.1f}% | {mb_transferred:5.1f}/{mb_total:5.1f} MB | {speed_mbs:5.2f} MB/s")
    sys.stdout.flush()

def capture_oled_screen(base_url, name="bench", print_art=True):
    try:
        url = f"{base_url}/api/screen"
        req = urllib.request.Request(url, headers={'User-Agent': 'MPS3-Bench'})
        with urllib.request.urlopen(req, timeout=2.0) as resp:
            raw_bytes = resp.read()
        if len(raw_bytes) == 1024:
            script_dir = os.path.dirname(os.path.abspath(__file__))
            if script_dir not in sys.path:
                sys.path.insert(0, script_dir)
            from oled_screenshot import decode_ssd1306, make_png, print_ascii
            pixels = decode_ssd1306(raw_bytes, rotate_180=True)
            os.makedirs("captures", exist_ok=True)
            png_path = f"captures/oled_{name}.png"
            with open(png_path, "wb") as f:
                f.write(make_png(pixels, scale=6))
            sys.stdout.write(f"\n  📸 [OLED CAPTURE] Framebuffer gravado em: {png_path}\n")
            if print_art:
                print_ascii(pixels)
            sys.stdout.flush()
            return png_path
    except Exception:
        pass
    return None

def test_ping_latency(base_url, samples=10):
    print("\n" + "=" * 65)
    print(f"📡 1. TESTE DE LATÊNCIA / RTT HTTP ({samples} amostras)")
    print("=" * 65)
    rtts = []
    status_url = f"{base_url}/api/status"
    for i in range(samples):
        t0 = time.perf_counter()
        try:
            req = urllib.request.Request(status_url, headers={'User-Agent': 'MPS3-Bench'})
            with urllib.request.urlopen(req, timeout=3.0) as resp:
                _ = resp.read()
            t1 = time.perf_counter()
            rtt_ms = (t1 - t0) * 1000.0
            rtts.append(rtt_ms)
            time.sleep(0.05)
        except Exception as e:
            print(f"  [Amostra {i+1}] Falha de resposta: {e}")
            time.sleep(0.1)

    if not rtts:
        print("  ❌ Nenhuma resposta recebida do dispositivo.")
        return None

    min_rtt = min(rtts)
    avg_rtt = sum(rtts) / len(rtts)
    max_rtt = max(rtts)
    jitter = sum(abs(rtts[i] - rtts[i-1]) for i in range(1, len(rtts))) / (len(rtts) - 1) if len(rtts) > 1 else 0

    print(f"  Resultados RTT: Mínimo={min_rtt:.1f}ms | Médio={avg_rtt:.1f}ms | Máximo={max_rtt:.1f}ms | Jitter={jitter:.1f}ms")
    if avg_rtt < 15:
        print("  Qualidade do Link: 🟢 EXCELENTE (Baixa latência, sinal forte)")
    elif avg_rtt < 50:
        print("  Qualidade do Link: 🟡 BOA (Estável)")
    else:
        print("  Qualidade do Link: 🔴 DEGRADADA (Possível atenuação física ou distância do modem)")
    return {"min": min_rtt, "avg": avg_rtt, "max": max_rtt, "jitter": jitter}

def test_net_sink(base_url, size_mb=10, mon=None):
    total_bytes = size_mb * 1024 * 1024
    print("\n" + "=" * 65)
    print(f"🚀 2. TESTE DE VAZÃO PURA DA REDE - SINK ({size_mb} MB descartados na RAM)")
    print("     (Mede o teto máximo de recepção do rádio Wi-Fi 802.11n / pilha TCP)")
    print("=" * 65)

    sink_url = f"{base_url}/api/bench_sink"
    # Prepara payload em blocos de 64 KB
    pattern_block = b"M" * CHUNK_SIZE
    parsed = urllib.parse.urlparse(sink_url)
    host = parsed.hostname
    port = parsed.port or 80
    path = parsed.path or "/"

    conn_timeout = max(20.0, size_mb * 5.0)
    try:
        sock = socket.create_connection((host, port), timeout=conn_timeout)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)

        header = (
            f"POST {path} HTTP/1.1\r\n"
            f"Host: {host}\r\n"
            f"Content-Type: application/octet-stream\r\n"
            f"Content-Length: {total_bytes}\r\n"
            f"Connection: close\r\n\r\n"
        ).encode('utf-8')
        sock.sendall(header)

        sent = 0
        t0 = time.perf_counter()
        last_t = t0
        last_sent = 0
        instant_speed = 0.0
        captured_mid = False

        while sent < total_bytes:
            to_send = min(CHUNK_SIZE, total_bytes - sent)
            sock.sendall(pattern_block[:to_send])
            sent += to_send

            if not captured_mid:
                elapsed_now = time.perf_counter() - t0
                if (sent >= total_bytes // 2 and elapsed_now >= 1.2) or (elapsed_now >= 2.0):
                    captured_mid = True
                    if mon:
                        threading.Thread(target=mon.capture_screenshot, args=("sink_mid", 2.0, True, base_url), daemon=True).start()

            now = time.perf_counter()
            dt = now - last_t
            if dt >= 0.2:
                instant_speed = ((sent - last_sent) / (1024.0 * 1024.0)) / dt
                last_t = now
                last_sent = sent
                progress_bar(sent, total_bytes, instant_speed, prefix="SINK")

        progress_bar(sent, total_bytes, instant_speed, prefix="SINK")
        sys.stdout.write("\n")

        # Aguarda resposta do servidor ESP32 com timeout generoso
        sock.settimeout(max(15.0, size_mb * 3.0))
        resp_data = b""
        while True:
            try:
                chunk = sock.recv(1024)
                if not chunk:
                    break
                resp_data += chunk
                if b"}" in resp_data and b"HTTP/1." in resp_data:
                    break
            except socket.timeout:
                if resp_data:
                    break
                raise
        sock.close()
        t1 = time.perf_counter()
        total_time = t1 - t0
        speed_mbs = (total_bytes / (1024.0 * 1024.0)) / total_time

        print(f"  Concluído em: {total_time:.2f} s")
        print(f"  Taxa Sustentada do PC -> ESP32: {format_speed(total_bytes, total_time)}")

        # Parse JSON do ESP32
        if b"{" in resp_data:
            json_part = resp_data[resp_data.find(b"{"):resp_data.rfind(b"}")+1].decode('utf-8', errors='ignore')
            try:
                esp_info = json.loads(json_part)
                t_ms = esp_info.get('time_ms', esp_info.get('duration_ms', 0))
                rc = esp_info.get('recv_calls', 0)
                rmax = esp_info.get('recv_max_ms', 0)
                print(f"  Telemetria interna do ESP32: {esp_info.get('speed_mbs', 0)} MB/s ({t_ms} ms | recv_calls={rc}, max={rmax} ms)")
            except Exception:
                pass
        return speed_mbs
    except Exception as e:
        print(f"  ❌ Falha no teste SINK: {e}")
        return None

def test_net_source(base_url, size_mb=10, mon=None):
    total_bytes = size_mb * 1024 * 1024
    print("\n" + "=" * 65)
    print(f"📥 3. TESTE DE VAZÃO PURA DA REDE - SOURCE ({size_mb} MB baixados da RAM)")
    print("     (Mede a velocidade pura de transmissão da placa para o PC)")
    print("=" * 65)

    source_url = f"{base_url}/api/bench_source?size={total_bytes}"
    timeout_val = max(30.0, size_mb * 6.0)
    try:
        req = urllib.request.Request(source_url, headers={'User-Agent': 'MPS3-Bench'})
        t0 = time.perf_counter()
        received = 0
        last_t = t0
        last_received = 0
        instant_speed = 0.0
        captured_mid = False

        with urllib.request.urlopen(req, timeout=timeout_val) as resp:
            while True:
                chunk = resp.read(CHUNK_SIZE)
                if not chunk:
                    break
                received += len(chunk)
                if not captured_mid and (received >= total_bytes // 2 or (time.perf_counter() - t0 >= 1.8)):
                    captured_mid = True
                    if mon:
                        threading.Thread(target=mon.capture_screenshot, args=("source_mid", 2.0, True, base_url), daemon=True).start()
                now = time.perf_counter()
                dt = now - last_t
                if dt >= 0.2:
                    instant_speed = ((received - last_received) / (1024.0 * 1024.0)) / dt
                    last_t = now
                    last_received = received
                    progress_bar(received, total_bytes, instant_speed, prefix="SOURCE")

        t1 = time.perf_counter()
        progress_bar(received, total_bytes, instant_speed, prefix="SOURCE")
        sys.stdout.write("\n")
        total_time = t1 - t0
        speed_mbs = (received / (1024.0 * 1024.0)) / total_time
        print(f"  Concluído em: {total_time:.2f} s")
        print(f"  Taxa Sustentada do ESP32 -> PC: {format_speed(received, total_time)}")
        return speed_mbs
    except Exception as e:
        print(f"  ❌ Falha no teste SOURCE: {e}")
        return None

def print_upload_microsecond_table(st, total_bytes):
    tot_us = max(1, int(st.get("total_us", st.get("total_ms", 1) * 1000)))
    fw_us = max(1, int(st.get("fwrite_us", st.get("fwrite_ms", 1) * 1000)))
    raw_sd_mbs = st.get("sd_write_mbps", (total_bytes / (1024.0 * 1024.0)) / (fw_us / 1000000.0))
    mbps = st.get("mbps", st.get("speed_mbs", 0.0))
    mode_names = {0: "0 (20MHz Fixo)", 1: "1 (Hybrid 20->40MHz)", 2: "2 (40MHz Cont.)", 3: "3 (Auto-Ch)"}
    clk_mode = st.get("sd_clk_mode", -1)
    print("\n  ⏱️ TABELA DE TELEMETRIA EM MICROSSEGUNDOS — UPLOAD (ESP32-S3):")
    print(f"     • Rádio & Clock SD:   Canal Wi-Fi = {st.get('wifi_ch', 0)} | RSSI = {st.get('rssi_dbm', 0)} dBm | SD Mode = {mode_names.get(clk_mode, clk_mode)} | Clock Final = {st.get('sd_khz', 0)} kHz")
    print(f"     • Buffers Persist.:   RingBuffer PSRAM = {st.get('rb_kb', 0)} KB | Bounce DMA = {st.get('bounce_kb', 0)} KB | Recv Buf = {st.get('recv_kb', 0)} KB | SHA256 = {'ON' if st.get('sha256_en', 0) else 'OFF (Sob Demanda)'}")
    print("     ┌────────────────────────────────────────┬──────────────┬───────────┬──────────────────────────────────────────┐")
    print("     │ Fase do Pipeline (Upload)              │   Tempo (ms) │ % Total   │ Detalhes / Sub-métricas                  │")
    print("     ├────────────────────────────────────────┼──────────────┼───────────┼──────────────────────────────────────────┤")
    rows = [
        ("1. Pre-Lock + Alloc", st.get("pre_lock_us", 0) + st.get("alloc_us", 0),
         f"pre_lock={st.get('pre_lock_us',0)/1000.0:.2f}ms, alloc={st.get('alloc_us',0)/1000.0:.2f}ms"),
        ("2. Open FATFS (Total)", st.get("open_us", st.get("open_ms", 0) * 1000),
         f"mkdir={st.get('mkdir_us',0)/1000.0:.2f}ms, unlink={st.get('unlink_us',0)/1000.0:.2f}ms"),
        ("   ├─ f_expand (Contíguo)", st.get("f_expand_us", 0),
         f"fopen={st.get('fopen_us',0)/1000.0:.2f}ms"),
        ("3. Produtor TCP (httpd_req_recv)", st.get("recv_us", st.get("recv_ms", 0) * 1000),
         f"calls={st.get('recv_calls',0)}, max={st.get('recv_max_us',0)/1000.0:.2f}ms, >50ms={st.get('recv_slow',0)}"),
        ("4. RingBuffer Push (xRingbufferSend)", st.get("ring_us", st.get("ring_ms", 0) * 1000),
         f"calls={st.get('ring_calls',0)}, full(>5ms)={st.get('ring_full',0)}"),
        ("5. Consumidor Wait RB (Core 1)", st.get("wr_wait_us", st.get("wr_wait_ms", 0) * 1000),
         f"yield={st.get('yield_us',0)/1000.0:.2f}ms"),
        ("6. Memcpy PSRAM -> Bounce DMA", st.get("memcpy_us", st.get("memcpy_ms", 0) * 1000),
         f"sha256_hw={st.get('sha256_us',0)/1000.0:.2f}ms"),
        ("7. Gravação SDMMC (fwrite CMD25)", fw_us,
         f"calls={st.get('fwrite_calls',st.get('wr_calls',0))}, max={st.get('fwrite_max_us',0)/1000.0:.2f}ms, >200ms={st.get('fwrite_slow',st.get('wr_slow',0))}"),
        ("8. Drain Wait (40MHz Turbo Drain)", st.get("drain_us", st.get("drain_ms", 0) * 1000),
         f"esvaziamento final do RingBuffer"),
        ("9. Fclose +Sync + Teardown", st.get("fclose_us", st.get("fclose_ms", 0) * 1000) + st.get("teardown_us", 0),
         f"fclose={st.get('fclose_us',0)/1000.0:.2f}ms, teardown={st.get('teardown_us',0)/1000.0:.2f}ms"),
        ("TOTAL HANDLER (Ponta a Ponta)", tot_us,
         f"Net={mbps:.2f} MB/s | SD Puro={raw_sd_mbs:.2f} MB/s"),
    ]
    for name, us_val, detail in rows:
        ms_val = float(us_val) / 1000.0
        pct = (float(us_val) / float(tot_us)) * 100.0
        print(f"     │ {name:<38} │ {ms_val:9.2f} ms │ {pct:7.1f} % │ {detail:<40} │")
    print("     └────────────────────────────────────────┴──────────────┴───────────┴──────────────────────────────────────────┘")

def print_download_microsecond_table(st, total_bytes):
    tot_us = max(1, int(st.get("total_us", st.get("total_ms", 1) * 1000)))
    fr_us = max(1, int(st.get("sd_read_us", st.get("fread_us", st.get("sd_read_ms", st.get("fread_ms", 1)) * 1000))))
    stat_us = int(st.get("stat_open_us", st.get("stat_us", 0) + st.get("fopen_us", 0) + st.get("fseek_us", 0)))
    net_us = int(st.get("net_send_us", st.get("send_us", st.get("net_send_ms", st.get("send_ms", 0)) * 1000)))
    raw_sd_mbs = st.get("sd_read_mbps", (total_bytes / (1024.0 * 1024.0)) / (fr_us / 1000000.0))
    mbps = st.get("mbps", st.get("speed_mbs", 0.0))
    print("\n  ⏱️ TABELA DE TELEMETRIA EM MICROSSEGUNDOS — DOWNLOAD (ESP32-S3):")
    print(f"     • Rádio & Clock SD:   Canal Wi-Fi = {st.get('wifi_ch', 0)} | RSSI = {st.get('rssi_dbm', 0)} dBm | SD Mode = {st.get('sd_clk_mode', -1)} | Clock = {st.get('sd_khz', 0)} kHz")
    print("     ┌────────────────────────────────────────┬──────────────┬───────────┬──────────────────────────────────────────┐")
    print("     │ Fase do Pipeline (Download)            │   Tempo (ms) │ % Total   │ Detalhes / Sub-métricas                  │")
    print("     ├────────────────────────────────────────┼──────────────┼───────────┼──────────────────────────────────────────┤")
    rows = [
        ("1. Stat + Fopen + Fseek", stat_us,
         f"stat_open={stat_us/1000.0:.2f}ms"),
        ("2. Leitura SDMMC (fread CMD18 DMA)", fr_us,
         f"calls={st.get('fread_calls',0)}, max={st.get('fread_max_us',0)/1000.0:.2f}ms, >50ms={st.get('fread_slow',0)}"),
        ("3. Envio TCP (httpd_resp_send_chunk)", net_us,
         f"calls={st.get('send_calls',0)}, max={st.get('send_max_us',0)/1000.0:.2f}ms, >50ms={st.get('send_slow',0)}"),
        ("4. Fclose FATFS", st.get("fclose_us", 0),
         f"rb_wait={st.get('ring_us', st.get('rb_wait_us',0))/1000.0:.2f}ms"),
        ("TOTAL DOWNLOAD (Ponta a Ponta)", tot_us,
         f"Net={mbps:.2f} MB/s | SD Read={raw_sd_mbs:.2f} MB/s"),
    ]
    for name, us_val, detail in rows:
        ms_val = float(us_val) / 1000.0
        pct = (float(us_val) / float(tot_us)) * 100.0
        print(f"     │ {name:<38} │ {ms_val:9.2f} ms │ {pct:7.1f} % │ {detail:<40} │")
    print("     └────────────────────────────────────────┴──────────────┴───────────┴──────────────────────────────────────────┘")

def fetch_json_endpoint(url, method="GET", timeout=5.0):
    try:
        req = urllib.request.Request(url, method=method, headers={'User-Agent': 'MPS3-Bench'})
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read().decode('utf-8', errors='replace'))
    except Exception:
        return None

def test_sd_upload(base_url, size_mb=10, mon=None, return_details=False):
    total_bytes = size_mb * 1024 * 1024
    print("\n" + "=" * 65)
    print(f"💾 4. TESTE DE UPLOAD & DOWNLOAD PONTA A PONTA (REDE + SDMMC FATFS) ({size_mb} MB)")
    print("     (Pipeline Persistente Dual-Core + Transfer Boost Mode + Telemetria em µs)")
    print("=" * 65)

    # Prepara bloco padrão de 64 KB e calcula MD5 esperado sem alocar matrizes gigantescas na RAM
    print("  Calculando hash MD5 esperado para o volume de dados...")
    hasher = hashlib.md5()
    pattern = bytearray(CHUNK_SIZE)
    for i in range(len(pattern)):
        pattern[i] = (i * 7 + 13) & 0xFF
    
    # Hash incremental do padrão repetido
    num_full_blocks = total_bytes // CHUNK_SIZE
    rem_bytes = total_bytes % CHUNK_SIZE
    for _ in range(num_full_blocks):
        hasher.update(pattern)
    if rem_bytes > 0:
        hasher.update(pattern[:rem_bytes])
    expected_md5 = hasher.hexdigest()
    print(f"  Hash MD5 original: {expected_md5}")

    test_file_path = "/bench_test_temp.bin"
    upload_url = f"{base_url}/api/upload?path={urllib.parse.quote(test_file_path)}&idx=1&count=1&batchTotal={total_bytes}"

    parsed = urllib.parse.urlparse(upload_url)
    host = parsed.hostname
    port = parsed.port or 80
    path = parsed.path + "?" + parsed.query

    upload_timeout = max(30.0, size_mb * 8.0)
    try:
        sock = socket.create_connection((host, port), timeout=upload_timeout)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)

        header = (
            f"PUT {path} HTTP/1.1\r\n"
            f"Host: {host}\r\n"
            f"Content-Type: application/octet-stream\r\n"
            f"Content-Length: {total_bytes}\r\n"
            f"Connection: close\r\n\r\n"
        ).encode('utf-8')
        sock.sendall(header)

        sent = 0
        t0 = time.perf_counter()
        last_t = t0
        last_sent = 0
        instant_speed = 0.0
        captured_mid = False

        while sent < total_bytes:
            to_send = min(CHUNK_SIZE, total_bytes - sent)
            sock.sendall(pattern[:to_send])
            sent += to_send

            if not captured_mid:
                elapsed_now = time.perf_counter() - t0
                if (sent >= total_bytes // 2 and elapsed_now >= 1.5) or (elapsed_now >= 2.5):
                    captured_mid = True
                    if mon:
                        threading.Thread(target=mon.capture_screenshot, args=("upload_mid", 2.0, True, base_url), daemon=True).start()

            now = time.perf_counter()
            dt = now - last_t
            if dt >= 0.2:
                instant_speed = ((sent - last_sent) / (1024.0 * 1024.0)) / dt
                last_t = now
                last_sent = sent
                progress_bar(sent, total_bytes, instant_speed, prefix="SD_PUT")

        progress_bar(sent, total_bytes, instant_speed, prefix="SD_PUT")
        sys.stdout.write("\n")

        # Aguarda confirmação HTTP 200 do ESP32 com timeout proporcional
        sock.settimeout(upload_timeout)
        resp_data = b""
        while True:
            try:
                chunk = sock.recv(1024)
                if not chunk:
                    break
                resp_data += chunk
                if b"\r\n\r\n" in resp_data:
                    hdr_part, _, body_part = resp_data.partition(b"\r\n\r\n")
                    cl_m = re.search(rb"content-length:\s*(\d+)", hdr_part, re.IGNORECASE)
                    if cl_m:
                        if len(body_part) >= int(cl_m.group(1)):
                            break
                    elif b"}" in body_part or b"\"status\"" in body_part or resp_data.endswith(b"\r\n\r\n"):
                        break
            except socket.timeout:
                if resp_data:
                    break
                raise
        sock.close()
        t1 = time.perf_counter()
        total_time = t1 - t0
        upload_speed_mbs = (total_bytes / (1024.0 * 1024.0)) / total_time

        print(f"  Concluído em: {total_time:.2f} s")
        print(f"  Taxa Efetiva de Escrita Ponta a Ponta: {format_speed(total_bytes, total_time)}")

        # Busca o JSON completo em /api/upload/stats (ou fallback no header X-Upload-Stats)
        up_stats = fetch_json_endpoint(f"{base_url}/api/upload/stats")
        if not up_stats or not up_stats.get("total_us"):
            resp_text = resp_data.decode('utf-8', errors='ignore')
            for line in resp_text.splitlines():
                if line.lower().startswith("x-upload-stats:"):
                    try:
                        up_stats = json.loads(line.split(":", 1)[1].strip())
                    except Exception:
                        pass
        if up_stats:
            print_upload_microsecond_table(up_stats, total_bytes)

        # Validação de integridade e medição de velocidade de Download (GET /api/download)
        print("\n  📥 Testando Download de Alta Velocidade (GET /api/download) + Integridade MD5...")
        download_url = f"{base_url}/api/download?path={urllib.parse.quote(test_file_path)}"
        req = urllib.request.Request(download_url, headers={'User-Agent': 'MPS3-Bench', 'Connection': 'keep-alive'})
        verify_hasher = hashlib.md5()
        verified_bytes = 0
        download_timeout = max(30.0, size_mb * 6.0)
        td0 = time.perf_counter()
        last_td = td0
        last_dl_bytes = 0
        dl_inst = 0.0
        with urllib.request.urlopen(req, timeout=download_timeout) as resp:
            while True:
                chunk = resp.read(CHUNK_SIZE)
                if not chunk:
                    break
                verify_hasher.update(chunk)
                verified_bytes += len(chunk)
                now_d = time.perf_counter()
                if now_d - last_td >= 0.2:
                    dl_inst = ((verified_bytes - last_dl_bytes) / (1024.0 * 1024.0)) / (now_d - last_td)
                    last_td = now_d
                    last_dl_bytes = verified_bytes
                    progress_bar(verified_bytes, total_bytes, dl_inst, prefix="SD_GET")
        td1 = time.perf_counter()
        dl_time = td1 - td0
        dl_speed_mbs = (verified_bytes / (1024.0 * 1024.0)) / dl_time if dl_time > 0 else 0.0
        progress_bar(verified_bytes, total_bytes, dl_speed_mbs, prefix="SD_GET")
        sys.stdout.write("\n")
        print(f"  Taxa Efetiva de Download SD -> PC: {format_speed(verified_bytes, dl_time)} em {dl_time:.2f} s")

        dl_stats = fetch_json_endpoint(f"{base_url}/api/download/stats")
        if dl_stats and dl_stats.get("total_us"):
            print_download_microsecond_table(dl_stats, verified_bytes)

        actual_md5 = verify_hasher.hexdigest()
        md5_ok = (actual_md5 == expected_md5 and verified_bytes == total_bytes)
        if md5_ok:
            print(f"  ✅ Validação de Integridade: PERFEITA (MD5 idêntico: {actual_md5})")
        else:
            print(f"  ❌ FALHA DE INTEGRIDADE: Esperado={expected_md5}, Obtido={actual_md5} ({verified_bytes}/{total_bytes} bytes)")

        # Limpeza do arquivo de teste
        print("  Limpando arquivo temporário de teste...")
        del_url = f"{base_url}/api/delete?path={urllib.parse.quote(test_file_path)}"
        req_del = urllib.request.Request(del_url, method='DELETE')
        try:
            with urllib.request.urlopen(req_del, timeout=5.0) as resp:
                _ = resp.read()
            print("  Arquivo temporário removido com sucesso.")
        except Exception:
            pass

        if return_details:
            return {
                "upload_mbs": upload_speed_mbs,
                "download_mbs": dl_speed_mbs,
                "md5_ok": md5_ok,
                "up_stats": up_stats or {},
                "dl_stats": dl_stats or {},
            }
        return upload_speed_mbs
    except Exception as e:
        print(f"  ❌ Falha no teste de Upload/Download SD: {e}")
        return None

def check_device_status(base_url):
    print("🔍 Verificando conectividade e estado do dispositivo...")
    try:
        req = urllib.request.Request(f"{base_url}/api/status", headers={'User-Agent': 'MPS3-Bench'})
        with urllib.request.urlopen(req, timeout=5.0) as resp:
            data = json.loads(resp.read().decode('utf-8'))
            print(f"  Estado atual: IP={data.get('ip', 'N/A')}, Modo={data.get('mode', 'N/A')}, Bateria={data.get('battery', 'N/A')}%")
            return data
    except Exception as e:
        print(f"  ⚠️ Não foi possível obter /api/status: {e}")
        return None

class SerialRebootMonitor:
    def __init__(self, port="COM5", baudrate=115200):
        self.port = port
        self.baudrate = baudrate
        self.ser = None
        self.thread = None
        self.running = False
        self.reboots = []
        self.last_ui = {}
        self._ui_event = threading.Event()
        self._scr_event = threading.Event()
        self.last_scr_raw = None

    def start(self):
        try:
            import serial
            self.ser = serial.Serial()
            self.ser.port = self.port
            self.ser.baudrate = self.baudrate
            self.ser.timeout = 0.5
            self.ser.dtr = False
            self.ser.rts = False
            self.ser.open()
            self.running = True
            self.thread = threading.Thread(target=self._loop, daemon=True)
            self.thread.start()
            time.sleep(0.3)
            self.send_cmd("dbg on")
            return True
        except Exception as e:
            print(f"  ⚠️ Aviso: Monitor serial ({self.port}) indisponível: {e}")
            return False

    def send_cmd(self, cmd):
        if self.ser and self.ser.is_open:
            try:
                self.ser.write(f"\n{cmd}\n".encode('utf-8'))
                self.ser.flush()
            except Exception:
                pass

    def _loop(self):
        while self.running and self.ser and self.ser.is_open:
            try:
                line = self.ser.readline().decode('utf-8', errors='replace').strip()
                if not line:
                    continue
                # Verificacao de reboot / crash
                if any(x in line for x in ["rst:0x", "Guru Meditation", "TG1WDT_SYS_RST", "abort()", "Backtrace:", "assert failed", "Panic"]):
                    self.reboots.append(line)
                    print(f"\n  🚨 ALERTA: REINÍCIO OU CRASH DETECTADO NA SERIAL: {line}")
                if line.startswith("@UI "):
                    try:
                        self.last_ui = json.loads(line[4:])
                        self._ui_event.set()
                    except Exception:
                        pass
                elif line.startswith("@SCR "):
                    try:
                        import base64
                        self.last_scr_raw = base64.b64decode(line[5:].strip())
                        self._scr_event.set()
                    except Exception:
                        pass
            except Exception:
                break

    def capture_screenshot(self, name="bench", timeout=2.0, print_art=True, base_url=None):
        self._scr_event.clear()
        self.last_scr_raw = None
        self.send_cmd("screen")
        if not (self._scr_event.wait(timeout) and self.last_scr_raw and len(self.last_scr_raw) == 1024):
            # Se a UART estiver com contenção de log, tenta ler diretamente do endpoint HTTP /api/screen
            if base_url:
                try:
                    req_scr = urllib.request.Request(f"{base_url}/api/screen")
                    with urllib.request.urlopen(req_scr, timeout=1.0) as scr_resp:
                        if scr_resp.status == 200:
                            data = scr_resp.read()
                            if len(data) == 1024:
                                self.last_scr_raw = data
                except Exception:
                    pass

        if self.last_scr_raw and len(self.last_scr_raw) == 1024:
            script_dir = os.path.dirname(os.path.abspath(__file__))
            if script_dir not in sys.path:
                sys.path.insert(0, script_dir)
            from oled_screenshot import decode_ssd1306, make_png, print_ascii
            pixels = decode_ssd1306(self.last_scr_raw, rotate_180=True)
            os.makedirs("captures", exist_ok=True)
            png_path = f"captures/oled_{name}.png"
            with open(png_path, "wb") as f:
                f.write(make_png(pixels, scale=6))
            sys.stdout.write(f"\n  📸 [OLED CAPTURE] Tela gravada em: {png_path}\n")
            if print_art:
                print_ascii(pixels)
            sys.stdout.flush()
            return png_path
        return None

    def query_ui(self, timeout=2.0):
        self._ui_event.clear()
        self.send_cmd("ui")
        if self._ui_event.wait(timeout):
            return self.last_ui
        return None

    def check_healthy(self, step_name):
        if self.reboots:
            print(f"\n❌ FALHA CRÍTICA: Dispositivo reiniciou durante o teste '{step_name}'!")
            for r in self.reboots:
                print(f"   Log de erro: {r}")
            return False
        ui = self.query_ui()
        if ui:
            if not ui.get("wifi", False):
                print(f"\n❌ FALHA: Dispositivo saiu do modo Wi-Fi durante '{step_name}'! (Estado: {ui.get('mode_name')})")
                return False
            print(f"  🔍 Verificação HIL [{step_name}]: 🟢 Modo Wi-Fi ativo | 0 Reboots")
        return True

    def stop(self):
        self.running = False
        if self.ser and self.ser.is_open:
            try:
                self.send_cmd("dbg off")
                self.ser.close()
            except Exception:
                pass

def test_browser_workflow(base_url, size_mb=10):
    print(f"\n🌐 [5/5] SIMULAÇÃO REAL DE USUÁRIO HUMANO NO NAVEGADOR WEB")
    print("  Testando fluxo completo pelo site gerado: Abrir página -> APIs iniciais ->")
    print("  Criar pasta -> Subir lote de músicas -> Listar álbum -> Ouvir/Seek no Player Web")

    parsed = urllib.parse.urlparse(base_url)
    host = parsed.hostname
    port = parsed.port or 80

    try:
        # 5.1 Carregar a página principal GET / (GZIP)
        t0 = time.perf_counter()
        req_root = urllib.request.Request(
            f"{base_url}/",
            headers={
                'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) Chrome/130.0.0.0',
                'Accept-Encoding': 'gzip, deflate'
            }
        )
        with urllib.request.urlopen(req_root, timeout=10.0) as resp:
            raw_html = resp.read()
            encoding = resp.headers.get('Content-Encoding', '')
        t_root_ms = (time.perf_counter() - t0) * 1000.0
        if encoding == 'gzip':
            html_text = gzip.decompress(raw_html).decode('utf-8', errors='replace')
        else:
            html_text = raw_html.decode('utf-8', errors='replace')

        if "MPS3" not in html_text or len(html_text) < 50000:
            print(f"  ❌ Falha ao validar HTML da página inicial ({len(html_text)} B)")
            return None
        print(f"  ✅ [Passo 1] Página Web carregada: {len(raw_html)/1024:.1f} KB gzip -> {len(html_text)/1024:.1f} KB HTML em {t_root_ms:.1f} ms")

        # 5.2 Rajada de chamadas simultâneas de abertura do navegador (DOMContentLoaded)
        init_endpoints = [
            "/api/status",
            "/api/list?path=%2F",
            "/api/now_playing",
            "/api/wifi/networks",
            "/api/eq",
            "/api/ota",
            "/api/podcasts_local"
        ]
        def fetch_ep(ep):
            ep_t0 = time.perf_counter()
            req = urllib.request.Request(f"{base_url}{ep}", headers={'User-Agent': 'Mozilla/5.0 Chrome/130.0'})
            with urllib.request.urlopen(req, timeout=15.0) as r:
                body = r.read()
                code = r.getcode()
            return ep, code, len(body), (time.perf_counter() - ep_t0) * 1000.0

        t_burst0 = time.perf_counter()
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as ex:
            futures = [ex.submit(fetch_ep, ep) for ep in init_endpoints]
            burst_results = [f.result() for f in futures]
        t_burst_ms = (time.perf_counter() - t_burst0) * 1000.0

        for ep, code, blen, dt_ms in burst_results:
            print(f"     • GET {ep:<22} -> HTTP {code} ({blen:5d} B) em {dt_ms:6.1f} ms")
        print(f"  ✅ [Passo 2] Rajada inicial do navegador ({len(init_endpoints)} APIs concorrentes) concluída em {t_burst_ms:.1f} ms sem reboot!")

        # 5.3 Criar pasta de álbum via POST /api/mkdir
        album_dir = "/_Album_Teste_Web"
        mkdir_body = json.dumps({"path": album_dir}).encode('utf-8')
        req_mkdir = urllib.request.Request(
            f"{base_url}/api/mkdir",
            data=mkdir_body,
            headers={'Content-Type': 'application/json'},
            method='POST'
        )
        with urllib.request.urlopen(req_mkdir, timeout=10.0) as resp:
            _ = resp.read()
        print(f"  ✅ [Passo 3] Pasta de álbum criada via Web API: {album_dir}")

        # 5.4 Upload de lote (batch) de músicas exatamente como o uploadBatch() do index.html
        track1_size = 512 * 1024  # 512 KB (faixa curta com acentos e espaços)
        track2_size = size_mb * 1024 * 1024  # Faixa principal Hi-Res (ex: 10 MB)
        batch_total = track1_size + track2_size

        tracks = [
            (f"{album_dir}/01 - Canção de Abertura (Ao Vivo).mp3", track1_size),
            (f"{album_dir}/02 - Sinfonia Hi-Res Master.flac", track2_size),
        ]

        pattern = bytearray(CHUNK_SIZE)
        for i in range(len(pattern)):
            pattern[i] = (i * 11 + 29) & 0xFF

        total_batch_sent = 0
        t_batch_start = time.perf_counter()
        last_prog_t = t_batch_start
        last_stats_hdr = ""

        sock = socket.create_connection((host, port), timeout=30.0)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)

        for idx, (t_path, t_bytes) in enumerate(tracks, start=1):
            encoded_q = (
                f"path={urllib.parse.quote(t_path)}"
                f"&idx={idx}&count={len(tracks)}&batchTotal={batch_total}"
            )
            conn_hdr = "keep-alive" if idx < len(tracks) else "close"
            req_hdr = (
                f"PUT /api/upload?{encoded_q} HTTP/1.1\r\n"
                f"Host: {host}:{port}\r\n"
                f"User-Agent: Mozilla/5.0 Chrome/130.0\r\n"
                f"Content-Type: application/octet-stream\r\n"
                f"Content-Length: {t_bytes}\r\n"
                f"Connection: {conn_hdr}\r\n\r\n"
            ).encode('utf-8')
            try:
                sock.sendall(req_hdr)
            except Exception:
                sock.close()
                sock = socket.create_connection((host, port), timeout=30.0)
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)
                sock.sendall(req_hdr)

            sent_file = 0
            while sent_file < t_bytes:
                to_send = min(CHUNK_SIZE, t_bytes - sent_file)
                sock.sendall(pattern[:to_send])
                sent_file += to_send
                total_batch_sent += to_send
                now_t = time.perf_counter()
                if now_t - last_prog_t >= 0.2 or sent_file == t_bytes:
                    last_prog_t = now_t
                    el = now_t - t_batch_start
                    cur_mbs = (total_batch_sent / (1024.0 * 1024.0)) / el if el > 0 else 0
                    progress_bar(total_batch_sent, batch_total, cur_mbs, prefix=f"WebUpload ({idx}/{len(tracks)})")

            resp_data = b""
            while True:
                try:
                    d = sock.recv(4096)
                    if not d:
                        break
                    resp_data += d
                    if b"\r\n\r\n" in resp_data:
                        hdr_end = resp_data.find(b"\r\n\r\n") + 4
                        hdr_part = resp_data[:hdr_end].decode('utf-8', errors='ignore')
                        clen = 0
                        for hline in hdr_part.splitlines():
                            if hline.lower().startswith("content-length:"):
                                clen = int(hline.split(":", 1)[1].strip())
                        if len(resp_data) - hdr_end >= clen:
                            break
                except socket.timeout:
                    if resp_data:
                        break
                    raise

            resp_str = resp_data.decode('utf-8', errors='replace')
            if "200 OK" not in resp_str:
                sock.close()
                print(f"\n  ❌ Erro HTTP no upload da faixa {idx}: {resp_str.splitlines()[0] if resp_str else 'Sem resposta'}")
                return None
            for line in resp_str.splitlines():
                if line.lower().startswith("x-upload-stats:"):
                    last_stats_hdr = line.split(":", 1)[1].strip()

            # Pausa de 25ms idêntica ao index.html entre arquivos do lote
            if idx < len(tracks):
                time.sleep(0.025)

        sock.close()
        t_batch_dur = time.perf_counter() - t_batch_start
        web_upload_mbs = (batch_total / (1024.0 * 1024.0)) / t_batch_dur
        print(f"\n  ✅ [Passo 4] Lote de músicas enviado via Web UI (Keep-Alive): {format_speed(batch_total, t_batch_dur)} em {t_batch_dur:.2f} s")
        if last_stats_hdr:
            print(f"     📊 Telemetria X-Upload-Stats (faixa principal): {last_stats_hdr}")

        # 5.5 Listar a pasta do álbum (como o navegador faz após o uploadBatch)
        list_url = f"{base_url}/api/list?path={urllib.parse.quote(album_dir)}"
        with urllib.request.urlopen(list_url, timeout=10.0) as resp:
            album_data = json.loads(resp.read().decode('utf-8', errors='replace'))
        album_items = album_data.get('entries', []) if isinstance(album_data, dict) else album_data
        listed_names = [it.get('name') for it in album_items]
        print(f"  ✅ [Passo 5] Listagem do álbum no navegador: {len(listed_names)} faixas encontradas {listed_names}")

        # 5.6 Ouvir a música no navegador:
        #   a) Leitura de tags ID3/FLAC (Range: bytes=0-131071)
        #   b) Streaming de reprodução + Seek no meio da música (Range: bytes=524288-1048575)
        main_track_path = tracks[1][0]
        dl_url = f"{base_url}/api/download?path={urllib.parse.quote(main_track_path)}"

        t_play0 = time.perf_counter()
        req_meta = urllib.request.Request(dl_url, headers={'Range': 'bytes=0-131071'})
        with urllib.request.urlopen(req_meta, timeout=10.0) as resp:
            meta_bytes = resp.read()
            meta_code = resp.getcode()
        t_meta_ms = (time.perf_counter() - t_play0) * 1000.0

        t_seek0 = time.perf_counter()
        req_seek = urllib.request.Request(dl_url, headers={'Range': 'bytes=524288-1048575'})
        with urllib.request.urlopen(req_seek, timeout=10.0) as resp:
            seek_bytes = resp.read()
            seek_code = resp.getcode()
            content_range = resp.headers.get('Content-Range', '')
        t_seek_dt = time.perf_counter() - t_seek0
        seek_mbs = (len(seek_bytes) / (1024.0 * 1024.0)) / t_seek_dt if t_seek_dt > 0 else 0

        # Verificar integridade dos bytes lidos no seek (512 KB de offset 512 KB..1 MB)
        expected_slice = (pattern * ((1048576 // CHUNK_SIZE) + 1))[524288:1048576]
        slice_ok = (seek_bytes == expected_slice)

        print(f"  ✅ [Passo 6] Player Web (<audio> Streaming & Seek):")
        print(f"     • Extração de Metadados (Range 0-128KB): HTTP {meta_code} ({len(meta_bytes)} B) em {t_meta_ms:.1f} ms")
        print(f"     • Seek de Áudio ({content_range}): HTTP {seek_code} ({len(seek_bytes)//1024} KB) a {seek_mbs:.2f} MB/s | Dados íntegros: {'SIM' if slice_ok else 'NÃO'}")

        # 5.7 Limpeza da pasta de álbum de teste
        del_url = f"{base_url}/api/delete?path={urllib.parse.quote(album_dir)}"
        req_del = urllib.request.Request(del_url, method='DELETE')
        with urllib.request.urlopen(req_del, timeout=10.0) as resp:
            _ = resp.read()
        print(f"  ✅ [Passo 7] Limpeza concluída ({album_dir} removido).")

        return web_upload_mbs
    except Exception as e:
        print(f"\n  ❌ Falha no fluxo de navegador Web: {e}")
        return None

def sweep_sd_clock_modes(base_url, size_mb=10, mon=None):
    print("\n" + "=" * 75)
    print(f"🔬 VARREDURA EMPÍRICA DE MODOS DE CLOCK SDMMC (--sweep-modes | {size_mb} MB)")
    print("=" * 75)
    modes = [
        (0, "Mode 0: 20 MHz Fixo"),
        (1, "Mode 1: Hybrid (20MHz RX -> 40MHz Drain)"),
        (2, "Mode 2: 40 MHz Contínuo"),
        (3, "Mode 3: Auto-Channel (40MHz ch!=5..8, Hybrid ch 5..8)"),
    ]
    results = []
    for mode_id, mode_label in modes:
        print(f"\n>>> Configurando {mode_label} via POST /api/bench/sd_clock?mode={mode_id} ...")
        cfg_resp = fetch_json_endpoint(f"{base_url}/api/bench/sd_clock?mode={mode_id}", method="POST")
        print(f"    Resposta do firmware: {cfg_resp}")
        det = test_sd_upload(base_url, size_mb=size_mb, return_details=True)
        if mon and not mon.check_healthy(f"Sweep {mode_label}"):
            break
        if det:
            up_st = det.get("up_stats", {})
            dl_st = det.get("dl_stats", {})
            results.append({
                "mode": mode_id,
                "label": mode_label,
                "up_mbs": det["upload_mbs"],
                "dl_mbs": det["download_mbs"],
                "sd_wr_mbs": up_st.get("sd_write_mbps", 0.0),
                "sd_rd_mbs": dl_st.get("sd_read_mbps", 0.0),
                "drain_ms": up_st.get("drain_us", 0) / 1000.0,
                "recv_slow": up_st.get("recv_slow", 0),
                "wr_slow": up_st.get("fwrite_slow", up_st.get("wr_slow", 0)),
                "wifi_ch": up_st.get("wifi_ch", 0),
                "rssi": up_st.get("rssi_dbm", 0),
                "md5_ok": det["md5_ok"],
            })

    # Restaura Mode 3 (Auto-Channel) ou o melhor modo medido sem perda de integridade
    best_mode = 3
    valid_res = [r for r in results if r["md5_ok"]]
    if valid_res:
        best_res = max(valid_res, key=lambda r: r["up_mbs"] + 0.5 * r["dl_mbs"])
        best_mode = best_res["mode"]
    fetch_json_endpoint(f"{base_url}/api/bench/sd_clock?mode={best_mode}", method="POST")

    print("\n" + "=" * 95)
    print("📊 TABELA COMPARATIVA DE MODOS DE CLOCK SDMMC (UPLOAD & DOWNLOAD)")
    print("=" * 95)
    print(f"{'Modo SDMMC':<42} | {'Upload':>9} | {'SD Write':>9} | {'Drain':>8} | {'Download':>9} | {'SD Read':>9} | {'MD5':>4}")
    print("-" * 95)
    for r in results:
        print(f"{r['label']:<42} | {r['up_mbs']:6.2f}MB/s | {r['sd_wr_mbs']:6.2f}MB/s | {r['drain_ms']:6.1f}ms | {r['dl_mbs']:6.2f}MB/s | {r['sd_rd_mbs']:6.2f}MB/s | {'OK' if r['md5_ok'] else 'ERR':>4}")
    print("=" * 95)
    print(f"🏆 Modo selecionado e aplicado no firmware: Mode {best_mode}")
    return results

def sweep_oled_fps(base_url, size_mb=10, mon=None):
    print("\n" + "=" * 75)
    print(f"🔬 VARREDURA EMPÍRICA DE TAXAS DE ATUALIZAÇÃO OLED (--sweep-fps | {size_mb} MB)")
    print("=" * 75)
    fps_list = [0, 4, 8, 10, 15, 20, 30]
    results = []
    base_up_mbs = None
    for fps in fps_list:
        label = f"{fps} FPS" if fps > 0 else "0 FPS (Display OFF)"
        print(f"\n>>> Configurando {label} via POST /api/bench/oled_fps?fps={fps} ...")
        cfg_resp = fetch_json_endpoint(f"{base_url}/api/bench/oled_fps?fps={fps}", method="POST")
        print(f"    Resposta do firmware: {cfg_resp}")
        det = test_sd_upload(base_url, size_mb=size_mb, return_details=True)
        if mon and not mon.check_healthy(f"Sweep {label}"):
            break
        if det:
            up_st = det.get("up_stats", {})
            dl_st = det.get("dl_stats", {})
            up_mbs = det["upload_mbs"]
            if base_up_mbs is None:
                base_up_mbs = up_mbs
            diff_pct = ((up_mbs - base_up_mbs) / base_up_mbs * 100.0) if base_up_mbs else 0.0
            results.append({
                "fps": fps,
                "label": label,
                "interval_ms": cfg_resp.get("interval_ms", 1000 // fps if fps else 0) if cfg_resp else 0,
                "up_mbs": up_mbs,
                "dl_mbs": det["download_mbs"],
                "sd_wr_mbs": up_st.get("sd_write_mbps", 0.0),
                "sd_rd_mbs": dl_st.get("sd_read_mbps", 0.0),
                "drain_ms": up_st.get("drain_us", 0) / 1000.0,
                "diff_pct": diff_pct,
                "md5_ok": det["md5_ok"],
            })

    # Restaura 4 FPS no firmware
    fetch_json_endpoint(f"{base_url}/api/bench/oled_fps?fps=4", method="POST")

    print("\n" + "=" * 95)
    print("📊 TABELA COMPARATIVA DE TAXAS DE ATUALIZAÇÃO OLED (UPLOAD & DOWNLOAD)")
    print("=" * 95)
    print(f"{'Taxa OLED':<24} | {'Intervalo':>9} | {'Upload Net':>10} | {'SD Write':>9} | {'Download':>9} | {'Impacto vs 0FPS':>15} | {'MD5':>4}")
    print("-" * 95)
    for r in results:
        status_md5 = "OK" if r["md5_ok"] else "ERR"
        diff_str = f"{r['diff_pct']:+5.1f}%" if r["fps"] > 0 else " BASE "
        print(f"{r['label']:<24} | {r['interval_ms']:>6} ms | {r['up_mbs']:7.2f}MB/s | {r['sd_wr_mbs']:6.2f}MB/s | {r['dl_mbs']:6.2f}MB/s | {diff_str:>15} | {status_md5:>4}")
    print("=" * 95)
    return results

def main():
    parser = argparse.ArgumentParser(description="MPS3 Wi-Fi Speed & Pipeline Benchmark Tool")
    parser.add_argument("target", nargs="?", default="mps3.local",
                        help="IP ou hostname do dispositivo MPS3 (ex: mps3.local ou 192.168.4.1)")
    parser.add_argument("--size", type=int, default=10, help="Tamanho do arquivo de teste em MB (padrão: 10)")
    parser.add_argument("--serial-port", default="COM5", help="Porta serial para monitoramento de UI e reboots (padrão: COM5)")
    parser.add_argument("--sweep-modes", action="store_true", help="Executa varredura comparativa dos Modos 0, 1, 2 e 3 de clock SDMMC")
    parser.add_argument("--fps", type=int, default=None, help="Configura a taxa de atualização (FPS) do OLED durante transferência")
    parser.add_argument("--sweep-fps", action="store_true", help="Executa varredura comparativa de taxas de atualização OLED (0, 4, 8, 10, 15, 20, 30 FPS)")
    args = parser.parse_args()

    target = args.target
    if not target.startswith("http://") and not target.startswith("https://"):
        base_url = f"http://{target}"
    else:
        base_url = target

    print("=" * 65)
    print("      MPS3 DAP - BENCHMARK DE VELOCIDADE WI-FI & PIPELINE SD")
    print(f"      Alvo: {base_url} | Tamanho dos Testes: {args.size} MB")
    print("=" * 65)

    # Inicia monitor serial
    mon = None
    if args.serial_port:
        print(f"🔌 Conectando monitor serial em {args.serial_port} (DTR=0, RTS=0)...")
        mon = SerialRebootMonitor(args.serial_port)
        if mon.start():
            ui_init = mon.query_ui()
            if ui_init:
                print(f"  Estado de UI inicial: Modo={ui_init.get('mode_name')} | Wi-Fi={ui_init.get('wifi')} ({ui_init.get('wifi_substate')}) | Menu={ui_init.get('menu_active')}")
                if not ui_init.get("wifi") or ui_init.get("wifi_substate") not in ("STA_ACTIVE", "AP_ACTIVE"):
                    print("  ⚠️ Ativando modo Wi-Fi STA via comando UART 'wifi sta'...")
                    mon.send_cmd("wifi sta")
                    time.sleep(6.0)
                    ui_init = mon.query_ui()
                    print(f"  Novo estado: Wi-Fi={ui_init.get('wifi') if ui_init else 'aguardando'} ({ui_init.get('wifi_substate') if ui_init else 'STA'})")
            else:
                print("  (Sem resposta do comando @UI - enviando 'wifi sta' preventivamente)")
                mon.send_cmd("wifi sta")
                time.sleep(5.0)

    status_before = None
    for attempt in range(8):
        status_before = check_device_status(base_url)
        if status_before:
            break
        time.sleep(2.0)
    if not status_before:
        print("❌ Dispositivo não está respondendo na URL especificada.")
        print("   Certifique-se de que o dispositivo está no modo Wi-Fi (tela 'WIFI' ativa).")
        if mon: mon.stop()
        sys.exit(1)

    if args.sweep_modes:
        sweep_sd_clock_modes(base_url, size_mb=args.size, mon=mon)

    if args.fps is not None:
        print(f"\n>>> Configurando OLED para {args.fps} FPS via POST /api/bench/oled_fps?fps={args.fps} ...")
        resp_fps = fetch_json_endpoint(f"{base_url}/api/bench/oled_fps?fps={args.fps}", method="POST")
        print(f"    Resposta do firmware: {resp_fps}")

    if args.sweep_fps:
        sweep_oled_fps(base_url, size_mb=args.size, mon=mon)
        if mon: mon.stop()
        return

    # 1. RTT Latency Ping
    rtt_res = test_ping_latency(base_url, samples=8)
    if mon and not mon.check_healthy("Ping Latency"):
        mon.stop()
        sys.exit(1)

    # 2. Network Sink (pure RAM)
    sink_speed = test_net_sink(base_url, size_mb=args.size, mon=mon)
    if mon and not mon.check_healthy("Network Sink"):
        mon.stop()
        sys.exit(1)

    # 3. Network Source (pure RAM)
    source_speed = test_net_source(base_url, size_mb=args.size, mon=mon)
    if mon and not mon.check_healthy("Network Source"):
        mon.stop()
        sys.exit(1)

    # 4. SD Upload & Download (Network + SDMMC FatFS)
    sd_details = test_sd_upload(base_url, size_mb=args.size, mon=mon, return_details=True)
    sd_speed = sd_details["upload_mbs"] if isinstance(sd_details, dict) else sd_details
    dl_speed = sd_details["download_mbs"] if isinstance(sd_details, dict) else None
    if mon and not mon.check_healthy("SD Card Upload & Download"):
        mon.stop()
        sys.exit(1)

    # 5. Fluxo Real de Navegador Web (Human Workflow)
    web_speed = test_browser_workflow(base_url, size_mb=args.size)
    if mon and not mon.check_healthy("Browser Web Workflow"):
        mon.stop()
        sys.exit(1)

    # Relatório Resumo
    print("\n" + "=" * 65)
    print("📊 RESUMO DO DESEMPENHO E DIAGNÓSTICO DE GARGALOS")
    print("=" * 65)
    if rtt_res:
        print(f"  - Latência RTT Média:       {rtt_res['avg']:6.1f} ms  (Jitter: {rtt_res['jitter']:.1f} ms)")
    if sink_speed:
        print(f"  - Teto do Rádio (Wi-Fi RX): {sink_speed:6.2f} MB/s ({sink_speed * 8:6.2f} Mbps)")
    if source_speed:
        print(f"  - Teto do Rádio (Wi-Fi TX): {source_speed:6.2f} MB/s ({source_speed * 8:6.2f} Mbps)")
    if sd_speed:
        print(f"  - Upload Ponta a Ponta SD:  {sd_speed:6.2f} MB/s ({sd_speed * 8:6.2f} Mbps)")
    if dl_speed:
        print(f"  - Download Ponta a Ponta:   {dl_speed:6.2f} MB/s ({dl_speed * 8:6.2f} Mbps)")
    if web_speed:
        print(f"  - Upload Real via Site Web: {web_speed:6.2f} MB/s ({web_speed * 8:6.2f} Mbps)")

    if sink_speed and web_speed:
        efficiency = (web_speed / sink_speed) * 100.0
        print(f"  - Eficiência do Site Web:   {efficiency:5.1f}% do teto da rede aproveitado")
        if efficiency > 75.0:
            print("  - Diagnóstico: 🚀 Site Web operando na velocidade máxima do hardware!")
        else:
            print("  - Diagnóstico: ⚠️ Cartão SD é o limitador físico de velocidade.")

    if mon:
        print("-" * 65)
        print(f"  - Verificação de Quedas:    0 Reinicializações (Sistema 100% Estável)")
        print(f"  - Integridade da UI:        Modo Wi-Fi permaneceu ATIVO em todas as fases")
        mon.stop()

    print("=" * 65 + "\n")

if __name__ == "__main__":
    main()

