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
import urllib.request
import urllib.error
import urllib.parse

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

def test_net_sink(base_url, size_mb=10):
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

    try:
        sock = socket.create_connection((host, port), timeout=10.0)
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

        while sent < total_bytes:
            to_send = min(CHUNK_SIZE, total_bytes - sent)
            sock.sendall(pattern_block[:to_send])
            sent += to_send

            now = time.perf_counter()
            dt = now - last_t
            if dt >= 0.2:
                instant_speed = ((sent - last_sent) / (1024.0 * 1024.0)) / dt
                last_t = now
                last_sent = sent
                progress_bar(sent, total_bytes, instant_speed, prefix="SINK")

        progress_bar(sent, total_bytes, instant_speed, prefix="SINK")
        sys.stdout.write("\n")

        # Aguarda resposta do servidor ESP32
        sock.settimeout(5.0)
        resp_data = b""
        while True:
            chunk = sock.recv(1024)
            if not chunk:
                break
            resp_data += chunk
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
                print(f"  Telemetria interna do ESP32: {esp_info.get('speed_mbs', 0)} MB/s ({esp_info.get('duration_ms', 0)} ms)")
            except Exception:
                pass
        return speed_mbs
    except Exception as e:
        print(f"  ❌ Falha no teste SINK: {e}")
        return None

def test_net_source(base_url, size_mb=10):
    total_bytes = size_mb * 1024 * 1024
    print("\n" + "=" * 65)
    print(f"📥 3. TESTE DE VAZÃO PURA DA REDE - SOURCE ({size_mb} MB baixados da RAM)")
    print("     (Mede a velocidade pura de transmissão da placa para o PC)")
    print("=" * 65)

    source_url = f"{base_url}/api/bench_source?size={total_bytes}"
    try:
        req = urllib.request.Request(source_url, headers={'User-Agent': 'MPS3-Bench'})
        t0 = time.perf_counter()
        received = 0
        last_t = t0
        last_received = 0
        instant_speed = 0.0

        with urllib.request.urlopen(req, timeout=15.0) as resp:
            while True:
                chunk = resp.read(CHUNK_SIZE)
                if not chunk:
                    break
                received += len(chunk)
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

def test_sd_upload(base_url, size_mb=10):
    total_bytes = size_mb * 1024 * 1024
    print("\n" + "=" * 65)
    print(f"💾 4. TESTE DE UPLOAD PONTA A PONTA (REDE + SDMMC FATFS) ({size_mb} MB)")
    print("     (Pipeline Ping-Pong assíncrono gravando arquivo real no Cartão SD)")
    print("=" * 65)

    # Gera payload previsível e calcula hash MD5 de conferência
    print("  Gerando dados de teste e calculando hash MD5...")
    hasher = hashlib.md5()
    # Usando repetição de padrão binário
    pattern = bytearray(CHUNK_SIZE)
    for i in range(len(pattern)):
        pattern[i] = (i * 7 + 13) & 0xFF
    
    full_data = bytearray()
    for _ in range(total_bytes // CHUNK_SIZE):
        full_data.extend(pattern)
        hasher.update(pattern)
    expected_md5 = hasher.hexdigest()
    print(f"  Hash MD5 original: {expected_md5}")

    test_file_path = "/bench_test_temp.bin"
    upload_url = f"{base_url}/api/upload?path={urllib.parse.quote(test_file_path)}&idx=1&count=1&batchTotal={total_bytes}"

    parsed = urllib.parse.urlparse(upload_url)
    host = parsed.hostname
    port = parsed.port or 80
    path = parsed.path + "?" + parsed.query

    try:
        sock = socket.create_connection((host, port), timeout=15.0)
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

        while sent < total_bytes:
            to_send = min(CHUNK_SIZE, total_bytes - sent)
            sock.sendall(full_data[sent:sent+to_send])
            sent += to_send

            now = time.perf_counter()
            dt = now - last_t
            if dt >= 0.2:
                instant_speed = ((sent - last_sent) / (1024.0 * 1024.0)) / dt
                last_t = now
                last_sent = sent
                progress_bar(sent, total_bytes, instant_speed, prefix="SD_PUT")

        progress_bar(sent, total_bytes, instant_speed, prefix="SD_PUT")
        sys.stdout.write("\n")

        # Aguarda confirmação HTTP 200 do ESP32
        sock.settimeout(15.0)
        resp_data = b""
        while True:
            chunk = sock.recv(1024)
            if not chunk:
                break
            resp_data += chunk
        sock.close()
        t1 = time.perf_counter()
        total_time = t1 - t0
        upload_speed_mbs = (total_bytes / (1024.0 * 1024.0)) / total_time

        print(f"  Concluído em: {total_time:.2f} s")
        print(f"  Taxa Efetiva de Escrita Ponta a Ponta: {format_speed(total_bytes, total_time)}")

        # Validação de integridade baixando o arquivo gravado e conferindo MD5
        print("\n  Verificando integridade no cartão SD...")
        download_url = f"{base_url}/api/download?path={urllib.parse.quote(test_file_path)}"
        req = urllib.request.Request(download_url, headers={'User-Agent': 'MPS3-Bench'})
        verify_hasher = hashlib.md5()
        verified_bytes = 0
        with urllib.request.urlopen(req, timeout=15.0) as resp:
            while True:
                chunk = resp.read(CHUNK_SIZE)
                if not chunk:
                    break
                verify_hasher.update(chunk)
                verified_bytes += len(chunk)

        actual_md5 = verify_hasher.hexdigest()
        if actual_md5 == expected_md5 and verified_bytes == total_bytes:
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

        return upload_speed_mbs
    except Exception as e:
        print(f"  ❌ Falha no teste de Upload SD: {e}")
        return None

def main():
    parser = argparse.ArgumentParser(description="MPS3 Wi-Fi Speed & Pipeline Benchmark Tool")
    parser.add_argument("target", nargs="?", default="192.168.15.61",
                        help="IP ou hostname do dispositivo MPS3 (ex: 192.168.15.61 ou 192.168.4.1)")
    parser.add_argument("--size", type=int, default=10, help="Tamanho do arquivo de teste em MB (padrão: 10)")
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

    # 1. RTT Latency Ping
    rtt_res = test_ping_latency(base_url, samples=8)

    # 2. Network Sink (pure RAM)
    sink_speed = test_net_sink(base_url, size_mb=args.size)

    # 3. Network Source (pure RAM)
    source_speed = test_net_source(base_url, size_mb=args.size)

    # 4. SD Upload (Network + SDMMC FatFS)
    sd_speed = test_sd_upload(base_url, size_mb=args.size)

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
        print(f"  - Upload Efetivo para SD:   {sd_speed:6.2f} MB/s ({sd_speed * 8:6.2f} Mbps)")

    if sink_speed and sd_speed:
        efficiency = (sd_speed / sink_speed) * 100.0
        print(f"  - Eficiência do Pipeline:   {efficiency:5.1f}% do teto da rede aproveitado")
        if efficiency > 75.0:
            print("  - Diagnóstico: 🚀 Pipeline assíncrono altamente eficiente!")
        else:
            print("  - Diagnóstico: ⚠️ Cartão SD é o limitador físico de velocidade.")
    print("=" * 65 + "\n")

if __name__ == "__main__":
    main()

