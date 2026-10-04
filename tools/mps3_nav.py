#!/usr/bin/env python3
"""
mps3_nav.py - Biblioteca e CLI de Navegacao Automatizada do MPS3

Permite injetar eventos de botoes (U, D, L, R, C), consultar o estado da UI (@UI),
fazer capturas do display OLED (@SCR) e navegar automaticamente atraves de um
grafo de telas para automacao de testes e debugging HIL (Hardware-In-the-Loop).
"""

import sys
import time
import json
import base64
import zlib
import struct
import re
import threading
import argparse
from typing import Optional, Dict, Any, Callable

try:
    import serial
except ImportError:
    print("Erro: pyserial nao instalado. Execute: pip install pyserial", file=sys.stderr)
    sys.exit(1)


class DeviceRebooted(Exception):
    """Lancado quando um reinicio ou crash inesperado do hardware e detectado."""
    pass


class NavigationTimeout(Exception):
    """Lancado quando um estado esperado nao e alcancado dentro do timeout."""
    pass


class Mps3Nav:
    def __init__(self, port: str = "COM5", baudrate: int = 115200, timeout: float = 1.0):
        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self.ser: Optional[serial.Serial] = None
        
        self._running = False
        self._reader_thread: Optional[threading.Thread] = None
        self._lock = threading.Lock()
        
        self.last_ui: Dict[str, Any] = {}
        self.last_scr_b64: Optional[str] = None
        self.raw_logs: list[str] = []
        
        self._ui_event = threading.Event()
        self._scr_event = threading.Event()
        self._reboot_detected = False
        self._reboot_reason = ""
        self._prev_dbg_mode = False

    def __enter__(self):
        self.open()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()

    def open(self):
        self.ser = serial.Serial()
        self.ser.port = self.port
        self.ser.baudrate = self.baudrate
        self.ser.timeout = self.timeout
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self._running = True
        self._reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
        self._reader_thread.start()
        
        # Aguarda estabilizacao inicial da porta serial
        time.sleep(1.2)
        # Limpa flags de reset geradas pela conexao fisica USB DTR/RTS
        self._reboot_detected = False
        self._reboot_reason = ""
        self.send_cmd("dbg on")
        time.sleep(0.3)
        try:
            self.query_ui()
        except DeviceRebooted:
            time.sleep(1.5)
            self._reboot_detected = False
            self.send_cmd("dbg on")
            time.sleep(0.3)
            self.query_ui()

    def close(self):
        if self._running:
            try:
                # Restaura para modo silencioso para nao poluir o uso normal
                self.send_cmd("dbg off")
                time.sleep(0.1)
            except Exception:
                pass
            self._running = False
            if self.ser:
                self.ser.close()

    def send_cmd(self, cmd: str):
        if not self.ser or not self.ser.is_open:
            raise RuntimeError("Porta serial fechada")
        with self._lock:
            self.ser.write(f"\n{cmd}\n".encode("utf-8"))
            self.ser.flush()

    def _reader_loop(self):
        while self._running and self.ser and self.ser.is_open:
            try:
                line = self.ser.readline()
                if not line:
                    continue
                s = line.decode("utf-8", errors="replace").strip()
                if not s:
                    continue
                
                self.raw_logs.append(s)
                if len(self.raw_logs) > 200:
                    self.raw_logs.pop(0)

                # Deteccao de reboot / crash do ESP32
                if "rst:0x" in s or "Guru Meditation" in s or "TG1WDT_SYS_RST" in s or "POWERON" in s:
                    self._reboot_detected = True
                    self._reboot_reason = s

                # Parse de snapshot de UI
                if s.startswith("@UI "):
                    try:
                        data = json.loads(s[4:])
                        self.last_ui = data
                        self._ui_event.set()
                    except Exception:
                        pass
                
                # Parse de tela @SCR
                elif s.startswith("@SCR "):
                    self.last_scr_b64 = s[5:].strip()
                    self._scr_event.set()

            except Exception:
                break

    def query_ui(self, timeout: float = 2.0) -> Dict[str, Any]:
        """Solicita e aguarda um snapshot @UI atualizado."""
        self._ui_event.clear()
        self.send_cmd("ui")
        if not self._ui_event.wait(timeout):
            if self._reboot_detected:
                raise DeviceRebooted(f"Dispositivo reiniciou durante query_ui: {self._reboot_reason}")
            raise NavigationTimeout("Timeout aguardando resposta @UI")
        return self.last_ui

    def press(self, btn: str, hold_ms: int = 80):
        """Pressiona um botao ('u', 'd', 'l', 'r', 'c')."""
        self.send_cmd(f"btn {btn} {hold_ms}")
        time.sleep((hold_ms + 60) / 1000.0)

    def seq(self, seq_str: str, wait_after: float = 0.2):
        """Enfileira sequencia de cliques, ex: 'U,U,R,C:800'."""
        self.send_cmd(f"seq {seq_str}")
        time.sleep(wait_after)

    def home(self):
        """Cancela qualquer modo ativo, acorda display e volta para o menu principal."""
        self.send_cmd("nav reset")
        time.sleep(0.3)
        return self.query_ui()

    def screenshot(self, output_png_path: Optional[str] = None) -> bytes:
        """Captura o framebuffer do OLED (128x64) e opcionalmente salva como PNG."""
        self._scr_event.clear()
        self.send_cmd("screen")
        if not self._scr_event.wait(timeout=3.0):
            if self._reboot_detected:
                raise DeviceRebooted(f"Dispositivo reiniciou durante screenshot: {self._reboot_reason}")
            raise NavigationTimeout("Timeout aguardando captura @SCR")
        
        raw_b64 = self.last_scr_b64
        if not raw_b64:
            raise RuntimeError("Nenhum dado de tela recebido")
        
        fb = base64.b64decode(raw_b64)
        if len(fb) != 1024:
            raise ValueError(f"Tamanho do framebuffer incorreto: {len(fb)} (esperado 1024)")

        # Decodifica matriz 128x64 a partir das 8 paginas SSD1306
        # Cada pagina tem 128 colunas; cada byte tem 8 pixels verticais
        pixels = bytearray(128 * 64)
        for page in range(8):
            for col in range(128):
                byte_val = fb[page * 128 + col]
                for bit in range(8):
                    px = 255 if (byte_val & (1 << bit)) else 0
                    y = page * 8 + bit
                    x = col
                    pixels[y * 128 + x] = px

        # O display fisico usa U8G2_R2 (rotacao 180 graus aplicada no desenho)
        # Invertemos 180 graus para exibir exatamente como o olho humano enxerga
        rotated = bytearray(128 * 64)
        for y in range(64):
            for x in range(128):
                rotated[(63 - y) * 128 + (127 - x)] = pixels[y * 128 + x]

        png_bytes = self._encode_png(rotated, 128, 64, scale=3)
        if output_png_path:
            with open(output_png_path, "wb") as f:
                f.write(png_bytes)
        return png_bytes

    def _encode_png(self, pixels: bytearray, width: int, height: int, scale: int = 1) -> bytes:
        """Gera PNG monocromatico em escala sem bibliotecas externas."""
        sw = width * scale
        sh = height * scale
        
        # Escala os pixels
        scaled_rows = []
        for y in range(height):
            row = bytearray()
            for x in range(width):
                val = pixels[y * width + x]
                row.extend([val] * scale)
            for _ in range(scale):
                scaled_rows.append(row)

        raw_data = bytearray()
        for row in scaled_rows:
            raw_data.append(0) # Filtro PNG: None
            raw_data.extend(row)

        compressed = zlib.compress(raw_data)

        def make_chunk(chunk_type: bytes, data: bytes) -> bytes:
            length = struct.pack(">I", len(data))
            crc = struct.pack(">I", zlib.crc32(chunk_type + data) & 0xFFFFFFFF)
            return length + chunk_type + data + crc

        png = bytearray(b"\x89PNG\r\n\x1a\n")
        # IHDR: width, height, bit_depth=8, color_type=0 (grayscale), compression=0, filter=0, interlace=0
        ihdr_data = struct.pack(">IIBBBBB", sw, sh, 8, 0, 0, 0, 0)
        png.extend(make_chunk(b"IHDR", ihdr_data))
        png.extend(make_chunk(b"IDAT", compressed))
        png.extend(make_chunk(b"IEND", b""))
        return bytes(png)

    def wait_for(self, condition: Callable[[Dict[str, Any]], bool], timeout: float = 10.0, step: float = 0.2) -> Dict[str, Any]:
        """Aguarda ate que uma condicao no estado da UI seja verdadeira."""
        start = time.time()
        while time.time() - start < timeout:
            if self._reboot_detected:
                raise DeviceRebooted(f"Dispositivo reiniciou: {self._reboot_reason}")
            try:
                st = self.query_ui(timeout=1.0)
                if condition(st):
                    return st
            except NavigationTimeout:
                pass
            time.sleep(step)
        raise NavigationTimeout(f"Condicao nao satisfeita em {timeout}s (Ultimo estado: {self.last_ui})")

    def goto(self, target: str) -> Dict[str, Any]:
        """
        Navega dinamicamente ate uma tela de destino:
        Exemplos de targets:
          - 'MAIN_MENU'
          - 'WIFI'
          - 'CONF_MENU'
          - 'CONF_SYSTEM'
          - 'CONF_AUDIO'
          - 'CONF_DISPLAY'
          - 'TOP_SCREEN'
        """
        target = target.upper()
        self.home()
        st = self.query_ui()

        if target in ("MAIN_MENU", "LIST"):
            return st

        if target == "WIFI":
            # No menu principal: indice 0=Player, 1=WiFi, 2=BT, 3=Conf, 4=USB, 5=Game
            # Se comeca em 0, vai para 1 (U ou D conforme cursor)
            self.seq("D,R")
            return self.wait_for(lambda s: s.get("wifi") is True or s.get("mode_name") == "LIST", timeout=5.0)

        elif target == "CONF_MENU":
            # Indice 3 = Conf: desce 3 vezes e entra com R
            self.seq("D,D,D,R")
            return self.wait_for(lambda s: s.get("mode_name") == "CONF_MENU", timeout=4.0)

        elif target == "CONF_SYSTEM":
            self.goto("CONF_MENU")
            # Em CONF_MENU: 0=Audio, 1=Display, 2=Sistema
            self.seq("D,D,R")
            return self.wait_for(lambda s: s.get("mode_name") == "CONF_SYSTEM", timeout=4.0)

        elif target == "CONF_AUDIO":
            self.goto("CONF_MENU")
            self.seq("R")
            return self.wait_for(lambda s: s.get("mode_name") == "CONF_AUDIO", timeout=4.0)

        elif target == "CONF_DISPLAY":
            self.goto("CONF_MENU")
            self.seq("D,R")
            return self.wait_for(lambda s: s.get("mode_name") == "CONF_DISPLAY", timeout=4.0)

        elif target == "TOP_SCREEN":
            # Da tela PLAYING, aperta U para subir para TOP_SCREEN
            self.home()
            self.seq("U")
            return self.wait_for(lambda s: s.get("mode_name") == "TOP_SCREEN", timeout=3.0)

        else:
            raise ValueError(f"Target de navegacao desconhecido: {target}")


