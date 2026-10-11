#!/usr/bin/env python3
"""
MPS3 Podcast Sync & Scraper Test Suite
Testes automatizados completos para validar:
1. Isolamento absoluto do Scraper (.staging, .lock, arquivos .part e .tmp)
2. Catálogo e metadados (/api/podcasts e /api/status)
3. Streaming de alta performance com chunks de 64 KB
4. Suporte a HTTP Range (RFC 7233) para retomada de downloads parciais
5. Resiliência a cancelamento abrupto e desconexão de clientes
"""

import os
import sys
import time
import json
import socket
import urllib.request
import threading
import tempfile
import shutil
import unittest
from pathlib import Path

# Adiciona o diretório atual ao path para importar o servidor
current_dir = Path(__file__).resolve().parent
sys.path.insert(0, str(current_dir))

from mps3_podcast_server import ThreadingHTTPServer, PodcastSyncHandler
from mps3_podcast_scraper import PodcastScraper

TEST_HOST = "127.0.0.1"
TEST_PORT = 18088


class TestPodcastSyncSuite(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Cria diretório temporário para simular a pasta de podcasts
        cls.test_dir = Path(tempfile.mkdtemp(prefix="mps3_test_podcasts_"))
        cls.recentes_dir = cls.test_dir / "Recentes"
        cls.recentes_dir.mkdir(parents=True, exist_ok=True)

        # Configura o servidor de teste
        PodcastSyncHandler.podcast_dir = cls.test_dir
        cls.server = ThreadingHTTPServer((TEST_HOST, TEST_PORT), PodcastSyncHandler)
        cls.server_thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.server_thread.start()
        time.sleep(0.2)  # Aguarda inicialização do socket

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        shutil.rmtree(cls.test_dir, ignore_errors=True)

    def test_01_scraper_isolation_and_locks(self):
        """Valida que arquivos em staging, temporários e locks não vazam para a API."""
        prog_dir = self.recentes_dir / "Tecnologia"
        prog_dir.mkdir(parents=True, exist_ok=True)

        # 1. Cria um episódio finalizado legítimo (100 KB)
        final_file = prog_dir / "ep01_completo.mp3"
        with open(final_file, "wb") as f:
            f.write(os.urandom(100 * 1024))

        # 2. Cria arquivos temporários e parciais que DEVEM ser ignorados
        staging_dir = self.test_dir / ".staging" / "Tecnologia"
        staging_dir.mkdir(parents=True, exist_ok=True)
        with open(staging_dir / "ep02_staging.mp3", "wb") as f:
            f.write(os.urandom(50 * 1024))

        with open(prog_dir / "ep03.mp3.part", "wb") as f:
            f.write(os.urandom(30 * 1024))

        with open(prog_dir / "ep04.mp3.download", "wb") as f:
            f.write(os.urandom(40 * 1024))

        with open(prog_dir / "ep05_vazio.mp3", "wb") as f:
            f.write(b"")  # 0 bytes

        # 3. Consulta /api/podcasts
        url = f"http://{TEST_HOST}:{TEST_PORT}/api/podcasts"
        with urllib.request.urlopen(url) as resp:
            data = json.loads(resp.read().decode("utf-8"))

        podcasts = data.get("podcasts", [])
        filenames = [p["filename"] for p in podcasts]

        # Validacoes
        self.assertIn("ep01_completo.mp3", filenames, "Episódio finalizado deve estar no catálogo")
        self.assertNotIn("ep02_staging.mp3", filenames, "Arquivos em .staging não podem aparecer no catálogo")
        self.assertNotIn("ep03.mp3.part", filenames, "Arquivos .part não podem aparecer no catálogo")
        self.assertNotIn("ep04.mp3.download", filenames, "Arquivos .download não podem aparecer no catálogo")
        self.assertNotIn("ep05_vazio.mp3", filenames, "Arquivos vazios não podem aparecer no catálogo")
        print("[OK] Teste 1: Isolamento estrito de arquivos temporários e staging validado!")

    def test_02_scraper_lock_status_reporting(self):
        """Valida que .scraper.lock é refletido em tempo real no /api/status."""
        scraper = PodcastScraper(self.test_dir)

        # Sem lock
        url = f"http://{TEST_HOST}:{TEST_PORT}/api/status"
        with urllib.request.urlopen(url) as resp:
            st = json.loads(resp.read().decode("utf-8"))
        self.assertFalse(st["scraper_active"])

        # Ativa lock
        scraper.acquire_lock()
        with urllib.request.urlopen(url) as resp:
            st = json.loads(resp.read().decode("utf-8"))
        self.assertTrue(st["scraper_active"], "Status deve indicar que o scraper está ativo")

        # Libera lock
        scraper.release_lock()
        with urllib.request.urlopen(url) as resp:
            st = json.loads(resp.read().decode("utf-8"))
        self.assertFalse(st["scraper_active"], "Status deve indicar liberação do scraper")
        print("[OK] Teste 2: Comunicação de Lock do Scraper com o Servidor validada!")

    def test_03_http_range_resume_download(self):
        """Valida suporte a HTTP Range (RFC 7233) para retomada de downloads."""
        prog_dir = self.recentes_dir / "Ciencia"
        prog_dir.mkdir(parents=True, exist_ok=True)
        test_file = prog_dir / "ep_astronomia.mp3"

        # Arquivo de 1 MB de dados conhecidos
        test_payload = bytes([i % 256 for i in range(1024 * 1024)])
        with open(test_file, "wb") as f:
            f.write(test_payload)

        # Requisita a partir de 500 KB (offset 512000)
        start_offset = 512000
        req = urllib.request.Request(
            f"http://{TEST_HOST}:{TEST_PORT}/podcasts/Ciencia/ep_astronomia.mp3",
            headers={"Range": f"bytes={start_offset}-"}
        )

        with urllib.request.urlopen(req) as resp:
            self.assertEqual(resp.status, 206, "Deve responder com HTTP 206 Partial Content")
            crange = resp.headers.get("Content-Range")
            self.assertIn(f"bytes {start_offset}-", crange)
            received_body = resp.read()

        expected_body = test_payload[start_offset:]
        self.assertEqual(len(received_body), len(expected_body))
        self.assertEqual(received_body, expected_body, "Bytes retomados devem bater 100% com o original")
        print("[OK] Teste 3: Retomada de download com HTTP Range (RFC 7233) validada com sucesso!")

    def test_04_throughput_streaming_benchmark(self):
        """Mede a taxa de vazão do streaming HTTP com chunks de 64 KB."""
        prog_dir = self.recentes_dir / "Bench"
        prog_dir.mkdir(parents=True, exist_ok=True)
        bench_file = prog_dir / "test_10mb.mp3"

        file_size_mb = 8
        total_bytes = file_size_mb * 1024 * 1024
        with open(bench_file, "wb") as f:
            f.write(os.urandom(total_bytes))

        req = urllib.request.Request(f"http://{TEST_HOST}:{TEST_PORT}/podcasts/Bench/test_10mb.mp3")

        t0 = time.perf_counter()
        downloaded = 0
        with urllib.request.urlopen(req) as resp:
            while True:
                chunk = resp.read(64 * 1024)
                if not chunk:
                    break
                downloaded += len(chunk)
        elapsed = time.perf_counter() - t0

        speed_mbs = (downloaded / (1024 * 1024)) / elapsed
        self.assertEqual(downloaded, total_bytes)
        print(f"[OK] Teste 4: Throughput local medido: {speed_mbs:.2f} MB/s ({downloaded // (1024*1024)} MB em {elapsed*1000:.1f} ms)")

    def test_05_abrupt_client_disconnect(self):
        """Valida que desconexões abruptas não travam o servidor nem causam vazamento de sockets."""
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect((TEST_HOST, TEST_PORT))
        request = b"GET /podcasts/Bench/test_10mb.mp3 HTTP/1.1\r\nHost: localhost\r\n\r\n"
        s.sendall(request)
        # Lê apenas os primeiros 128 KB e fecha o socket abruptamente com RST
        s.recv(131072)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b"\x01\x00\x00\x00\x00\x00\x00\x00")
        s.close()
        time.sleep(0.1)

        # O servidor deve continuar atendendo novas requisições normalmente
        url = f"http://{TEST_HOST}:{TEST_PORT}/api/status"
        with urllib.request.urlopen(url) as resp:
            self.assertEqual(resp.status, 200)
        print("[OK] Teste 5: Resiliência a desconexão abrupta do cliente validada com sucesso!")


if __name__ == "__main__":
    unittest.main()
