#!/usr/bin/env python3
"""
MPS3 OLED Screen Capture Tool
Captura o buffer de vídeo do display SSD1306 (1024 bytes) diretamente do ESP32-S3 via UART
e converte em imagens PNG de alta resolução (1-bit com pixels nítidos) e mapa ASCII.
"""

import sys
import os
import time
import base64
import zlib
import struct
import argparse
from datetime import datetime

try:
    import serial
except ImportError:
    print("ERRO: Pacote 'pyserial' não encontrado. Instale com: pip install pyserial", file=sys.stderr)
    sys.exit(1)


def make_png(pixels, scale=6, color_on=(0, 229, 255), color_off=(7, 10, 16)):
    """
    Gera um binário PNG RGBA puro usando apenas a biblioteca padrão (struct + zlib).
    """
    w = 128 * scale
    h = 64 * scale
    
    r_on, g_on, b_on = color_on
    r_off, g_off, b_off = color_off
    
    on_bytes = bytes([r_on, g_on, b_on, 255])
    off_bytes = bytes([r_off, g_off, b_off, 255])
    
    raw_rgba = bytearray()
    for y_s in range(h):
        raw_rgba.append(0)  # Filter type 0 (None)
        y = y_s // scale
        for x_s in range(w):
            x = x_s // scale
            if pixels[y][x]:
                raw_rgba.extend(on_bytes)
            else:
                raw_rgba.extend(off_bytes)

    def chunk(chunk_type, data):
        return (struct.pack('>I', len(data)) + 
                chunk_type + 
                data + 
                struct.pack('>I', zlib.crc32(chunk_type + data) & 0xffffffff))

    png = bytearray(b'\x89PNG\r\n\x1a\n')
    ihdr = struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)
    png.extend(chunk(b'IHDR', ihdr))
    png.extend(chunk(b'IDAT', zlib.compress(bytes(raw_rgba), 9)))
    png.extend(chunk(b'IEND', b''))
    return bytes(png)


def capture_framebuffer(port='COM5', baudrate=115200, cmd_before=None, timeout=1.5):
    """
    Conecta à UART com DTR/RTS desativados, opcionalmente envia um comando,
    e solicita a captura do framebuffer através do comando 'screen'.
    """
    s = serial.Serial()
    s.port = port
    s.baudrate = baudrate
    s.dtr = False
    s.rts = False
    s.timeout = 1.0
    s.open()
    time.sleep(0.05)
    s.reset_input_buffer()

    if cmd_before:
        for c in cmd_before:
            s.write(c.encode('utf-8') + b'\n')
            time.sleep(0.12)
        s.reset_input_buffer()

    s.write(b'screen\n')
    t0 = time.time()
    raw = None
    while time.time() - t0 < timeout:
        line = s.readline().decode('utf-8', errors='ignore').strip()
        if line.startswith('@SCR '):
            b64_data = line[5:].strip()
            raw = base64.b64decode(b64_data)
            break
    s.close()
    return raw


def decode_ssd1306(raw_bytes, rotate_180=True):
    """
    Decodifica o buffer SSD1306 (8 páginas x 128 colunas, 1024 bytes).
    Aplica rotação de 180° por padrão para casar com a montagem física U8G2_R2 do MPS3.
    """
    if not raw_bytes or len(raw_bytes) != 1024:
        raise ValueError(f"Buffer inválido: esperado 1024 bytes, recebido {len(raw_bytes) if raw_bytes else 0}")

    raw_pixels = []
    for y in range(64):
        row = []
        page = y // 8
        bit = y % 8
        for x in range(128):
            byte_val = raw_bytes[page * 128 + x]
            row.append((byte_val >> bit) & 1)
        raw_pixels.append(row)

    if not rotate_180:
        return raw_pixels

    # Rotação física de 180 graus (U8G2_R2)
    logical_pixels = []
    for y in range(64):
        row = []
        for x in range(128):
            row.append(raw_pixels[63 - y][127 - x])
        logical_pixels.append(row)

    return logical_pixels


def print_ascii(pixels):
    """Exibe o mapa visual da tela 128x64 no terminal."""
    if hasattr(sys.stdout, 'reconfigure'):
        try:
            sys.stdout.reconfigure(encoding='utf-8')
        except Exception:
            pass

    char_on = '#' if sys.platform == 'win32' and not sys.stdout.encoding.lower().startswith('utf') else '█'
    hdr1 = '    ' + ''.join([str(i // 10) if i % 10 == 0 else ' ' for i in range(128)])
    hdr2 = '    ' + ''.join([str(i % 10) for i in range(128)])
    print(hdr1)
    print(hdr2)
    for y in range(64):
        line = ''.join([char_on if p else '.' for p in pixels[y]])
        print(f'{y:02d}: {line}')


def main():
    parser = argparse.ArgumentParser(description="MPS3 OLED Framebuffer Capture & Visual Inspection")
    parser.add_argument('--port', '-p', default='COM5', help="Porta serial UART (padrão: COM5)")
    parser.add_argument('--baud', '-b', type=int, default=115200, help="Baudrate (padrão: 115200)")
    parser.add_argument('--out', '-o', help="Caminho de saída da imagem PNG (padrão: auto-gerado em captures/)")
    parser.add_argument('--scale', '-s', type=int, default=6, help="Fator de escala dos pixels (padrão: 6x -> 768x384)")
    parser.add_argument('--cmd', '-c', action='append', help="Comando(s) UART a enviar antes do print (ex: -c 'prompt' ou -c 'btn U')")
    parser.add_argument('--no-rotate', action='store_true', help="Não rotacionar 180° (mantém orientação bruta do buffer)")
    parser.add_argument('--ascii', action='store_true', help="Imprimir visualização ASCII no terminal")
    parser.add_argument('--white', action='store_true', help="Usar pixels brancos em vez de ciano estilo OLED")
    
    args = parser.parse_args()

    print(f"[*] Conectando à placa na porta {args.port}...")
    try:
        raw = capture_framebuffer(port=args.port, baudrate=args.baud, cmd_before=args.cmd)
    except Exception as e:
        print(f"[-] Erro ao comunicar com a porta {args.port}: {e}", file=sys.stderr)
        sys.exit(1)

    if not raw:
        print("[-] Falha ao capturar o framebuffer. Verifique se a placa está ligada e respondendo.", file=sys.stderr)
        sys.exit(1)

    rotate = not args.no_rotate
    pixels = decode_ssd1306(raw, rotate_180=rotate)

    if args.ascii:
        print_ascii(pixels)

    color = (255, 255, 255) if args.white else (0, 229, 255)
    png_bytes = make_png(pixels, scale=args.scale, color_on=color)

    out_file = args.out
    if not out_file:
        os.makedirs("captures", exist_ok=True)
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        out_file = os.path.join("captures", f"oled_capture_{ts}.png")

    # Garante que diretório de destino exista
    out_dir = os.path.dirname(out_file)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)

    with open(out_file, "wb") as f:
        f.write(png_bytes)

    print(f"[+] Framebuffer capturado com sucesso! (1024 bytes)")
    print(f"[+] Imagem PNG ({128 * args.scale}x{64 * args.scale} px) salva em: {out_file}")


if __name__ == "__main__":
    main()
