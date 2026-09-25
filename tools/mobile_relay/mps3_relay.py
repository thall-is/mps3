#!/usr/bin/env python3
"""
MPS3 Mobile Relay Proxy
Permite que o reprodutor MPS3 (ESP32-S3), conectado ao Hotspot Wi-Fi do smartphone,
acesse o servidor Radxa Rock 3E na VPN Tailscale (100.120.93.115:8088).

Como funciona:
O Android cria uma rede NAT para o Hotspot (ex: 192.168.43.0/24), onde o celular
é o gateway (192.168.43.1). O MPS3 tenta se conectar automaticamente em http://192.168.43.1:8088.
Este script roda no celular (via Termux) na porta 8088 e encaminha todas as requisições
diretamente pela interface Tailscale do celular para o servidor.

Uso no Termux:
    python mps3_relay.py
    ou
    python mps3_relay.py --target http://100.120.93.115:8088 --port 8088
"""

import sys
import argparse
import urllib.request
import urllib.error
import urllib.parse
from http.server import HTTPServer, BaseHTTPRequestHandler
import socketserver
import time

DEFAULT_TARGET = "http://100.120.93.115:8088"
DEFAULT_PORT = 8088
BUFFER_SIZE = 64 * 1024  # 64 KB por bloco de streaming


class ThreadingHTTPServer(socketserver.ThreadingMixIn, HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


class RelayHandler(BaseHTTPRequestHandler):
    target_base = DEFAULT_TARGET

    def log_message(self, format, *args):
        sys.stderr.write(f"[{time.strftime('%H:%M:%S')}] {self.client_address[0]} - {format % args}\n")

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, HEAD, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Range, Content-Type")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):
        self._proxy_request("GET")

    def do_HEAD(self):
        self._proxy_request("HEAD")

    def _proxy_request(self, method):
        target_url = f"{self.target_base.rstrip('/')}{self.path}"
        req = urllib.request.Request(target_url, method=method)

        # Repassa cabeçalhos relevantes (especialmente Range para áudio)
        for header, value in self.headers.items():
            if header.lower() in ["range", "accept", "user-agent", "if-none-match", "if-modified-since"]:
                req.add_header(header, value)

        try:
            with urllib.request.urlopen(req, timeout=30) as remote_resp:
                self.send_response(remote_resp.status)

                # Repassa cabeçalhos da resposta
                for h, val in remote_resp.getheaders():
                    # Evita duplicar cabeçalhos hop-by-hop
                    if h.lower() not in ["connection", "transfer-encoding"]:
                        self.send_header(h, val)

                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()

                if method != "HEAD":
                    while True:
                        chunk = remote_resp.read(BUFFER_SIZE)
                        if not chunk:
                            break
                        try:
                            self.wfile.write(chunk)
                        except (BrokenPipeError, ConnectionResetError):
                            break

        except urllib.error.HTTPError as e:
            self.send_response(e.code)
            for h, val in e.headers.items():
                if h.lower() not in ["connection", "transfer-encoding"]:
                    self.send_header(h, val)
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            try:
                self.wfile.write(e.read())
            except Exception:
                pass

        except Exception as e:
            self.send_error(502, f"Erro de comunicacao com o servidor Tailscale: {e}")


def main():
    parser = argparse.ArgumentParser(description="MPS3 Mobile Relay Proxy (Hotspot -> Tailscale)")
    parser.add_argument("-t", "--target", default=DEFAULT_TARGET,
                        help=f"URL do servidor Radxa na Tailscale (padrao: {DEFAULT_TARGET})")
    parser.add_argument("-p", "--port", type=int, default=DEFAULT_PORT,
                        help=f"Porta local de escuta (padrao: {DEFAULT_PORT})")
    parser.add_argument("-b", "--bind", default="0.0.0.0",
                        help="Endereco de bind (padrao: 0.0.0.0)")

    args = parser.parse_args()

    RelayHandler.target_base = args.target

    server_address = (args.bind, args.port)
    httpd = ThreadingHTTPServer(server_address, RelayHandler)

    print("=" * 60)
    print("   🎙️  MPS3 Mobile Relay Proxy - Ativo!")
    print(f"   Escutando em: http://{args.bind}:{args.port}")
    print(f"   Destino VPN:  {args.target}")
    print("=" * 60)
    print("Mantenha o Hotspot e o Tailscale ativos no celular.")
    print("Pressione Ctrl+C para encerrar.\n")

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nEncerrando relay proxy...")
        httpd.server_close()


if __name__ == "__main__":
    main()

