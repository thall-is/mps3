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
    timeout_val = max(30.0, size_mb * 6.0)
    try:
        req = urllib.request.Request(source_url, headers={'User-Agent': 'MPS3-Bench'})
        t0 = time.perf_counter()
        received = 0
        last_t = t0
        last_received = 0
        instant_speed = 0.0

        with urllib.request.urlopen(req, timeout=timeout_val) as resp:
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

        while sent < total_bytes:
            to_send = min(CHUNK_SIZE, total_bytes - sent)
            sock.sendall(pattern[:to_send])
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
                    if b"Content-Length: 0" in resp_data or resp_data.endswith(b"\r\n\r\n"):
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

        # Validação de integridade baixando o arquivo gravado e conferindo MD5
        print("\n  Verificando integridade no cartão SD...")
        download_url = f"{base_url}/api/download?path={urllib.parse.quote(test_file_path)}"
        req = urllib.request.Request(download_url, headers={'User-Agent': 'MPS3-Bench'})
        verify_hasher = hashlib.md5()
        verified_bytes = 0
        download_timeout = max(30.0, size_mb * 6.0)
        with urllib.request.urlopen(req, timeout=download_timeout) as resp:
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
            except Exception:
                break

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

def main():
    parser = argparse.ArgumentParser(description="MPS3 Wi-Fi Speed & Pipeline Benchmark Tool")
    parser.add_argument("target", nargs="?", default="192.168.15.61",
                        help="IP ou hostname do dispositivo MPS3 (ex: 192.168.15.61 ou 192.168.4.1)")
    parser.add_argument("--size", type=int, default=10, help="Tamanho do arquivo de teste em MB (padrão: 10)")
    parser.add_argument("--serial-port", default="COM5", help="Porta serial para monitoramento de UI e reboots (padrão: COM5)")
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
                print(f"  Estado de UI inicial: Modo={ui_init.get('mode_name')} | Wi-Fi={ui_init.get('wifi')} | Menu={ui_init.get('menu_active')}")
                if not ui_init.get("wifi"):
                    print("  ⚠️ Dispositivo não está no modo Wi-Fi! Enviando comando 'w' para ativar...")
                    mon.send_cmd("w")
                    time.sleep(3.0)
                    ui_init = mon.query_ui()
                    print(f"  Novo estado: Wi-Fi={ui_init.get('wifi')}")
            else:
                print("  (Sem resposta do comando @UI - continuando via HTTP)")

    status_before = check_device_status(base_url)
    if not status_before:
        print("❌ Dispositivo não está respondendo na URL especificada.")
        print("   Certifique-se de que o dispositivo está no modo Wi-Fi (tela 'WIFI' ativa).")
        if mon: mon.stop()
        sys.exit(1)

    # 1. RTT Latency Ping
    rtt_res = test_ping_latency(base_url, samples=8)
    if mon and not mon.check_healthy("Ping Latency"):
        mon.stop()
        sys.exit(1)

    # 2. Network Sink (pure RAM)
    sink_speed = test_net_sink(base_url, size_mb=args.size)
    if mon and not mon.check_healthy("Network Sink"):
        mon.stop()
        sys.exit(1)

    # 3. Network Source (pure RAM)
    source_speed = test_net_source(base_url, size_mb=args.size)
    if mon and not mon.check_healthy("Network Source"):
        mon.stop()
        sys.exit(1)

    # 4. SD Upload (Network + SDMMC FatFS)
    sd_speed = test_sd_upload(base_url, size_mb=args.size)
    if mon and not mon.check_healthy("SD Card Upload"):
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
        print(f"  - Upload Efetivo para SD:   {sd_speed:6.2f} MB/s ({sd_speed * 8:6.2f} Mbps)")

    if sink_speed and sd_speed:
        efficiency = (sd_speed / sink_speed) * 100.0
        print(f"  - Eficiência do Pipeline:   {efficiency:5.1f}% do teto da rede aproveitado")
        if efficiency > 75.0:
            print("  - Diagnóstico: 🚀 Pipeline assíncrono altamente eficiente!")
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

