"""
Cross-platform RSS & YouTube Podcast Downloader (`downloader.py`).

Cleanly ported from `mps3_podcast_scraper.py` using cross-platform `pathlib.Path`
paths (zero hardcoded OS paths), strict `.staging` isolation, atomic `os.replace()`
publication, FAT32-safe filename sanitization, and optional `yt-dlp` integration.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import time
import unicodedata
import urllib.request
import xml.etree.ElementTree as ET
from email.utils import parsedate_to_datetime
from pathlib import Path
from typing import List, Optional


FAT32_FORBIDDEN_CHARS = re.compile(r'[<>:"/\\|?*\x00-\x1f]')


def sanitize_filename(name: str, max_len: int = 120) -> str:
    """
    Normalizes a filename to Unicode NFC and strips characters forbidden on FAT32/Windows.
    """
    norm = unicodedata.normalize("NFC", str(name)).strip()
    cleaned = FAT32_FORBIDDEN_CHARS.sub("_", norm)
    cleaned = re.sub(r"\s+", " ", cleaned).strip(" .")
    if len(cleaned) > max_len:
        cleaned = cleaned[:max_len].rstrip(" .")
    return cleaned or "episode"


class PodcastDownloader:
    """
    Downloads podcast episodes from RSS feeds or YouTube channels into
    `<podcasts_dir>/.staging/<Show>/` and atomically publishes completed files
    into `<podcasts_dir>/<Show>/` (or `<podcasts_dir>/Recentes/<Show>/` if `Recentes` exists).
    """

    def __init__(self, podcasts_dir: Path | str):
        self.podcasts_dir = Path(podcasts_dir)
        self.staging_dir = self.podcasts_dir / ".staging"
        self.lock_file = self.podcasts_dir / ".scraper.lock"

    @property
    def target_root(self) -> Path:
        recentes = self.podcasts_dir / "Recentes"
        return recentes if recentes.is_dir() else self.podcasts_dir

    def acquire_lock(self) -> bool:
        self.podcasts_dir.mkdir(parents=True, exist_ok=True)
        self.staging_dir.mkdir(parents=True, exist_ok=True)
        try:
            self.lock_file.write_text(
                f"pid={os.getpid()}\ntime={int(time.time())}\n",
                encoding="utf-8",
            )
            return True
        except OSError:
            return False

    def release_lock(self) -> None:
        try:
            self.lock_file.unlink(missing_ok=True)
        except OSError:
            pass

    def download_episode(
        self,
        show_name: str,
        episode_title: str,
        audio_url: str,
        pub_date_prefix: Optional[str] = None,
        filename: Optional[str] = None,
    ) -> Optional[Path]:
        """
        Downloads a single audio file into `.staging/<Show>/<filename>.part`,
        validates minimum size (> 1 KB), and atomically moves it via `os.replace()`
        to `<target_root>/<Show>/<filename>`.
        """
        safe_show = sanitize_filename(show_name, max_len=64)
        if not filename:
            safe_title = sanitize_filename(episode_title, max_len=100)
            ext = os.path.splitext(audio_url.split("?")[0])[1].lower() or ".mp3"
            if ext not in (".mp3", ".m4a", ".aac", ".ogg", ".opus", ".flac", ".wav"):
                ext = ".mp3"
            if pub_date_prefix:
                filename = f"{pub_date_prefix}_{safe_title}{ext}"
            else:
                filename = f"{safe_title}{ext}"

        prog_staging = self.staging_dir / safe_show
        prog_staging.mkdir(parents=True, exist_ok=True)

        prog_dest = self.target_root / safe_show
        prog_dest.mkdir(parents=True, exist_ok=True)

        temp_file = prog_staging / f"{filename}.part"
        final_file = prog_dest / filename

        if final_file.is_file() and final_file.stat().st_size > 1024:
            return final_file

        try:
            req = urllib.request.Request(
                audio_url,
                headers={"User-Agent": "MPS3-Podcast-Sync/2.0"},
            )
            with urllib.request.urlopen(req, timeout=30) as resp, open(temp_file, "wb") as out_f:
                while True:
                    chunk = resp.read(64 * 1024)
                    if not chunk:
                        break
                    out_f.write(chunk)

            st = temp_file.stat()
            if st.st_size < 1024:
                temp_file.unlink(missing_ok=True)
                return None

            os.replace(str(temp_file), str(final_file))
            return final_file
        except Exception:
            temp_file.unlink(missing_ok=True)
            return None

    def ingest_rss_feed(
        self,
        show_name: str,
        feed_url: str,
        max_episodes: int = 3,
    ) -> List[Path]:
        """Parses an RSS feed and downloads up to `max_episodes` newest episodes."""
        downloaded_files: List[Path] = []
        self.acquire_lock()
        try:
            req = urllib.request.Request(
                feed_url,
                headers={"User-Agent": "MPS3-Podcast-Sync/2.0"},
            )
            with urllib.request.urlopen(req, timeout=20) as resp:
                xml_data = resp.read()

            root = ET.fromstring(xml_data)
            channel = root.find("channel")
            if channel is None:
                return downloaded_files

            count = 0
            for item in channel.findall("item"):
                if count >= max_episodes:
                    break
                title = (item.findtext("title") or "Episode").strip()
                pub_date_raw = (item.findtext("pubDate") or "").strip()
                date_prefix: Optional[str] = None
                if pub_date_raw:
                    try:
                        dt = parsedate_to_datetime(pub_date_raw)
                        date_prefix = dt.strftime("%Y-%m-%d")
                    except Exception:
                        date_prefix = None

                enclosure = item.find("enclosure")
                if enclosure is not None:
                    audio_url = enclosure.get("url")
                    if audio_url:
                        res = self.download_episode(
                            show_name=show_name,
                            episode_title=title,
                            audio_url=audio_url,
                            pub_date_prefix=date_prefix,
                        )
                        if res is not None:
                            downloaded_files.append(res)
                            count += 1
        finally:
            self.release_lock()
        return downloaded_files

    def ingest_youtube_url(
        self,
        show_name: str,
        channel_or_playlist_url: str,
        max_episodes: int = 3,
    ) -> List[Path]:
        """
        Optional wrapper around `yt-dlp` (if installed on PATH) to download audio-only MP3
        episodes into `.staging` and move completed MP3s to `<target_root>/<Show>/`.
        """
        yt_dlp_bin = shutil.which("yt-dlp")
        if not yt_dlp_bin:
            raise RuntimeError("yt-dlp is not installed or not found on PATH")

        safe_show = sanitize_filename(show_name, max_len=64)
        prog_staging = self.staging_dir / safe_show
        prog_staging.mkdir(parents=True, exist_ok=True)
        prog_dest = self.target_root / safe_show
        prog_dest.mkdir(parents=True, exist_ok=True)

        published: List[Path] = []
        self.acquire_lock()
        try:
            output_tmpl = str(prog_staging / "%(upload_date)s_%(title).100s.%(ext)s")
            cmd = [
                yt_dlp_bin,
                "--extract-audio",
                "--audio-format",
                "mp3",
                "--playlist-end",
                str(max_episodes),
                "--no-part",
                "-o",
                output_tmpl,
                channel_or_playlist_url,
            ]
            subprocess.run(cmd, check=False, timeout=900)
            for mp3_file in sorted(prog_staging.glob("*.mp3")):
                if mp3_file.stat().st_size > 1024:
                    dest_file = prog_dest / sanitize_filename(mp3_file.name, max_len=140)
                    os.replace(str(mp3_file), str(dest_file))
                    published.append(dest_file)
        finally:
            self.release_lock()
        return published
