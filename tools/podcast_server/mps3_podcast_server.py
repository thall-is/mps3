#!/usr/bin/env python3
"""
MPS3 Podcast Sync Server
Servidor HTTP leve para sincronização automatizada de podcasts entre a SBC
Radxa Rock 3E (Ubuntu Server) e o reprodutor de áudio Hi-Res MPS3 (ESP32-S3).

Recursos:
- Catálogo JSON com metadados de episódios novos (/api/podcasts).
- Streaming de áudio com suporte a HTTP Range (RFC 7233) para retomada de downloads.
- Detecção e agrupamento por programa/canal (subpastas de 'Recentes').
- Zero dependências externas (Python 3 standard library pura).
"""

import os
import sys
import json
import time
import urllib.parse
import mimetypes
import hashlib
from pathlib import Path
from http.server import HTTPServer, BaseHTTPRequestHandler
import socketserver

DEFAULT_PORT = 8088
DEFAULT_HOST = "0.0.0.0"

# Se executado no Windows, usa R:\Podcasts como padrão; no Linux, /mnt/USBT/Podcasts
if sys.platform.startswith("win"):
    DEFAULT_DIR = r"R:\Podcasts"
else:
    DEFAULT_DIR = "/mnt/USBT/Podcasts"

SUPPORTED_EXTENSIONS = {".mp3", ".flac", ".wav", ".m4a", ".ogg", ".opus", ".aac"}
CHUNK_SIZE = 64 * 1024  # 64 KB por bloco de streaming