def main():
    parser = argparse.ArgumentParser(description="Ferramenta de Navegacao e Debug Automatizado do MPS3")
    parser.add_argument("port", nargs="?", default="COM5", help="Porta serial do ESP32 (ex: COM5)")
    parser.add_argument("cmd", nargs="?", default="ui", choices=["ui", "home", "press", "seq", "screen", "goto", "monitor"], help="Comando a executar")
    parser.add_argument("args", nargs="*", help="Argumentos do comando (ex: goto WIFI, press c 700, screen tela.png)")

    args = parser.parse_args()

    with Mps3Nav(args.port) as nav:
        if args.cmd == "ui":
            print(json.dumps(nav.query_ui(), indent=2))
        elif args.cmd == "home":
            nav.home()
            print("Home executado com sucesso.")
            print(json.dumps(nav.last_ui, indent=2))
        elif args.cmd == "press":
            btn = args.args[0] if len(args.args) > 0 else "c"
            ms = int(args.args[1]) if len(args.args) > 1 else 80
            nav.press(btn, ms)
            print(f"Botao {btn} pressionado ({ms} ms)")
        elif args.cmd == "seq":
            seq_val = args.args[0] if len(args.args) > 0 else "U,R"
            nav.seq(seq_val)
            print(f"Sequencia {seq_val} enviada")
        elif args.cmd == "screen":
            out_path = args.args[0] if len(args.args) > 0 else "mps3_screen.png"
            nav.screenshot(out_path)
            print(f"Captura salva em: {out_path}")
        elif args.cmd == "goto":
            tgt = args.args[0] if len(args.args) > 0 else "MAIN_MENU"
            st = nav.goto(tgt)
            print(f"Navegado com sucesso para {tgt}:")
            print(json.dumps(st, indent=2))
        elif args.cmd == "monitor":
            print("Monitorando eventos de UI... (Pressione Ctrl+C para sair)")
            while True:
                time.sleep(0.5)


if __name__ == "__main__":
    main()
