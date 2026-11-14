#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Camada 2 de Testes Automatizados — E2E Stateful Mock Server (tests/test_browser_podcast_e2e.py)
-----------------------------------------------------------------------------------------------
Sobe um servidor HTTP multi-thread local que simula simultaneamente:
1. O Backend C do ESP32-S3 (`wifi_transfer.c`):
   - `GET /` servindo o payload Gzip real de `index_html_gz.h`
   - `GET /api/sync/manifest`
   - `POST /api/sync/begin`
   - `PUT /api/upload?path=...&sha256=...`
   - `POST /api/sync/verify` (validação real de SHA-256 no SD simulado)
   - `POST /api/move` (promoção atômica de `.staging` para `/Podcasts/<Canal>/`)
   - `POST /api/delete` (limpeza de episódios já ouvidos fora do Top 3)
   - `POST /api/sync/done`
   - Persistência de `/Podcasts/.subscriptions.json`
2. O Pool de APIs YouTube (Piped/Invidious) e CDN de streams M4A:
   - `GET /channels/UC_XADREZ`
   - `GET /streams/<videoId>`
   - `GET /media/<videoId>.m4a`

Executa o motor JS real de `09_podcast_sync.js` contra o servidor e valida o estado final do SD!
"""

import gzip
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import pathlib
import re
import subprocess
import threading
import unittest
from urllib.parse import parse_qs, unquote, urlparse

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
JS_EXTRACTOR_PATH = os.path.join(
    PROJECT_ROOT, "components", "wifi_transfer", "web", "src", "js", "09a_yt_extractor.js"
)
JS_MODULE_PATH = os.path.join(
    PROJECT_ROOT, "components", "wifi_transfer", "web", "src", "js", "09_podcast_sync.js"
)
HEADER_PATH = os.path.join(
    PROJECT_ROOT, "components", "wifi_transfer", "include", "index_html_gz.h"
)


def extract_pure_logic_js():
    with open(JS_EXTRACTOR_PATH, "r", encoding="utf-8") as f:
        ext_js = f.read()
    with open(JS_MODULE_PATH, "r", encoding="utf-8") as f:
        full_js = f.read()
    marker = "// --- 11.6 ESTADO DA UI"
    idx = full_js.find(marker)
    return ext_js + "\n" + full_js[:idx]


class MockMps3AndYoutubeState:
    def __init__(self):
        self.sync_begun = False
        self.sync_done_summary = None
        # Simula arquivos em /Podcasts/... (path -> bytes)
        self.sd_files = {
            "/Podcasts/Xadrez Verbal/19700101 - Ep 2 Antigo Ouvido [yt_ep2].m4a": b"OLD_PLAYED_AUDIO",
            "/Podcasts/Xadrez Verbal/19700101 - Ep 1 Antigo Nao Ouvido [yt_ep1].m4a": b"OLD_UNPLAYED_AUDIO",
        }
        self.played_flags = {
            "/Podcasts/Xadrez Verbal/19700101 - Ep 2 Antigo Ouvido [yt_ep2].m4a": True,
            "/Podcasts/Xadrez Verbal/19700101 - Ep 1 Antigo Nao Ouvido [yt_ep1].m4a": False,
        }
        self.played_history = [
            "Xadrez Verbal/19700101 - Ep 4 Ja Ouvido [yt_ep4].m4a"
        ]
        # Streams de áudio no CDN simulado do YouTube
        self.yt_media = {
            "yt_ep5": b"FAKE_M4A_AUDIO_STREAM_EPISODE_5_" * 256,
            "yt_ep4": b"FAKE_M4A_AUDIO_STREAM_EPISODE_4_" * 256,
            "yt_ep3": b"FAKE_M4A_AUDIO_STREAM_EPISODE_3_" * 256,
        }
        self.innertube_calls = []
        self.pull_direct_calls = []
        self.innertube_enabled = False


class MockHandler(BaseHTTPRequestHandler):
    state: MockMps3AndYoutubeState = None
    base_url: str = ""

    def log_message(self, format, *args):
        pass  # Silencia logs HTTP durante os testes

    def _send_json(self, status, payload):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        parsed = urlparse(self.path)
        qs = parse_qs(parsed.query)

        if parsed.path == "/api/sync/manifest":
            podcasts = []
            for path, data in list(self.state.sd_files.items()):
                if not path.startswith("/Podcasts/"):
                    continue
                rel = path[len("/Podcasts/") :]
                if "/" not in rel or rel.startswith("."):
                    continue
                prog, fname = rel.split("/", 1)
                if fname.startswith("."):
                    continue
                podcasts.append(
                    {
                        "program": prog,
                        "filename": fname,
                        "path": path,
                        "size": len(data),
                        "played": self.state.played_flags.get(path, False),
                    }
                )
            return self._send_json(
                200,
                {
                    "free_bytes": 500 * 1024 * 1024,
                    "total_bytes": 1000 * 1024 * 1024,
                    "battery_pct": 92,
                    "charging": True,
                    "podcasts": podcasts,
                    "played_history": self.state.played_history,
                },
            )

        if parsed.path == "/api/download":
            req_path = unquote(qs.get("path", [""])[0])
            if req_path in self.state.sd_files:
                data = self.state.sd_files[req_path]
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
                return
            return self._send_json(404, {"error": "not found"})

        # Mock YouTube Piped API: GET /channels/UC_XADREZ
        if parsed.path == "/channels/UC_XADREZ":
            return self._send_json(
                200,
                {
                    "name": "Xadrez Verbal",
                    "relatedStreams": [
                        {"url": "/watch?v=yt_ep5", "title": "Ep 5 Inedito", "uploaded": 5000000, "duration": 600},
                        {"url": "/watch?v=yt_ep4", "title": "Ep 4 Ja Ouvido", "uploaded": 4000000, "duration": 600},
                        {"url": "/watch?v=yt_ep3", "title": "Ep 3 Inedito", "uploaded": 3000000, "duration": 600},
                        {"url": "/watch?v=yt_ep2", "title": "Ep 2 Antigo Ouvido", "uploaded": 2000000, "duration": 600},
                        {"url": "/watch?v=yt_ep1", "title": "Ep 1 Antigo Nao Ouvido", "uploaded": 1000000, "duration": 600},
                    ],
                },
            )

        # Mock YouTube Piped Stream Resolver: GET /streams/<vid>
        if parsed.path.startswith("/streams/"):
            vid = parsed.path.split("/")[-1]
            return self._send_json(
                200,
                {
                    "audioStreams": [
                        {
                            "url": f"{self.base_url}/media/{vid}.m4a",
                            "mimeType": "audio/mp4; codecs=\"mp4a.40.2\"",
                            "bitrate": 128000,
                        }
                    ]
                },
            )

        # Mock YouTube Media Stream Download: GET /media/<vid>.m4a
        if parsed.path.startswith("/media/") and parsed.path.endswith(".m4a"):
            vid = parsed.path.split("/")[-1].replace(".m4a", "")
            payload = self.state.yt_media.get(vid, b"DEFAULT_AUDIO")
            self.send_response(200)
            self.send_header("Content-Type", "audio/mp4")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            return

        # Mock Podcast RSS 2.0 XML Feed: GET /rss/bbc.xml
        if parsed.path == "/rss/bbc.xml":
            xml_body = f"""<?xml version="1.0" encoding="UTF-8"?>
            <rss version="2.0">
              <channel>
                <title>BBC Global News</title>
                <item>
                  <title>Global News: AI &amp; Chips</title>
                  <guid>bbc-ep-2026-10-20</guid>
                  <pubDate>Tue, 20 Oct 2026 01:00:00 GMT</pubDate>
                  <enclosure url="{self.base_url}/media/yt_ep5.m4a" length="8192" type="audio/mp4"/>
                </item>
              </channel>
            </rss>""".encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/rss+xml; charset=utf-8")
            self.send_header("Content-Length", str(len(xml_body)))
            self.end_headers()
            self.wfile.write(xml_body)
            return

        return self._send_json(404, {"error": "unknown endpoint"})

    def do_PUT(self):
        parsed = urlparse(self.path)
        qs = parse_qs(parsed.query)
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length) if length > 0 else b""

        if parsed.path == "/api/upload":
            dest_path = unquote(qs.get("path", [""])[0])
            expected_sha = qs.get("sha256", [""])[0].lower()
            actual_sha = hashlib.sha256(body).hexdigest().lower()
            if expected_sha and expected_sha != actual_sha:
                return self._send_json(400, {"ok": False, "error": "sha256 mismatch on upload"})
            self.state.sd_files[dest_path] = body
            return self._send_json(200, {"ok": True, "size": len(body), "sha256": actual_sha})

        return self._send_json(404, {"error": "unknown PUT"})

    def do_POST(self):
        parsed = urlparse(self.path)
        qs = parse_qs(parsed.query)
        length = int(self.headers.get("Content-Length", "0"))
        raw_body = self.rfile.read(length) if length > 0 else b""

        if parsed.path == "/api/sync/begin":
            self.state.sync_begun = True
            for k in list(self.state.sd_files.keys()):
                if k.startswith("/Podcasts/.staging/"):
                    del self.state.sd_files[k]
            return self._send_json(200, {"ok": True})

        if parsed.path == "/api/sync/verify":
            req = json.loads(raw_body.decode("utf-8") or "{}")
            path = req.get("path", "")
            exp_sha = (req.get("expected_sha256") or "").lower()
            exp_size = req.get("expected_size", -1)
            if path not in self.state.sd_files:
                return self._send_json(404, {"ok": False, "verified": False, "error": "missing"})
            data = self.state.sd_files[path]
            act_sha = hashlib.sha256(data).hexdigest().lower()
            verified = (len(data) == exp_size) and (act_sha == exp_sha)
            return self._send_json(
                200 if verified else 422,
                {"ok": verified, "verified": verified, "sha256": act_sha, "size": len(data)},
            )

        if parsed.path == "/api/move":
            body_json = json.loads(raw_body.decode("utf-8") or "{}") if raw_body else {}
            src = body_json.get("old_path") or body_json.get("src") or unquote(qs.get("src", [""])[0])
            dst = body_json.get("new_path") or body_json.get("dst") or unquote(qs.get("dst", [""])[0])
            if getattr(self.state, "require_json_move", False):
                if not raw_body or not body_json.get("old_path") or not body_json.get("new_path"):
                    return self._send_json(500, {"error": "ESP32 firmware requires JSON old_path/new_path"})
            if src not in self.state.sd_files:
                return self._send_json(404, {"ok": False, "error": "origem nao encontrada"})
            self.state.sd_files[dst] = self.state.sd_files.pop(src)
            self.state.played_flags[dst] = False
            return self._send_json(200, {"status": "ok", "ok": True})

        if parsed.path == "/api/delete":
            body_json = json.loads(raw_body.decode("utf-8") or "{}") if raw_body else {}
            target = body_json.get("path") or unquote(qs.get("path", [""])[0])
            if target in self.state.sd_files:
                del self.state.sd_files[target]
                self.state.played_flags.pop(target, None)
                return self._send_json(200, {"status": "ok", "ok": True})
            return self._send_json(404, {"ok": False})

        if parsed.path == "/api/sync/done":
            self.state.sync_done_summary = json.loads(raw_body.decode("utf-8") or "{}")
            return self._send_json(200, {"ok": True})

        if parsed.path == "/api/yt/innertube":
            if not self.state.innertube_enabled:
                return self._send_json(502, {"error": "relay_disabled_in_test"})
            raw_txt = raw_body.decode("utf-8", errors="ignore")
            meta_str, post_str = raw_txt.split("\n", 1) if "\n" in raw_txt else (raw_txt, "")
            meta = json.loads(meta_str or "{}")
            body_obj = json.loads(post_str or "{}") if post_str.strip() else {}
            self.state.innertube_calls.append({"meta": meta, "body": body_obj})
            target_url = meta.get("url", "")

            if "navigation/resolve_url" in target_url:
                return self._send_json(
                    200,
                    {"endpoint": {"browseEndpoint": {"browseId": "UC_DLSHOW_TEST_123456789"}}},
                )
            if "youtubei/v1/browse" in target_url:
                if body_obj.get("browseId") == "FEwhat_to_watch":
                    return self._send_json(200, {"responseContext": {"visitorData": "VISITOR_MOCK_123"}})
                return self._send_json(
                    200,
                    {
                        "responseContext": {"visitorData": "VISITOR_BROWSE_456"},
                        "contents": {
                            "twoColumnBrowseResultsRenderer": {
                                "tabs": [
                                    {
                                        "tabRenderer": {
                                            "selected": True,
                                            "content": {
                                                "richGridRenderer": {
                                                    "contents": [
                                                        {
                                                            "richItemRenderer": {
                                                                "content": {
                                                                    "videoRenderer": {
                                                                        "videoId": "dlshow_0669",
                                                                        "title": {"runs": [{"text": "DLSHOW #669 AO VIVO"}]},
                                                                        "publishedTimeText": {"simpleText": "Transmitido há 1 dia"},
                                                                        "lengthText": {"simpleText": "2:15:00"},
                                                                    }
                                                                }
                                                            }
                                                        },
                                                        {
                                                            "richItemRenderer": {
                                                                "content": {
                                                                    "videoRenderer": {
                                                                        "videoId": "dlshow_memb",
                                                                        "title": {"runs": [{"text": "EXCLUSIVO MEMBROS"}]},
                                                                        "badges": [{"metadataBadgeRenderer": {"style": "BADGE_STYLE_TYPE_MEMBERS_ONLY"}}],
                                                                    }
                                                                }
                                                            }
                                                        },
                                                    ]
                                                }
                                            },
                                        }
                                    }
                                ]
                            }
                        },
                    },
                )
            if "youtubei/v1/player" in target_url:
                vid = body_obj.get("videoId", "dlshow_0669")
                return self._send_json(
                    200,
                    {
                        "playabilityStatus": {"status": "OK"},
                        "streamingData": {
                            "adaptiveFormats": [
                                {
                                    "itag": 140,
                                    "mimeType": 'audio/mp4; codecs="mp4a.40.2"',
                                    "bitrate": 128000,
                                    "contentLength": "16384",
                                    "url": f"https://rr1---sn-mock.googlevideo.com/videoplayback?id={vid}&itag=140",
                                }
                            ]
                        },
                    },
                )
            return self._send_json(502, {"error": "unknown_innertube_url"})

        if parsed.path == "/api/sync/pull_direct":
            req = json.loads(raw_body.decode("utf-8") or "{}")
            self.state.pull_direct_calls.append(req)
            staging_path = req.get("staging_path", "")
            append = bool(req.get("append"))
            offset_bytes = int(req.get("offset_bytes") or 0)
            url = req.get("url", "")
            # Simula geração de bytes por fragmento
            if "range=0-" in url:
                chunk_data = b"GV_FRAG_1_" * 512
            elif "range=" in url:
                chunk_data = b"GV_FRAG_2_" * 512
            else:
                chunk_data = b"CLOUDFRONT_MP3_DIRECT_" * 512

            if append and staging_path in self.state.sd_files:
                self.state.sd_files[staging_path] += chunk_data
            else:
                self.state.sd_files[staging_path] = chunk_data

            ndjson = (
                json.dumps(
                    {
                        "rx": offset_bytes + len(chunk_data),
                        "total": req.get("total_bytes") or (offset_bytes + len(chunk_data)),
                        "speed_kbs": 215.4,
                    }
                )
                + "\n"
                + json.dumps(
                    {
                        "done": True,
                        "status": "ok",
                        "bytes": len(chunk_data),
                        "total": offset_bytes + len(chunk_data),
                    }
                )
                + "\n"
            ).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/x-ndjson; charset=utf-8")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Content-Length", str(len(ndjson)))
            self.end_headers()
            self.wfile.write(ndjson)
            return

        return self._send_json(404, {"error": "unknown POST"})


class TestBrowserPodcastE2E(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.core_js = extract_pure_logic_js()
        cls.state = MockMps3AndYoutubeState()
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), MockHandler)
        cls.port = cls.server.server_port
        cls.base_url = f"http://127.0.0.1:{cls.port}"
        MockHandler.state = cls.state
        MockHandler.base_url = cls.base_url
        cls.server_thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.server_thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()

    def test_01_full_browser_scrape_plan_and_transactional_sync(self):
        js_driver = f"""
        const BASE = {json.dumps(self.base_url)};
        globalThis.window = {{}};
        globalThis.document = {{
            getElementById: () => ({{ value: BASE }})
        }};
        globalThis.localStorage = {{
            getItem: () => BASE,
            setItem: () => {{}}
        }};
        {self.core_js}

        (async () => {{
            // 1. Persiste assinaturas em /Podcasts/.subscriptions.json no SD
            const subsPayload = {{
                version: 3,
                subscriptions: [
                    {{ id: 'sub1', name: 'Xadrez Verbal', type: 'youtube', channel_id: 'UC_XADREZ', url: 'https://youtube.com/@xadrezverbal', enabled: true }}
                ]
            }};
            await fetch(BASE + '/api/upload?path=' + encodeURIComponent('/Podcasts/.subscriptions.json'), {{
                method: 'PUT',
                body: JSON.stringify(subsPayload)
            }});

            // 2. Busca manifesto do MPS3 e raspa o canal YouTube via Piped mock
            const manifest = await (await fetch(BASE + '/api/sync/manifest')).json();
            const scraped = await scrapeYoutubeSubscription(subsPayload.subscriptions[0], 6);
            const plan = BrowserSyncPlanner.computeSyncPlan(scraped, manifest, {{ keepRecent: 3, autoCleanPlayed: true }});

            // 3. Executa o protocolo transacional (/api/sync/begin -> delete -> staging -> verify -> move -> done)
            await fetch(BASE + '/api/sync/begin', {{ method: 'POST' }});

            let deletedCount = 0;
            for (const delItem of plan.deletes) {{
                const rDel = await fetch(BASE + '/api/delete?path=' + encodeURIComponent(delItem.path), {{ method: 'POST' }});
                if (rDel.ok) deletedCount++;
            }}

            let downloadedCount = 0;
            for (const ep of plan.downloads.filter(d => d.selected)) {{
                const resolved = await resolveYoutubeAudioStream(ep);
                const mediaRes = await fetch(resolved.url);
                const audioBytes = new Uint8Array(await mediaRes.arrayBuffer());
                const sha = sha256Bytes(audioBytes);
                const fname = buildPodEpisodeFilename(ep, resolved.ext);
                const stagingPath = '/Podcasts/.staging/' + fname;
                const finalPath = '/Podcasts/' + ep.program + '/' + fname;

                const upRes = await fetch(BASE + '/api/upload?path=' + encodeURIComponent(stagingPath) + '&sha256=' + sha, {{
                    method: 'PUT',
                    body: audioBytes
                }});
                if (!upRes.ok) throw new Error('Upload failed');

                const verRes = await fetch(BASE + '/api/sync/verify', {{
                    method: 'POST',
                    headers: {{ 'Content-Type': 'application/json' }},
                    body: JSON.stringify({{ path: stagingPath, expected_size: audioBytes.byteLength, expected_sha256: sha }})
                }});
                const verJson = await verRes.json();
                if (!verJson.verified) throw new Error('Verify failed');

                const mvRes = await fetch(BASE + '/api/move?src=' + encodeURIComponent(stagingPath) + '&dst=' + encodeURIComponent(finalPath), {{
                    method: 'POST'
                }});
                if (!mvRes.ok) throw new Error('Move failed');
                downloadedCount++;
            }}

            await fetch(BASE + '/api/sync/done', {{
                method: 'POST',
                headers: {{ 'Content-Type': 'application/json' }},
                body: JSON.stringify({{ downloaded: downloadedCount, deleted: deletedCount }})
            }});

            console.log(JSON.stringify({{
                scrapedCount: scraped.length,
                plannedDownloads: plan.downloads.map(d => d.id),
                plannedDeletes: plan.deletes.map(d => d.filename),
                skippedPlayed: plan.skippedPlayed.map(d => d.id),
                downloadedCount,
                deletedCount
            }}));
        }})().catch(e => {{ console.error(e); process.exit(1); }});
        """

        res = subprocess.run(
            ["node", "-"],
            input=js_driver,
            capture_output=True,
            text=True,
            encoding="utf-8",
            timeout=15,
        )
        if res.returncode != 0:
            self.fail(f"Erro no E2E JS:\n{res.stderr}\n{res.stdout}")

        summary = json.loads(res.stdout.strip())
        self.assertEqual(summary["scrapedCount"], 5)
        # Top 3 do canal são yt_ep5, yt_ep4, yt_ep3. Como yt_ep4 está no played_history, baixa yt_ep5 e yt_ep3!
        self.assertEqual(summary["plannedDownloads"], ["yt_ep5", "yt_ep3"])
        self.assertEqual(summary["skippedPlayed"], ["yt_ep4"])
        self.assertEqual(summary["downloadedCount"], 2)
        self.assertEqual(summary["deletedCount"], 1)

        # Verifica estado final do cartão SD simulado no servidor
        self.assertTrue(self.state.sync_begun)
        self.assertEqual(self.state.sync_done_summary, {"downloaded": 2, "deleted": 1})
        self.assertIn("/Podcasts/.subscriptions.json", self.state.sd_files)

        sd_keys = list(self.state.sd_files.keys())
        # Episódio 2 (antigo e já ouvido) foi removido
        self.assertFalse(any("yt_ep2" in k for k in sd_keys))
        # Episódio 1 (antigo mas NÃO ouvido) foi preservado intacto!
        self.assertTrue(any("yt_ep1" in k for k in sd_keys))
        # Episódios 5 e 3 foram verificados com SHA-256 e movidos para a pasta final
        self.assertTrue(any("yt_ep5" in k and ".staging" not in k for k in sd_keys))
        self.assertTrue(any("yt_ep3" in k and ".staging" not in k for k in sd_keys))

    def test_02_sync_verify_rejects_corrupted_sha256(self):
        js_driver = f"""
        const BASE = {json.dumps(self.base_url)};
        {self.core_js}
        (async () => {{
            const goodBytes = new TextEncoder().encode('CONTEUDO_INTEGRO_PODCAST');
            const goodSha = sha256Bytes(goodBytes);
            const badSha = '0'.repeat(64);
            const stagingPath = '/Podcasts/.staging/corrupt_test.m4a';

            await fetch(BASE + '/api/upload?path=' + encodeURIComponent(stagingPath), {{
                method: 'PUT',
                body: goodBytes
            }});

            const verBad = await fetch(BASE + '/api/sync/verify', {{
                method: 'POST',
                headers: {{ 'Content-Type': 'application/json' }},
                body: JSON.stringify({{ path: stagingPath, expected_size: goodBytes.byteLength, expected_sha256: badSha }})
            }});
            const badJson = await verBad.json();

            const verGood = await fetch(BASE + '/api/sync/verify', {{
                method: 'POST',
                headers: {{ 'Content-Type': 'application/json' }},
                body: JSON.stringify({{ path: stagingPath, expected_size: goodBytes.byteLength, expected_sha256: goodSha }})
            }});
            const goodJson = await verGood.json();

            console.log(JSON.stringify({{
                badStatus: verBad.status,
                badVerified: badJson.verified,
                goodStatus: verGood.status,
                goodVerified: goodJson.verified
            }}));
        }})().catch(e => {{ console.error(e); process.exit(1); }});
        """
        res = subprocess.run(
            ["node", "-"],
            input=js_driver,
            capture_output=True,
            text=True,
            encoding="utf-8",
            timeout=10,
        )
        self.assertEqual(res.returncode, 0, res.stderr)
        out = json.loads(res.stdout.strip())
        self.assertEqual(out["badStatus"], 422)
        self.assertFalse(out["badVerified"])
        self.assertEqual(out["goodStatus"], 200)
        self.assertTrue(out["goodVerified"])

    def test_03_dom_sync_loop_strict_firmware_contract(self):
        """
        Executes the real btnStartPodSync.onclick handler from 09_podcast_sync.js
        with strict ESP32 firmware contract enforcement (JSON old_path/new_path on
        POST /api/move and X-Expected-SHA256 header on PUT /api/upload).
        """
        self.state.require_json_move = True
        full_js = (
            pathlib.Path(__file__).resolve().parents[1]
            / "components"
            / "wifi_transfer"
            / "web"
            / "src"
            / "js"
            / "09_podcast_sync.js"
        ).read_text(encoding="utf-8")

        js_driver = f"""
        const BASE = {json.dumps(self.base_url)};
        const origFetch = globalThis.fetch;
        globalThis.fetch = (url, opts) => {{
            const target = String(url).startsWith('http') ? url : (BASE + url);
            return origFetch(target, opts);
        }};
        globalThis.setInterval = () => 1;
        globalThis.fetchStatus = () => {{}};
        globalThis.load = () => {{}};
        globalThis.confirm = () => true;
        globalThis.alert = () => {{}};

        const elements = new Map();
        function makeEl(id) {{
            if (!elements.has(id)) {{
                elements.set(id, {{
                    id,
                    value: '',
                    checked: true,
                    disabled: false,
                    textContent: '',
                    innerHTML: '',
                    style: {{}},
                    children: [],
                    onclick: null,
                    appendChild(ch) {{ this.children.push(ch); }},
                    querySelector() {{ return {{ checked: true }}; }},
                    querySelectorAll() {{ return []; }}
                }});
            }}
            return elements.get(id);
        }}
        globalThis.document = {{
            getElementById: (id) => makeEl(id),
            createElement: (tag) => ({{ tag, style: {{}}, textContent: '' }})
        }};
        globalThis.window = {{ location: {{ hostname: 'mps3.local', origin: BASE }} }};
        globalThis.localStorage = {{ getItem: () => null, setItem: () => {{}} }};

        let capturedXhrHeaders = {{}};
        globalThis.XMLHttpRequest = class {{
            constructor() {{
                this.upload = {{}};
                this.headers = {{}};
                this.status = 0;
                this.responseText = '';
            }}
            open(method, url) {{
                this.method = method;
                this.url = String(url).startsWith('http') ? url : (BASE + url);
            }}
            setRequestHeader(k, v) {{
                this.headers[k] = v;
                capturedXhrHeaders[k] = v;
            }}
            async send(blob) {{
                try {{
                    const buf = Buffer.from(await blob.arrayBuffer());
                    const res = await origFetch(this.url, {{
                        method: this.method,
                        headers: this.headers,
                        body: buf
                    }});
                    this.status = res.status;
                    this.responseText = await res.text();
                    if (this.onload) this.onload();
                }} catch (e) {{
                    if (this.onerror) this.onerror(e);
                }}
            }}
        }};

        {full_js}

        (async () => {{
            podPendingList = [{{
                id: 'strict_ep1',
                program: 'NerdCast',
                title: 'Episodio Teste Final Transferencia',
                mtime: 1761500000,
                source_type: 'rss',
                direct_url: BASE + '/media/yt_ep5.m4a',
                ext: 'm4a',
                size_bytes: 4096,
                selected: true
            }}];
            podPlannedDeletes = [];
            await document.getElementById('btnStartPodSync').onclick();
            console.log(JSON.stringify({{
                statusMsg: document.getElementById('podStatusMsg').textContent,
                xhrHeaders: capturedXhrHeaders,
                remainingSelected: podPendingList.filter(e => e.selected).length
            }}));
        }})().catch(e => {{ console.error(e); process.exit(1); }});
        """
        try:
            res = subprocess.run(
                ["node", "-"],
                input=js_driver,
                capture_output=True,
                text=True,
                encoding="utf-8",
                timeout=15,
            )
            self.assertEqual(res.returncode, 0, f"{res.stderr}\n{res.stdout}")
            out = json.loads(res.stdout.strip())
            self.assertIn("1/1 baixados", out["statusMsg"])
            self.assertEqual(out["remainingSelected"], 0)
            self.assertEqual(len(out["xhrHeaders"].get("X-Expected-SHA256", "")), 64)
            self.assertTrue(
                any("/Podcasts/NerdCast/" in k and "strict_ep1" in k for k in self.state.sd_files.keys())
            )
        finally:
            self.state.require_json_move = False

    def test_04_zero_cors_innertube_relay_scrape_and_visionos_resolve(self):
        """
        Valida a raspagem YouTube e resolução de stream M4A (VISIONOS 1.02) via
        relay nativo do ESP32-S3 (POST /api/yt/innertube) sem Tampermonkey e sem Piped/Invidious,
        incluindo Proto Field Mask (&fields=...) e filtragem de vídeos exclusivos para membros.
        """
        self.state.innertube_enabled = True
        self.state.innertube_calls.clear()
        try:
            js_driver = f"""
            const BASE = {json.dumps(self.base_url)};
            const origFetch = globalThis.fetch;
            globalThis.fetch = (url, opts) => {{
                const target = String(url).startsWith('http') ? url : (BASE + url);
                return origFetch(target, opts);
            }};
            globalThis.window = {{}};
            globalThis.document = {{ getElementById: () => ({{ value: '' }}) }};
            globalThis.localStorage = {{ getItem: () => null, setItem: () => {{}} }};
            {self.core_js}

            (async () => {{
                const sub = {{
                    name: 'Desce a Letra Show',
                    type: 'youtube',
                    url: 'https://www.youtube.com/@DESCEALETRASHOW/streams',
                    enabled: true
                }};
                const items = await scrapeYoutubeSubscription(sub, 5);
                const stream = await resolveYoutubeAudioStream(items[0]);
                console.log(JSON.stringify({{ items, stream }}));
            }})().catch(e => {{ console.error(e); process.exit(1); }});
            """
            res = subprocess.run(
                ["node", "-"],
                input=js_driver,
                capture_output=True,
                text=True,
                encoding="utf-8",
                timeout=15,
            )
            self.assertEqual(res.returncode, 0, f"{res.stderr}\n{res.stdout}")
            out = json.loads(res.stdout.strip())
            # Apenas dlshow_0669 deve retornar (dlshow_memb filtrado como exclusivo de membros)
            self.assertEqual(len(out["items"]), 1)
            self.assertEqual(out["items"][0]["video_id"], "dlshow_0669")
            self.assertEqual(out["items"][0]["title"], "DLSHOW #669 AO VIVO")
            self.assertEqual(out["stream"]["ext"], "m4a")
            self.assertEqual(out["stream"]["contentLength"], 16384)
            self.assertIn("googlevideo.com", out["stream"]["url"])
            # Verifica se o relay /api/yt/innertube foi acionado com Proto Field Mask (&fields=...)
            relay_urls = [c["meta"]["url"] for c in self.state.innertube_calls]
            self.assertTrue(any("navigation/resolve_url" in u and "fields=" in u for u in relay_urls))
            self.assertTrue(any("youtubei/v1/browse" in u and "fields=" in u for u in relay_urls))
            self.assertTrue(any("youtubei/v1/player" in u and "fields=" in u for u in relay_urls))
        finally:
            self.state.innertube_enabled = False

    def test_05_zero_cors_pull_direct_ndjson_anchor_and_googlevideo_chunks(self):
        """
        Valida o motor pullEpisodeDirectOnMps3 (POST /api/sync/pull_direct com stream NDJSON):
        1. Desencapsulamento automático de URLs Anchor/Spotify (/podcast/play/.../https%3A%2F%2F...cloudfront.net...).
        2. Download fragmentado (&range=...) com append=0 e append=1 para streams googlevideo.com > 8 MB.
        """
        self.state.pull_direct_calls.clear()
        full_js = (
            pathlib.Path(__file__).resolve().parents[1]
            / "components"
            / "wifi_transfer"
            / "web"
            / "src"
            / "js"
            / "09_podcast_sync.js"
        ).read_text(encoding="utf-8")
        ext_js = (
            pathlib.Path(__file__).resolve().parents[1]
            / "components"
            / "wifi_transfer"
            / "web"
            / "src"
            / "js"
            / "09a_yt_extractor.js"
        ).read_text(encoding="utf-8")

        js_driver = f"""
        const BASE = {json.dumps(self.base_url)};
        const origFetch = globalThis.fetch;
        globalThis.fetch = (url, opts) => {{
            const target = String(url).startsWith('http') ? url : (BASE + url);
            return origFetch(target, opts);
        }};
        globalThis.setInterval = () => 1;
        globalThis.fetchStatus = () => {{}};
        globalThis.load = () => {{}};
        const dummyEl = () => ({{ style: {{}}, textContent: '', value: '', checked: true, disabled: false }});
        globalThis.document = {{ getElementById: () => dummyEl(), querySelectorAll: () => [] }};
        globalThis.window = globalThis;
        globalThis.localStorage = {{ getItem: () => null, setItem: () => {{}} }};

        {ext_js}
        {full_js}

        (async () => {{
            isPodSyncing = true;
            const anchorUrl = 'https://anchor.fm/s/1068cff58/podcast/play/128659259/https%3A%2F%2Fd3ctxlq1ktw2nl.cloudfront.net%2Fstaging%2F2026-10-3%2Fep_horaboa.mp3';
            const b1 = await pullEpisodeDirectOnMps3(anchorUrl, '/Podcasts/.staging/horaboa.mp3', 0, '', 1, 2, 20000000, 'Hora Boa #1');

            const gvUrl = 'https://rr1---sn-mock.googlevideo.com/videoplayback?id=dlshow_669&itag=140';
            const tenMb = 10 * 1024 * 1024;
            const b2 = await pullEpisodeDirectOnMps3(gvUrl, '/Podcasts/.staging/dlshow.m4a', tenMb, 'Safari/26.0', 2, 2, 20000000, 'DLSHOW 669');
            console.log(JSON.stringify({{ b1, b2 }}));
        }})().catch(e => {{ console.error(e); process.exit(1); }});
        """
        res = subprocess.run(
            ["node", "-"],
            input=js_driver,
            capture_output=True,
            text=True,
            encoding="utf-8",
            timeout=15,
        )
        self.assertEqual(res.returncode, 0, f"{res.stderr}\n{res.stdout}")
        # 1 chamada para CloudFront (desencapsulado do Anchor) + 2 chamadas para googlevideo (10 MB = 8 MB + 2 MB)
        self.assertEqual(len(self.state.pull_direct_calls), 3)
        self.assertEqual(
            self.state.pull_direct_calls[0]["url"],
            "https://d3ctxlq1ktw2nl.cloudfront.net/staging/2026-10-3/ep_horaboa.mp3",
        )
        self.assertEqual(self.state.pull_direct_calls[0]["append"], 0)
        self.assertEqual(self.state.pull_direct_calls[0]["batch_total"], 20000000)
        self.assertEqual(self.state.pull_direct_calls[0]["title"], "Hora Boa #1")
        self.assertIn("range=0-8388607", self.state.pull_direct_calls[1]["url"])
        self.assertEqual(self.state.pull_direct_calls[1]["append"], 0)
        self.assertEqual(self.state.pull_direct_calls[1]["batch_total"], 20000000)
        self.assertEqual(self.state.pull_direct_calls[1]["title"], "DLSHOW 669")
        self.assertIn("range=8388608-10485759", self.state.pull_direct_calls[2]["url"])
        self.assertEqual(self.state.pull_direct_calls[2]["append"], 1)
        self.assertEqual(
            self.state.sd_files["/Podcasts/.staging/dlshow.m4a"],
            (b"GV_FRAG_1_" * 512) + (b"GV_FRAG_2_" * 512),
        )


if __name__ == "__main__":
    unittest.main()
