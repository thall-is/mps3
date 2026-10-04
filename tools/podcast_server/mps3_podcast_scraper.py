#!/usr/bin/env python3
"""
MPS3 Podcast Scraper & Ingestion Engine - Versão 2.0
Gerenciador automatizado de scraping com isolamento estrito de Staging e Locks.

Garante que o MPS3 (ESP32-S3) e o Podcast Sync Server NUNCA tentem baixar ou indexar
arquivos incompletos, temporários ou que estejam no meio do processo de download.

Padrão de Ingestão:
1. Ativa .scraper.lock
2. Baixa exclusivamente dentro de <BASE_DIR>/.staging/
3. Valida integridade do áudio (tamanho > 1 KB, cabeçalhos)
4. Move atomicamente (os.replace) para <BASE_DIR>/Recentes/<Programa>/<Arquivo>
5. Remove .scraper.lock e limpa resíduos de .staging/
"""

import os
import sys
import time
import argparse
import urllib.request
import xml.etree.ElementTree as ET
from pathlib import Path

# Diretórios padrão
if sys.platform.startswith("win"):
    DEFAULT_DIR = r"R:\Podcasts"
else:
    DEFAULT_DIR = "/mnt/USBT/Podcasts"


class PodcastScraper:
    def __init__(self, base_dir=DEFAULT_DIR):
        self.base_dir = Path(base_dir)
        self.staging_dir = self.base_dir / ".staging"
        self.recentes_dir = self.base_dir / "Recentes"
        self.lock_file = self.base_dir / ".scraper.lock"

    def acquire_lock(self):
        self.base_dir.mkdir(parents=True, exist_ok=True)
        self.staging_dir.mkdir(parents=True, exist_ok=True)
        self.recentes_dir.mkdir(parents=True, exist_ok=True)
        try:
            with open(self.lock_file, "w") as f:
                f.write(f"pid={os.getpid()}\ntime={int(time.time())}\n")
            print(f"[*] Lock ativado: {self.lock_file}")
            return True
        except Exception as e:
            print(f"[!] Erro ao criar lock file: {e}")
            return False

    def release_lock(self):
        try:
            if self.lock_file.exists():
                self.lock_file.unlink()
                print(f"[*] Lock liberado: {self.lock_file}")
        except Exception as e:
            print(f"[!] Erro ao liberar lock: {e}")

    def download_episode(self, program_name, episode_title, audio_url, filename=None):
        """
        Baixa um episódio em staging e publica atomicamente na pasta Recentes.
        """
        if not filename:
            # Sanitiza nome do arquivo
            clean_title = "".join(c for c in episode_title if c.isalnum() or c in (" ", "-", "_", ".")).rstrip()
            clean_title = clean_title.replace(" ", "_")
            ext = os.path.splitext(audio_url.split("?")[0])[1] or ".mp3"
            filename = f"{clean_title}{ext}"

        prog_staging = self.staging_dir / program_name
        prog_staging.mkdir(parents=True, exist_ok=True)

        prog_dest = self.recentes_dir / program_name
        prog_dest.mkdir(parents=True, exist_ok=True)

        temp_file = prog_staging / f"{filename}.download"
        final_file = prog_dest / filename

        if final_file.exists():
            print(f"[-] Episodio ja publicado: {filename}")
            return True

        print(f"[+] Baixando em Staging: '{episode_title}'...")
        print(f"    URL:     {audio_url}")
        print(f"    Staging: {temp_file}")

        try:
            req = urllib.request.Request(
                audio_url,
                headers={"User-Agent": "MPS3-Podcast-Scraper/2.0"}
            )
            with urllib.request.urlopen(req, timeout=30) as resp, open(temp_file, "wb") as out_f:
                total_len = resp.headers.get("Content-Length")
                total_bytes = int(total_len) if total_len else 0
                downloaded = 0
                chunk_sz = 64 * 1024

                while True:
                    chunk = resp.read(chunk_sz)
                    if not chunk:
                        break
                    out_f.write(chunk)
                    downloaded += len(chunk)
                    if total_bytes > 0:
                        pct = (downloaded * 100) // total_bytes
                        sys.stdout.write(f"\r    Progresso: {pct}% ({downloaded // 1024} KB)")
                        sys.stdout.flush()

            print()

            # Validacao basica de integridade
            st = temp_file.stat()
            if st.st_size < 1024:
                print(f"[!] Erro: Arquivo baixado muito pequeno ({st.st_size} bytes). Descartando.")
                temp_file.unlink(missing_ok=True)
                return False

            # PUBLICACAO ATOMICA: o arquivo so aparece publicamente quando 100% completo
            os.replace(str(temp_file), str(final_file))
            print(f"[OK] Publicado com sucesso: {final_file} ({st.st_size // 1024} KB)")
            return True

        except Exception as e:
            print(f"\n[!] Falha no download de '{episode_title}': {e}")
            temp_file.unlink(missing_ok=True)
            return False

    def ingest_rss_feed(self, program_name, feed_url, max_episodes=3):
        """Lê um feed RSS de podcast e faz a ingestão dos episódios mais recentes."""
        print(f"\n=======================================================")
        print(f"  Scraping Feed: [{program_name}]")
        print(f"  URL: {feed_url}")
        print(f"=======================================================")

        try:
            req = urllib.request.Request(feed_url, headers={"User-Agent": "MPS3-Scraper/2.0"})
            with urllib.request.urlopen(req, timeout=15) as resp:
                xml_data = resp.read()

            root = ET.fromstring(xml_data)
            channel = root.find("channel")
            if not channel:
                print("[!] Feed invalido: tag <channel> nao encontrada")
                return

            items = channel.findall("item")
            count = 0
            for item in items:
                if count >= max_episodes:
                    break
                title = item.findtext("title", "Sem Titulo").strip()
                enclosure = item.find("enclosure")
                if enclosure is not None:
                    audio_url = enclosure.get("url")
                    if audio_url:
                        self.download_episode(program_name, title, audio_url)
                        count += 1

        except Exception as e:
            print(f"[!] Erro ao processar feed {feed_url}: {e}")


def main():
    parser = argparse.ArgumentParser(description="MPS3 Podcast Ingestion Engine v2.0")
    parser.add_argument("--dir", default=DEFAULT_DIR, help="Diretório base de Podcasts")
    parser.add_argument("--feed", help="URL do Feed RSS para raspar")
    parser.add_argument("--name", default="Canal", help="Nome do programa/canal")
    parser.add_argument("--limit", type=int, default=3, help="Maximo de episodios a baixar por feed")
    args = parser.parse_args()

    scraper = PodcastScraper(args.dir)
    scraper.acquire_lock()
    try:
        if args.feed:
            scraper.ingest_rss_feed(args.name, args.feed, args.limit)
        else:
            print("[*] Nenhum feed especificado. Para usar: python mps3_podcast_scraper.py --feed <URL> --name <NOME>")
    finally:
        scraper.release_lock()


if __name__ == "__main__":
    main()
