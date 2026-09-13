#!/usr/bin/env python3
"""
mps3 Multi-COM Testbench Monitor
=================================
Monitor sincronizado multi-porta serial para o sistema de áudio mps3:
  - ESP32-S3 (Placa Principal / Player): COM7
  - ESP32 Classic (Co-Processador bt_companion): COM12
  - ESP32 Classic (Receptor de Teste bt_audio_sink): auto-detectado ou configurado

Uso:
  python testbench_monitor.py [--s3 COM7] [--comp COM12] [--sink COM3] [--baud 115200]
"""

import sys
import time
import threading
import argparse
from datetime import datetime

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("ERRO: pyserial nao instalado. Execute: pip install pyserial")
    sys.exit(1)

# Cores ANSI para o terminal
CLR_RESET   = "\033[0m"
CLR_BOLD    = "\033[1m"
CLR_S3      = "\033[96m"  # Ciano
CLR_COMP    = "\033[93m"  # Amarelo
CLR_SINK    = "\033[92m"  # Verde
CLR_ERR     = "\033[91m"  # Vermelho
CLR_WARN    = "\033[33m"  # Laranja
CLR_CODEC   = "\033[95m"  # Magenta
CLR_SEP     = "\033[90m"  # Cinza

# Palavras-chave de destaque
HIGHLIGHT_CODECS = ["LDAC", "aptX", "aptX HD", "SBC", "AAC"]
HIGHLIGHT_ERRORS = ["GURU", "abort()", "assert", "Falha", "ERROR", "panic", "ESP_ERR"]
HIGHLIGHT_NOTIFS = ["Codec ativo", "TELEMETRIA", "SEP", "conectado", "bitrate", "Preferencia"]

running = True
log_lock = threading.Lock()

def colorize(text, tag_color):
    upper = text.upper()
    for err in HIGHLIGHT_ERRORS:
        if err.upper() in upper:
            return f"{CLR_ERR}{CLR_BOLD}{text}{CLR_RESET}"
    for codec in HIGHLIGHT_CODECS:
        if codec in text:
            return f"{tag_color}{text.replace(codec, f'{CLR_CODEC}{CLR_BOLD}{codec}{CLR_RESET}{tag_color}')}{CLR_RESET}"
    return f"{tag_color}{text}{CLR_RESET}"

def reader_thread(port_name, tag, tag_color, baud, log_file):
    global running
    try:
        ser = serial.Serial(port_name, baud, timeout=0.1)
    except Exception as e:
        with log_lock:
            print(f"{CLR_WARN}[{tag} - {port_name}] Nao foi possivel abrir porta: {e}{CLR_RESET}")
        return

    with log_lock:
        print(f"{tag_color}[{tag}] Conectado em {port_name} @ {baud} bps{CLR_RESET}")

    buf = ""
    while running:
        try:
            raw = ser.read(ser.in_waiting or 1)
            if not raw:
                time.sleep(0.01)
                continue
            text = raw.decode("utf-8", "replace")
            buf += text
            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                line = line.strip("\r\n")
                if not line:
                    continue
                ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
                with log_lock:
                    formatted = f"{CLR_SEP}[{ts}]{CLR_RESET} {tag_color}[{tag:4s}]{CLR_RESET} {colorize(line, tag_color)}"
                    print(formatted)
                    if log_file:
                        log_file.write(f"[{ts}] [{tag}] {line}\n")
                        log_file.flush()
        except Exception as e:
            if running:
                with log_lock:
                    print(f"{CLR_ERR}[{tag}] Erro de leitura: {e}{CLR_RESET}")
            break
    ser.close()

def main():
    global running
    parser = argparse.ArgumentParser(description="mps3 Multi-COM Real-Time Testbench Monitor")
    parser.add_argument("--s3", default="COM7", help="Porta COM do ESP32-S3 (padrao: COM7)")
    parser.add_argument("--comp", default="COM12", help="Porta COM do Companion BT (padrao: COM12)")
    parser.add_argument("--sink", default=None, help="Porta COM do Sink de Teste (opcional)")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (padrao: 115200)")
    parser.add_argument("--log", default="testbench.log", help="Arquivo de log unificado")
    args = parser.parse_args()

    print(f"{CLR_BOLD}======================================================{CLR_RESET}")
    print(f"{CLR_BOLD}   mps3 Multi-COM Real-Time Audio Testbench Monitor   {CLR_RESET}")
    print(f"{CLR_BOLD}======================================================{CLR_RESET}")
    print(f" Portas configuradas:")
    print(f"   {CLR_S3}S3 Player :{CLR_RESET} {args.s3}")
    print(f"   {CLR_COMP}Companion :{CLR_RESET} {args.comp}")
    if args.sink:
        print(f"   {CLR_SINK}Sink Test :{CLR_RESET} {args.sink}")
    print(f" Gravando log unificado em: {args.log}")
    print(f" Pressione Ctrl+C para encerrar.\n")

    log_f = open(args.log, "w", encoding="utf-8")

    threads = []
    t_s3 = threading.Thread(target=reader_thread, args=(args.s3, "S3", CLR_S3, args.baud, log_f), daemon=True)
    t_comp = threading.Thread(target=reader_thread, args=(args.comp, "COMP", CLR_COMP, args.baud, log_f), daemon=True)
    threads.extend([t_s3, t_comp])

    if args.sink:
        t_sink = threading.Thread(target=reader_thread, args=(args.sink, "SINK", CLR_SINK, args.baud, log_f), daemon=True)
        threads.append(t_sink)

    for t in threads:
        t.start()

    try:
        while True:
            time.sleep(0.5)
    except KeyboardInterrupt:
        print(f"\n{CLR_WARN}Encerrando monitor...{CLR_RESET}")
        running = False
        time.sleep(0.3)
        log_f.close()
        print(f"Log salvo com sucesso em: {args.log}")

if __name__ == "__main__":
    main()

