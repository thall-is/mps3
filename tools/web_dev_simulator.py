#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
MPS3 Web UI & Podcast Scraper Local Simulator (tools/web_dev_simulator.py)
--------------------------------------------------------------------------
Emula 100% da API HTTP do firmware ESP32-S3 (`wifi_transfer.c`) em uma sessão local no PC,
usando `scratch/virtual_sdcard/` como cartão MicroSD real, sem precisar do ESP32 ligado!

Recursos:
1. Hot-Assembly de `components/wifi_transfer/web/src/`: basta dar F5 no navegador em
   `http://127.0.0.1:8090` para carregar qualquer edição feita em `src/`.
2. Emulação completa de `/api/status`, `/api/list`, `/api/mkdir`, `/api/upload`,
   `/api/download` (com suporte a HTTP 206 Range para o Player Web), `/api/move`,
   `/api/copy`, `/api/delete`, `/api/eq`, `/api/wifi/networks` e `/api/ota`.
3. Emulação fiel do protocolo transacional de Podcasts (`/api/sync/manifest`,
   `/api/sync/begin`, `/api/sync/verify` com SHA-256 real em `.staging`, `/api/sync/done`).
4. Endpoint opcional de diagnósticos/proxy local (`/api/dev/yt_proxy`) caso o usuário queira
   testar raspagem YouTube em rede com bloqueio de instâncias Piped/Invidious públicas.