class ThreadingHTTPServer(socketserver.ThreadingMixIn, HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


class PodcastSyncHandler(BaseHTTPRequestHandler):
    podcast_dir = Path(DEFAULT_DIR)

    def log_message(self, format, *args):
        # Log simplificado com timestamp
        sys.stderr.write(f"[{time.strftime('%Y-%m-%d %H:%M:%S')}] {self.client_address[0]} - {format % args}\n")

    def send_cors_headers(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS, HEAD")
        self.send_header("Access-Control-Allow-Headers", "*")
        self.send_header("Access-Control-Allow-Private-Network", "true")

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_cors_headers()
        self.send_header("Access-Control-Max-Age", "86400")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        query = urllib.parse.parse_qs(parsed.query)

        if path == "/api/status":
            self.handle_api_status()
        elif path == "/api/podcasts":
            self.handle_api_podcasts(query)
        elif path.startswith("/podcasts/") or path == "/download":
            self.handle_file_download(path, query)
        else:
            self.send_error(404, "Endpoint nao encontrado")

    def handle_api_status(self):
        """Retorna o status do servidor e estatísticas básicas de armazenamento."""
        base_path = self.podcast_dir
        recentes_path = base_path / "Recentes" if (base_path / "Recentes").exists() else base_path

        stat_info = {
            "server": "mps3-podcast-sync",
            "version": "1.0.0",
            "server_time": int(time.time()),
            "server_time_str": time.strftime("%Y-%m-%d %H:%M:%S"),
            "base_dir": str(base_path),
            "recentes_exists": recentes_path.exists(),
        }

        try:
            stat = os.statvfs(str(base_path))
            free_bytes = stat.f_bavail * stat.f_frsize
            total_bytes = stat.f_blocks * stat.f_frsize
            stat_info["storage_free_mb"] = free_bytes // (1024 * 1024)
            stat_info["storage_total_mb"] = total_bytes // (1024 * 1024)
        except Exception:
            pass

        data = json.dumps(stat_info, indent=2).encode("utf-8")
        self.send_response(200)
        self.send_cors_headers()
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def handle_api_podcasts(self, query):
        """Retorna o catálogo de episódios de podcasts em JSON."""
        base_dir = self.podcast_dir
        scan_dir = base_dir / "Recentes" if (base_dir / "Recentes").is_dir() else base_dir

        limit = int(query.get("limit", [50])[0])
        podcasts = []

        if scan_dir.exists():
            for root, _, files in os.walk(scan_dir):
                root_path = Path(root)
                rel_to_scan = root_path.relative_to(scan_dir)
                program_name = rel_to_scan.parts[0] if rel_to_scan.parts else "Geral"

                for f in sorted(files):
                    file_path = root_path / f
                    ext = file_path.suffix.lower()
                    if ext in SUPPORTED_EXTENSIONS:
                        try:
                            stat = file_path.stat()
                            # Caminho relativo a partir de scan_dir
                            rel_file_path = file_path.relative_to(scan_dir).as_posix()
                            file_id = hashlib.md5(rel_file_path.encode("utf-8")).hexdigest()[:12]

                            podcasts.append({
                                "id": file_id,
                                "program": program_name,
                                "title": file_path.stem,
                                "filename": f,
                                "rel_path": rel_file_path,
                                "size_bytes": stat.st_size,
                                "mtime": int(stat.st_mtime),
                                "mtime_str": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(stat.st_mtime)),
                                "ext": ext
                            })
                        except Exception as e:
                            self.log_message(f"Erro ao ler arquivo {file_path}: {e}")

        # Ordenar dos mais recentes para os mais antigos
        podcasts.sort(key=lambda p: p["mtime"], reverse=True)
        if limit > 0:
            podcasts = podcasts[:limit]

        response = {
            "server_time": int(time.time()),
            "count": len(podcasts),
            "podcasts": podcasts
        }

        data = json.dumps(response, indent=2, ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_cors_headers()
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def handle_file_download(self, path, query):
        """Serve o arquivo com suporte total a HTTP Range (retomada de download)."""
        base_dir = self.podcast_dir
        scan_dir = base_dir / "Recentes" if (base_dir / "Recentes").is_dir() else base_dir

        if path.startswith("/podcasts/"):
            rel_path = urllib.parse.unquote(path[len("/podcasts/"):])
        else:
            rel_path = query.get("path", [""])[0]
            rel_path = urllib.parse.unquote(rel_path)

        if not rel_path:
            self.send_error(400, "Parametro de caminho vazio")
            return

        # Previne directory traversal (..)
        target_file = (scan_dir / rel_path).resolve()
        try:
            target_file.relative_to(scan_dir.resolve())
        except ValueError:
            self.send_error(403, "Acesso negado fora do diretorio permitido")
            return

        if not target_file.is_file():
            self.send_error(404, "Arquivo nao encontrado")
            return

        file_size = target_file.stat().st_size
        content_type, _ = mimetypes.guess_type(str(target_file))
        if not content_type:
            content_type = "application/octet-stream"

        # Trata cabeçalho Range (ex: "bytes=1048576-")
        range_header = self.headers.get("Range")
        start_byte = 0
        end_byte = file_size - 1
        is_range = False

        if range_header and range_header.startswith("bytes="):
            try:
                ranges = range_header[len("bytes="):].strip().split("-")
                if ranges[0]:
                    start_byte = int(ranges[0])
                if len(ranges) > 1 and ranges[1]:
                    end_byte = int(ranges[1])
                if start_byte > end_byte or start_byte >= file_size:
                    self.send_response(416)  # Range Not Satisfiable
                    self.send_header("Content-Range", f"bytes */{file_size}")
                    self.end_headers()
                    return
                is_range = True
            except Exception:
                start_byte = 0
                end_byte = file_size - 1

        bytes_to_send = end_byte - start_byte + 1

        if is_range:
            self.send_response(206)  # Partial Content
            self.send_header("Content-Range", f"bytes {start_byte}-{end_byte}/{file_size}")
        else:
            self.send_response(200)

        self.send_cors_headers()
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(bytes_to_send))
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Last-Modified", self.date_time_string(target_file.stat().st_mtime))
        self.end_headers()

        # Streaming do arquivo
        try:
            with open(target_file, "rb") as f:
                if start_byte > 0:
                    f.seek(start_byte)
                sent = 0
                while sent < bytes_to_send:
                    read_len = min(CHUNK_SIZE, bytes_to_send - sent)
                    chunk = f.read(read_len)
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    sent += len(chunk)
        except (ConnectionResetError, BrokenPipeError):
            pass  # Cliente desconectou normalmente ou abortou download


def run_server(host=DEFAULT_HOST, port=DEFAULT_PORT, directory=DEFAULT_DIR):
    dir_path = Path(directory)
    if not dir_path.exists():
        print(f"[!] AVISO: Diretorio {dir_path} nao existe atualmente. O servidor aguardara sua criacao.")

    PodcastSyncHandler.podcast_dir = dir_path
    server = ThreadingHTTPServer((host, port), PodcastSyncHandler)
    print("=" * 60)
    print("  MPS3 PODCAST SYNC SERVER INICIADO")
    print(f"  Diretorio: {dir_path}")
    print(f"  Porta:     {port}")
    print(f"  Endpoints: http://{host}:{port}/api/status")
    print(f"             http://{host}:{port}/api/podcasts")
    print("=" * 60)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nEncerrando servidor...")
        server.server_close()


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description="MPS3 Podcast Sync Server")
    parser.add_argument("--dir", default=DEFAULT_DIR, help="Diretorio raiz dos podcasts")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="Porta TCP do servidor (padrao: 8088)")
    parser.add_argument("--host", default=DEFAULT_HOST, help="Host IP para bind (padrao: 0.0.0.0)")
    args = parser.parse_args()

    run_server(args.host, args.port, args.dir)