"""

import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import mimetypes
import os
import re
import shutil
import socket
import sys
import time
from urllib.parse import parse_qs, quote, unquote, urlparse
import urllib.request

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.abspath(os.path.join(SCRIPT_DIR, ".."))
sys.path.insert(0, SCRIPT_DIR)

import pack_web  # noqa: E402
import yt_scraper_core  # noqa: E402


class VirtualSdState:
    @staticmethod
    def _detect_lan_ip():
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(("8.8.8.8", 80))
            ip = s.getsockname()[0]
            s.close()
            return ip
        except Exception:
            return "127.0.0.1"

    def __init__(self, root_dir, port=8090):
        self.root_dir = os.path.abspath(root_dir)
        self.port = port
        self.lan_ip = self._detect_lan_ip()
        os.makedirs(self.root_dir, exist_ok=True)
        os.makedirs(os.path.join(self.root_dir, "Podcasts"), exist_ok=True)
        os.makedirs(os.path.join(self.root_dir, "Musicas"), exist_ok=True)
        os.makedirs(os.path.join(self.root_dir, ".mps3_analytics"), exist_ok=True)
        self.played_ledger_path = os.path.join(self.root_dir, "Podcasts", ".played.txt")
        self.sd_status = "free"
        self.eq_config = {
            "enabled": True,
            "active_preset_idx": 0,
            "presets": [
                {"name": "Flat", "overall_gain": 0.0, "gains": [0] * 10},
                {"name": "Rock", "overall_gain": -1.5, "gains": [4, 3, 1, -1, -2, 0, 2, 3, 4, 4]},
            ],
        }
        self.wifi_networks = [{"ssid": "MPS3_Casa_5G"}, {"ssid": "Hotspot_Celular"}]

    def resolve_sd_path(self, rel_url_path):
        clean = unquote(rel_url_path or "/").replace("\\", "/").strip()
        if not clean.startswith("/"):
            clean = "/" + clean
        norm = os.path.normpath(clean.lstrip("/"))
        if norm in (".", ""):
            full = self.root_dir
        else:
            full = os.path.abspath(os.path.join(self.root_dir, norm))
        if not full.startswith(self.root_dir):
            raise ValueError("Path traversal bloqueado")
        return full, "/" + norm.replace("\\", "/") if norm not in (".", "") else "/"

    def get_played_set(self):
        if not os.path.exists(self.played_ledger_path):
            return set()
        with open(self.played_ledger_path, "r", encoding="utf-8", errors="ignore") as f:
            return {line.strip() for line in f if line.strip()}

    def scan_podcasts_manifest(self):
        pod_root = os.path.join(self.root_dir, "Podcasts")
        played_set = self.get_played_set()
        played_lower = {p.lower() for p in played_set}
        podcasts = []
        if os.path.isdir(pod_root):
            for root, dirs, files in os.walk(pod_root):
                dirs[:] = [d for d in dirs if not d.startswith(".")]
                for fname in sorted(files):
                    if fname.startswith(".") or not fname.lower().endswith((".mp3", ".m4a", ".aac", ".opus", ".ogg", ".wav")):
                        continue
                    fpath = os.path.join(root, fname)
                    rel_dir = os.path.relpath(root, pod_root).replace("\\", "/")
                    prog = rel_dir.split("/")[-1] if rel_dir != "." else "Geral"
                    rel_key = f"{prog}/{fname}"
                    rel_full_key = f"{rel_dir}/{fname}" if rel_dir != "." else fname
                    podcasts.append(
                        {
                            "program": prog,
                            "filename": fname,
                            "path": f"/Podcasts/{rel_full_key}",
                            "size": os.path.getsize(fpath),
                            "mtime": int(os.path.getmtime(fpath)),
                            "played": (rel_key.lower() in played_lower) or (fname.lower() in played_lower) or (rel_full_key.lower() in played_lower),
                        }
                    )
        return podcasts, sorted(played_set)


class SimulatorRequestHandler(BaseHTTPRequestHandler):
    sd: VirtualSdState = None
    paths: dict = None

    def log_message(self, fmt, *args):
        sys.stdout.write(f"[SIM {self.log_date_time_string()}] {fmt % args}\n")

    def _send_json(self, status, data, extra_headers=None):
        body = json.dumps(data, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Length", str(len(body)))
        if extra_headers:
            for k, v in extra_headers.items():
                self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "*")
        self.end_headers()

    def do_GET(self):
        parsed = urlparse(self.path)
        qs = parse_qs(parsed.query)

        # 1. Página Principal: Monta web/src/ em tempo real (Hot-Reload)
        if parsed.path in ("/", "/index.html"):
            assembled_html, _ = pack_web.assemble_from_src(
                self.paths["src_dir"], self.paths["template_path"]
            )
            raw = assembled_html.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)
            return

        # 2. Telemetria do Sistema (/api/status)
        if parsed.path == "/api/status":
            try:
                du = shutil.disk_usage(self.sd.root_dir)
                total_b = du.total
                free_b = du.free
            except Exception:
                total_b = 32 * 1024 * 1024 * 1024
                free_b = 16 * 1024 * 1024 * 1024
            return self._send_json(
                200,
                {
                    "total": total_b,
                    "free": free_b,
                    "battery_percent": 96,
                    "battery_mv": 4140,
                    "battery_charging": True,
                    "battery_time_left": 840,
                    "sd_status": self.sd.sd_status,
                    "ip": f"{self.sd.lan_ip}:{self.sd.port}",
                    "wifi_mode": "STA",
                },
            )

        # 3. Listagem de Diretórios (/api/list)
        if parsed.path == "/api/list":
            dir_param = qs.get("path", qs.get("dir", ["/"]))[0]
            try:
                full_dir, _ = self.sd.resolve_sd_path(dir_param)
            except ValueError as e:
                return self._send_json(400, {"error": str(e)})
            if not os.path.isdir(full_dir):
                return self._send_json(200, [])
            entries = []
            for name in sorted(os.listdir(full_dir)):
                if name.startswith("."):
                    continue
                fpath = os.path.join(full_dir, name)
                is_dir = os.path.isdir(fpath)
                entries.append(
                    {
                        "name": name,
                        "dir": is_dir,
                        "size": 0 if is_dir else os.path.getsize(fpath),
                    }
                )
            entries.sort(key=lambda x: (not x["dir"], x["name"].lower()))
            return self._send_json(200, entries)

        # 4. Download / Streaming com suporte a Range (/api/download)
        if parsed.path == "/api/download":
            req_path = qs.get("path", [""])[0]
            try:
                full_path, _ = self.sd.resolve_sd_path(req_path)
            except ValueError as e:
                return self._send_json(400, {"error": str(e)})
            if not os.path.isfile(full_path):
                return self._send_json(404, {"error": "Arquivo nao encontrado"})

            file_size = os.path.getsize(full_path)
            if full_path.lower().endswith(".jsonl"):
                mime = "application/json; charset=utf-8"
            else:
                mime, _ = mimetypes.guess_type(full_path)
                mime = mime or "application/octet-stream"
            dl_flag = qs.get("dl", ["0"])[0] == "1"
            range_hdr = self.headers.get("Range")

            if range_hdr and range_hdr.startswith("bytes="):
                rng = range_hdr.replace("bytes=", "").split("-")
                start = int(rng[0]) if rng[0] else 0
                end = int(rng[1]) if (len(rng) > 1 and rng[1]) else (file_size - 1)
                end = min(end, file_size - 1)
                length = max(0, end - start + 1)
                with open(full_path, "rb") as f:
                    f.seek(start)
                    chunk = f.read(length)
                self.send_response(206)
                self.send_header("Content-Type", mime)
                self.send_header("Access-Control-Allow-Origin", "*")
                self.send_header("Accept-Ranges", "bytes")
                self.send_header("Content-Range", f"bytes {start}-{end}/{file_size}")
                self.send_header("Content-Length", str(len(chunk)))
                self.end_headers()
                self.wfile.write(chunk)
                return

            with open(full_path, "rb") as f:
                data = f.read()
            self.send_response(200)
            self.send_header("Content-Type", mime)
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Accept-Ranges", "bytes")
            if dl_flag:
                self.send_header("Content-Disposition", f'attachment; filename="{os.path.basename(full_path)}"')
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return

        # 4b. Manifesto de Telemetria / Estatísticas (/api/analytics/manifest)
        if parsed.path == "/api/analytics/manifest":
            ana_dir = os.path.join(self.sd.root_dir, ".mps3_analytics")
            files = []
            if os.path.isdir(ana_dir):
                for fname in sorted(os.listdir(ana_dir)):
                    if fname.startswith("."):
                        continue
                    fp = os.path.join(ana_dir, fname)
                    if os.path.isfile(fp):
                        files.append(
                            {
                                "name": fname,
                                "download_path": f"/.mps3_analytics/{fname}",
                                "size": os.path.getsize(fp),
                                "mtime": int(os.path.getmtime(fp)),
                            }
                        )
            return self._send_json(200, {"status": "ok", "analytics_dir": "/.mps3_analytics", "files": files})

        # 5. Manifesto de Sincronização de Podcasts (/api/sync/manifest e /api/podcasts_local)
        if parsed.path in ("/api/sync/manifest", "/api/podcasts_local"):
            podcasts, played_history = self.sd.scan_podcasts_manifest()
            return self._send_json(
                200,
                {
                    "free_bytes": 16 * 1024 * 1024 * 1024,
                    "total_bytes": 32 * 1024 * 1024 * 1024,
                    "battery_pct": 96,
                    "charging": True,
                    "podcasts": podcasts,
                    "played_history": played_history,
                },
            )

        # 6. Endpoints auxiliares da UI (EQ, Wi-Fi, OTA)
        if parsed.path == "/api/eq":
            return self._send_json(200, self.sd.eq_config)
        if parsed.path == "/api/wifi/networks":
            return self._send_json(200, {"networks": self.sd.wifi_networks})
        if parsed.path == "/api/ota":
            return self._send_json(
                200,
                {
                    "running_version": "mps3_sim_local_v3.0",
                    "running_partition": "ota_0 (Simulador PC)",
                    "next_partition": "ota_1",
                    "battery_percent": 96,
                    "battery_charging": True,
                },
            )

        # 7. Ponte YouTube InnerTube Real embutida no Simulador Local (/channels/*, /c/*, /streams/*, /api/dev/yt_stream/*)
        if parsed.path.startswith("/channels/") or parsed.path.startswith("/c/"):
            ref = parsed.path.split("/", 2)[-1]
            req_tab = qs.get("tab", [None])[0]
            try:
                feed_data = yt_scraper_core.yt_fetch_channel_feed(ref, tab=req_tab)
                return self._send_json(200, feed_data)
            except Exception as e:
                print(f"[SIM ERROR /channels/] {type(e).__name__}: {e}")
                return self._send_json(502, {"error": str(e)})

        if parsed.path.startswith("/streams/"):
            vid = parsed.path.split("/")[-1].strip()
            host = self.headers.get("Host", "127.0.0.1:8090")
            stream_url = f"http://{host}/api/dev/yt_stream/{quote(vid)}.m4a"
            return self._send_json(
                200,
                {
                    "audioStreams": [
                        {
                            "url": stream_url,
                            "mimeType": 'audio/mp4; codecs="mp4a.40.2"',
                            "bitrate": 128000,
                        }
                    ]
                },
            )

        if parsed.path.startswith("/api/dev/yt_stream/"):
            vid = parsed.path.split("/")[-1].replace(".m4a", "").replace(".webm", "").strip()
            max_bytes = int(qs.get("max_bytes", ["0"])[0] or "0")
            try:
                t0 = time.time()
                mime, total_len, meta, frag_gen = yt_scraper_core.yt_stream_audio_fragments(
                    vid, max_bytes=max_bytes
                )
                self.send_response(200)
                self.send_header("Content-Type", mime.split(";")[0])
                self.send_header("Access-Control-Allow-Origin", "*")
                if total_len > 0:
                    self.send_header("Content-Length", str(total_len))
                self.end_headers()

                sent_bytes = 0
                for chunk in frag_gen:
                    self.wfile.write(chunk)
                    sent_bytes += len(chunk)
                dt = time.time() - t0
                print(
                    f"[SIM] /api/dev/yt_stream/{vid}.m4a -> {sent_bytes / (1024*1024):.2f} MB "
                    f"(client={meta['clientUsed']}, itag={meta['itag']}) enviado em {dt:.2f}s"
                )
                return
            except Exception as e:
                print(f"[SIM ERROR /api/dev/yt_stream/{vid}] {type(e).__name__}: {e}")
                return self._send_json(502, {"error": str(e)})

        # 8. Ponte RSS Proxy opcional para CDNs de áudio sem CORS (ex: CloudFront do Anchor.fm)
        if parsed.path == "/api/dev/rss_proxy":
            target_url = qs.get("url", [""])[0]
            if not target_url.startswith(("http://", "https://")):
                return self._send_json(400, {"error": "URL invalida para rss_proxy"})
            try:
                req = urllib.request.Request(
                    target_url,
                    headers={"User-Agent": "Mozilla/5.0 (MPS3-Podcast-Sync/2026.10)"},
                )
                with urllib.request.urlopen(req, timeout=30) as resp:
                    ctype = resp.headers.get("Content-Type", "audio/mpeg")
                    clen = resp.headers.get("Content-Length")
                    self.send_response(200)
                    self.send_header("Content-Type", ctype)
                    self.send_header("Access-Control-Allow-Origin", "*")
                    if clen:
                        self.send_header("Content-Length", clen)
                    self.end_headers()
                    while True:
                        chunk = resp.read(256 * 1024)
                        if not chunk:
                            break
                        self.wfile.write(chunk)
                return
            except Exception as e:
                print(f"[SIM ERROR /api/dev/rss_proxy] {type(e).__name__}: {e}")
                return self._send_json(502, {"error": str(e)})

        return self._send_json(404, {"error": f"Endpoint nao encontrado: {parsed.path}"})

    def do_PUT(self):
        parsed = urlparse(self.path)
        qs = parse_qs(parsed.query)
        length = int(self.headers.get("Content-Length", "0"))
        self.sd.sd_status = "writing"
        try:
            body = self.rfile.read(length) if length > 0 else b""
            if parsed.path == "/api/upload":
                req_path = qs.get("path", [""])[0]
                expected_sha = (qs.get("sha256", [""])[0] or self.headers.get("X-Expected-SHA256") or "").lower()
                full_path, _ = self.sd.resolve_sd_path(req_path)
                os.makedirs(os.path.dirname(full_path), exist_ok=True)
                actual_sha = hashlib.sha256(body).hexdigest().lower()
                if expected_sha and expected_sha != actual_sha:
                    return self._send_json(
                        422,
                        {"ok": False, "error": "SHA-256 divergente", "expected": expected_sha, "actual": actual_sha},
                    )
                with open(full_path, "wb") as f:
                    f.write(body)
                return self._send_json(
                    200,
                    {"ok": True, "size": len(body), "sha256": actual_sha},
                    extra_headers={"X-Content-SHA256": actual_sha},
                )
        finally:
            self.sd.sd_status = "free"
        return self._send_json(404, {"error": "PUT desconhecido"})

    def do_POST(self):
        parsed = urlparse(self.path)
        qs = parse_qs(parsed.query)
        length = int(self.headers.get("Content-Length", "0"))
        raw_body = self.rfile.read(length) if length > 0 else b""

        body_json = {}
        if raw_body:
            try:
                body_json = json.loads(raw_body.decode("utf-8"))
            except Exception:
                body_json = {}

        if parsed.path == "/api/mkdir":
            req_path = body_json.get("path") or qs.get("path", [""])[0]
            full_path, _ = self.sd.resolve_sd_path(req_path)
            os.makedirs(full_path, exist_ok=True)
            return self._send_json(200, {"status": "ok", "ok": True})

        if parsed.path in ("/api/move", "/api/rename"):
            src = (
                body_json.get("old_path")
                or body_json.get("src")
                or body_json.get("src_path")
                or qs.get("src", qs.get("old_path", qs.get("old", [""])))[0]
            )
            dst = (
                body_json.get("new_path")
                or body_json.get("dst")
                or body_json.get("dest_path")
                or qs.get("dst", qs.get("new_path", qs.get("new", [""])))[0]
            )
            full_src, _ = self.sd.resolve_sd_path(src)
            full_dst, _ = self.sd.resolve_sd_path(dst)
            if not os.path.exists(full_src):
                return self._send_json(404, {"ok": False, "error": "origem nao encontrada"})
            os.makedirs(os.path.dirname(full_dst), exist_ok=True)
            if os.path.isfile(full_dst) and os.path.abspath(full_src) != os.path.abspath(full_dst):
                os.remove(full_dst)
            shutil.move(full_src, full_dst)
            return self._send_json(200, {"status": "ok", "ok": True})

        if parsed.path == "/api/copy":
            src = body_json.get("src_path") or body_json.get("src") or qs.get("src", [""])[0]
            dst = body_json.get("dest_path") or body_json.get("dst") or qs.get("dst", [""])[0]
            full_src, _ = self.sd.resolve_sd_path(src)
            full_dst, _ = self.sd.resolve_sd_path(dst)
            if not os.path.exists(full_src):
                return self._send_json(404, {"ok": False, "error": "origem nao encontrada"})
            os.makedirs(os.path.dirname(full_dst), exist_ok=True)
            if os.path.isdir(full_src):
                shutil.copytree(full_src, full_dst, dirs_exist_ok=True)
            else:
                shutil.copy2(full_src, full_dst)
            return self._send_json(200, {"status": "ok", "ok": True})

        if parsed.path == "/api/delete":
            target = body_json.get("path") or qs.get("path", [""])[0]
            full_target, _ = self.sd.resolve_sd_path(target)
            if os.path.isdir(full_target):
                shutil.rmtree(full_target, ignore_errors=True)
            elif os.path.isfile(full_target):
                os.remove(full_target)
            return self._send_json(200, {"status": "ok", "ok": True})

        if parsed.path == "/api/sync/begin":
            staging_dir = os.path.join(self.sd.root_dir, "Podcasts", ".staging")
            if os.path.isdir(staging_dir):
                shutil.rmtree(staging_dir, ignore_errors=True)
            os.makedirs(staging_dir, exist_ok=True)
            print("[SIM] /api/sync/begin -> /Podcasts/.staging limpo e pronto.")
            return self._send_json(200, {"ok": True})

        if parsed.path == "/api/sync/verify":
            req = json.loads(raw_body.decode("utf-8") or "{}")
            path = req.get("path", "")
            exp_sha = (req.get("expected_sha256") or "").lower()
            exp_size = req.get("expected_size", -1)
            full_path, _ = self.sd.resolve_sd_path(path)
            if not os.path.isfile(full_path):
                return self._send_json(404, {"ok": False, "verified": False, "error": "Arquivo ausente"})
            with open(full_path, "rb") as f:
                data = f.read()
            act_sha = hashlib.sha256(data).hexdigest().lower()
            verified = (len(data) == exp_size) and (act_sha == exp_sha)
            print(
                f"[SIM] /api/sync/verify -> {os.path.basename(full_path)} | {len(data)} bytes | SHA-256 verified={verified}"
            )
            return self._send_json(
                200 if verified else 422,
                {"ok": verified, "verified": verified, "size": len(data), "sha256": act_sha},
            )

        if parsed.path == "/api/sync/done":
            info = json.loads(raw_body.decode("utf-8") or "{}")
            print(f"[SIM] /api/sync/done -> Sessao concluida: {info}")
            return self._send_json(200, {"ok": True})

        if parsed.path == "/api/yt/innertube":
            raw_txt = raw_body.decode("utf-8", errors="ignore")
            if "\n" in raw_txt:
                meta_str, post_str = raw_txt.split("\n", 1)
            else:
                meta_str, post_str = raw_txt, ""
            meta = json.loads(meta_str or "{}")
            target_url = meta.get("url", "")
            method = (meta.get("method") or ("POST" if post_str else "GET")).upper()
            headers = {
                "User-Agent": meta.get("ua") or yt_scraper_core.CONFIG["web_ua"],
                "Accept-Language": "pt-BR,pt;q=0.9,en-US;q=0.8",
            }
            if "youtube.com" in target_url:
                headers["Origin"] = "https://www.youtube.com"
            if meta.get("client_name"):
                headers["X-YouTube-Client-Name"] = str(meta["client_name"])
            if meta.get("client_version"):
                headers["X-YouTube-Client-Version"] = str(meta["client_version"])
            if meta.get("visitor_id"):
                headers["X-Goog-Visitor-Id"] = str(meta["visitor_id"])
            if method == "POST":
                headers["Content-Type"] = "application/json"
            try:
                req = urllib.request.Request(
                    target_url,
                    data=post_str.encode("utf-8") if method == "POST" else None,
                    headers=headers,
                    method=method,
                )
                with urllib.request.urlopen(req, timeout=20) as resp:
                    payload = resp.read()
                self.send_response(200)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Access-Control-Allow-Origin", "*")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
                return
            except Exception as e:
                return self._send_json(502, {"error": str(e)})

        if parsed.path == "/api/sync/pull_direct":
            target_url = body_json.get("url", "")
            staging_path = body_json.get("staging_path", "")
            ua = body_json.get("ua") or yt_scraper_core.CONFIG["web_ua"]
            append = bool(body_json.get("append"))
            total_bytes = int(body_json.get("total_bytes") or 0)
            offset_bytes = int(body_json.get("offset_bytes") or 0)
            try:
                full_dst, _ = self.sd.resolve_sd_path(staging_path)
                os.makedirs(os.path.dirname(full_dst), exist_ok=True)
                req_hdrs = {"User-Agent": ua}
                if body_json.get("range"):
                    req_hdrs["Range"] = str(body_json["range"])
                req = urllib.request.Request(target_url, headers=req_hdrs)
                self.send_response(200)
                self.send_header("Content-Type", "application/x-ndjson; charset=utf-8")
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                written = 0
                t0 = time.time()
                with urllib.request.urlopen(req, timeout=30) as resp, open(
                    full_dst, "ab" if append else "wb"
                ) as fp:
                    while True:
                        blk = resp.read(256 * 1024)
                        if not blk:
                            break
                        fp.write(blk)
                        written += len(blk)
                        dt = max(0.05, time.time() - t0)
                        kbs = (written / 1024.0) / dt
                        line = json.dumps(
                            {
                                "rx": offset_bytes + written,
                                "total": total_bytes or (offset_bytes + written),
                                "speed_kbs": round(kbs, 1),
                            }
                        ) + "\n"
                        self.wfile.write(line.encode("utf-8"))
                done_line = json.dumps(
                    {
                        "done": True,
                        "status": "ok",
                        "bytes": written,
                        "total": offset_bytes + written,
                    }
                ) + "\n"
                self.wfile.write(done_line.encode("utf-8"))
                return
            except Exception as e:
                err_line = json.dumps({"done": True, "status": "error", "error": str(e)}) + "\n"
                self.wfile.write(err_line.encode("utf-8"))
                return

        if parsed.path == "/api/eq":
            self.sd.eq_config = json.loads(raw_body.decode("utf-8") or "{}")
            return self._send_json(200, {"ok": True})

        return self._send_json(404, {"error": f"POST desconhecido: {parsed.path}"})

    def do_DELETE(self):
        return self.do_POST()


def run_simulator(port=8090, sd_dir=None):
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")

    paths = pack_web.get_paths(PROJECT_ROOT)
    if sd_dir is None:
        sd_dir = os.path.join(PROJECT_ROOT, "scratch", "virtual_sdcard")

    sd_state = VirtualSdState(sd_dir, port=port)
    SimulatorRequestHandler.sd = sd_state
    SimulatorRequestHandler.paths = paths

    server = ThreadingHTTPServer(("0.0.0.0", port), SimulatorRequestHandler)
    print("======================================================================")
    print("  MPS3 LOCAL WEB SIMULATOR & PODCAST SCRAPER SANDBOX")
    print("======================================================================")
    print(f"  URL no PC          : http://127.0.0.1:{port}")
    print(f"  Ponte Smartphone   : http://{sd_state.lan_ip}:{port}")
    print(f"  Cartao SD Virtual  : {sd_state.root_dir}")
    print(f"  Codigo-Fonte (Live): {paths['src_dir']}")
    print("======================================================================")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[SIM] Encerrando simulador...")
        server.server_close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="MPS3 Local Web UI & Podcast Scraper Simulator")
    parser.add_argument("--port", type=int, default=8090, help="Porta HTTP local (padrao: 8090)")
    parser.add_argument("--sd-dir", type=str, default=None, help="Diretorio do cartao SD virtual")
    args = parser.parse_args()
    run_simulator(port=args.port, sd_dir=args.sd_dir)

