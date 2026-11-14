#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Suíte de Testes Automatizados — MPS3 Wrapped, QR ISO-18004 & Ponte PC -> Smartphone
-----------------------------------------------------------------------------------
Valida:
1. Motor analítico JS (`10_wrapped_stats.js` no V8/Node): JSONL parser, amostras PCM,
   porcentagem Hi-Res 24-bit, Streak diário, Personas e filtros Música vs Podcast.
2. Codificador QR Code JS (`encodeQrMatrix` / `generateQrSvg`): decodificação reversa
   completa (Format Info BCH, Unmasking, Zigzag Bitstream, Síndromes Reed-Solomon == 0
   e extração exata da URL da ponte mobile em V1..V7).
3. Fluxo E2E da Ponte PC -> Smartphone via `web_dev_simulator.py`:
   `/api/analytics/manifest`, upload de PNG para `/.mps3_analytics/share/mps3_wrapped.png`,
   descoberta de IP LAN em `/api/status` e download mobile com `Content-Type: image/png`
   e `Content-Disposition: attachment` (`&dl=1`).
"""

from http.server import ThreadingHTTPServer
import json
import os
import subprocess
import sys
import tempfile
import threading
import unittest
from urllib.parse import quote
import urllib.request

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(PROJECT_ROOT, "tools"))

import pack_web  # noqa: E402
import web_dev_simulator  # noqa: E402


def gf_mul(a, b):
    z = 0
    for i in range(7, -1, -1):
        z = (z << 1) ^ ((z >> 7) * 0x11D)
        z ^= ((b >> i) & 1) * a
    return z & 0xFF


def gf_pow(x, power):
    res = 1
    for _ in range(power):
        res = gf_mul(res, x)
    return res


def decode_qr_matrix_iso18004(mod):
    """Decodificador ISO/IEC 18004 independente para validar a matriz gerada pelo JS."""
    size = len(mod)
    ver = (size - 17) // 4
    assert 1 <= ver <= 9 and ver * 4 + 17 == size

    # 1. Lê primeira cópia dos 15 Format Bits e desfaz XOR 0x5412
    fmt_bits = 0
    for i in range(6):
        fmt_bits |= (mod[i][8] & 1) << i
    fmt_bits |= (mod[7][8] & 1) << 6
    fmt_bits |= (mod[8][8] & 1) << 7
    fmt_bits |= (mod[8][7] & 1) << 8
    for i in range(9, 15):
        fmt_bits |= (mod[8][14 - i] & 1) << i

    # Verifica igualdade com a segunda cópia
    fmt_bits_copy2 = 0
    for i in range(8):
        fmt_bits_copy2 |= (mod[8][size - 1 - i] & 1) << i
    for i in range(8, 15):
        fmt_bits_copy2 |= (mod[size - 15 + i][8] & 1) << i
    assert fmt_bits == fmt_bits_copy2, "Cópias 1 e 2 dos Format Bits divergem!"

    raw_fmt = fmt_bits ^ 0x5412
    # Verifica resto BCH (polinômio 0x537) == 0
    rem = raw_fmt
    for b in range(14, 9, -1):
        if rem & (1 << b):
            rem ^= 0x537 << (b - 10)
    assert rem == 0, f"BCH inválido nos Format Bits: resto={rem}"

    ecc_fmt = (raw_fmt >> 13) & 0x03
    mask = (raw_fmt >> 10) & 0x07
    assert ecc_fmt == 0x01 and mask == 0

    # 2. Marca módulos funcionais
    is_fn = [[False] * size for _ in range(size)]
    for ox, oy in [(0, 0), (size - 7, 0), (0, size - 7)]:
        for dy in range(-1, 8):
            for dx in range(-1, 8):
                cx, cy = ox + dx, oy + dy
                if 0 <= cx < size and 0 <= cy < size:
                    is_fn[cy][cx] = True
    for i in range(8, size - 8):
        is_fn[6][i] = True
        is_fn[i][6] = True
    if ver >= 2:
        align_centers = {2: 18, 3: 22, 4: 26, 5: 30, 6: 34, 7: 22, 8: 24, 9: 26}
        coords = [6, size - 7] if ver <= 6 else [6, align_centers[ver], size - 7]
        for ay in coords:
            for ax in coords:
                if (ax, ay) in ((6, 6), (size - 7, 6), (6, size - 7)):
                    continue
                for dy in range(-2, 3):
                    for dx in range(-2, 3):
                        is_fn[ay + dy][ax + dx] = True
    for i in range(9):
        if i != 6:
            is_fn[8][i] = True
            is_fn[i][8] = True
    for i in range(8):
        is_fn[8][size - 1 - i] = True
        is_fn[size - 1 - i][8] = True
    is_fn[size - 8][8] = True
    if ver >= 7:
        for i in range(18):
            a = size - 11 + (i % 3)
            b = i // 3
            is_fn[b][a] = True
            is_fn[a][b] = True

    # 3. Extrai bits em ziguezague desfazendo máscara 0
    bits = []
    right = size - 1
    while right >= 1:
        if right == 6:
            right = 5
        for vert in range(size):
            for j in range(2):
                cx = right - j
                up = (((right & 2) == 0) ^ (cx < 6))
                cy = (size - 1 - vert) if up else vert
                if not is_fn[cy][cx]:
                    val = mod[cy][cx] ^ (1 if (cx + cy) % 2 == 0 else 0)
                    bits.append(val)
        right -= 2

    vtab = {
        1: (26, 7, 1),
        2: (44, 10, 1),
        3: (70, 15, 1),
        4: (100, 20, 1),
        5: (134, 26, 1),
        6: (172, 18, 2),
        7: (196, 20, 2),
        8: (242, 24, 2),
        9: (292, 30, 2),
    }
    total_cw, ec_len, num_blocks = vtab[ver]
    cws = []
    for i in range(0, total_cw * 8, 8):
        b = 0
        for k in range(8):
            b = (b << 1) | bits[i + k]
        cws.append(b)

    # Desentrelaça blocos e valida todas as síndromes Reed-Solomon == 0!
    data_cw_len = total_cw - ec_len * num_blocks
    blk_len = data_cw_len // num_blocks
    data_blocks = [[] for _ in range(num_blocks)]
    ec_blocks = [[] for _ in range(num_blocks)]

    idx = 0
    max_blk = data_cw_len - blk_len * (num_blocks - 1)
    for i in range(max_blk):
        for b in range(num_blocks):
            this_len = (data_cw_len - blk_len * (num_blocks - 1)) if b == num_blocks - 1 else blk_len
            if i < this_len:
                data_blocks[b].append(cws[idx])
                idx += 1
    for i in range(ec_len):
        for b in range(num_blocks):
            ec_blocks[b].append(cws[idx])
            idx += 1

    all_data = []
    for b in range(num_blocks):
        full_poly = data_blocks[b] + ec_blocks[b]
        for s_idx in range(ec_len):
            root = gf_pow(2, s_idx)
            syn = 0
            for coef in full_poly:
                syn = gf_mul(syn, root) ^ coef
            assert syn == 0, f"Síndrome RS[{s_idx}] != 0 no bloco {b}: {syn}"
        all_data.extend(data_blocks[b])

    # Decodifica bitstream Modo Byte (0100)
    dbits = []
    for byte in all_data:
        for k in range(7, -1, -1):
            dbits.append((byte >> k) & 1)
    mode = (dbits[0] << 3) | (dbits[1] << 2) | (dbits[2] << 1) | dbits[3]
    assert mode == 0b0100, f"Modo inesperado: {mode}"
    length = 0
    for k in range(4, 12):
        length = (length << 1) | dbits[k]
    out_bytes = bytearray()
    for i in range(length):
        b = 0
        for k in range(8):
            b = (b << 1) | dbits[12 + i * 8 + k]
        out_bytes.append(b)
    return out_bytes.decode("utf-8")


class TestWrappedAnalyticsAndMobileBridge(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.js_path = os.path.join(
            PROJECT_ROOT, "components", "wifi_transfer", "web", "src", "js", "10_wrapped_stats.js"
        )

    def _run_node(self, eval_tail):
        harness = f"""
        const fs = require('fs');
        const vm = require('vm');
        global.document = {{
            getElementById: () => null,
            querySelectorAll: () => []
        }};
        global.window = {{
            location: {{ search: '', origin: 'http://mps3.local', pathname: '/' }},
            innerWidth: 1440
        }};
        global.navigator = {{ userAgent: 'Mozilla/5.0 Windows NT 10.0', maxTouchPoints: 0 }};
        const code = fs.readFileSync({json.dumps(self.js_path)}, 'utf8');
        vm.runInThisContext(code);
        {eval_tail}
        """
        proc = subprocess.run(
            ["node", "-e", harness],
            capture_output=True,
            text=True,
            encoding="utf-8",
            timeout=10,
        )
        if proc.returncode != 0:
            self.fail(f"Node execution failed:\n{proc.stderr}")
        return json.loads(proc.stdout.strip())

    def test_01_qr_iso18004_full_reed_solomon_decode(self):
        test_urls = [
            "http://192.168.4.1/?share=wrapped",
            "http://192.168.1.100/?share=wrapped&v=m2rk9a",
            "http://192.168.1.108:8090/?share=wrapped&v=1730079999",
            "http://100.64.0.1:8090/?share=wrapped&v=long_tailscale_bridge_token_2026_test_v7_matrix",
        ]
        matrices = self._run_node(
            f"""
            const urls = {json.dumps(test_urls)};
            const res = urls.map(u => ({{ url: u, qr: encodeQrMatrix(u), svgLen: generateQrSvg(u).length }}));
            console.log(JSON.stringify(res));
            """
        )
        for item in matrices:
            self.assertIsNotNone(item["qr"])
            self.assertGreater(item["svgLen"], 400)
            mod = [[int(x) for x in list(row.values())] if isinstance(row, dict) else row for row in item["qr"]["mod"]]
            decoded = decode_qr_matrix_iso18004(mod)
            self.assertEqual(decoded, item["url"])

    def test_02_analytics_engine_math_and_personas(self):
        res = self._run_node(
            """
            const demo = generateDemoEvents();
            const allSt = computeWrappedAnalytics(demo, 'all', 'all');
            const musSt = computeWrappedAnalytics(demo, 'all', 'music');
            const podSt = computeWrappedAnalytics(demo, 'all', 'podcast');
            console.log(JSON.stringify({ allSt, musSt, podSt }));
            """
        )
        all_st = res["allSt"]
        mus_st = res["musSt"]
        pod_st = res["podSt"]
        self.assertGreater(all_st["totalSec"], 10000)
        self.assertGreaterEqual(all_st["longestStreak"], 20)
        self.assertGreaterEqual(all_st["zeroSkipPct"], 90)
        # Valida novas métricas de concorrentes (Spotify / Apple / TIDAL / Amazon / Last.fm / Roon)
        self.assertGreater(len(mus_st["topAlbums"]), 4)
        self.assertEqual(mus_st["topAlbums"][0]["name"], "The Dark Side of the Moon")
        self.assertGreaterEqual(mus_st["fullAlbumSessions"], 5)
        self.assertTrue(1970 <= mus_st["musicalAge"] <= 1995)
        self.assertGreater(len(mus_st["decades"]), 2)
        self.assertGreater(len(mus_st["topGenres"]), 3)
        self.assertGreater(all_st["peakCalendarDay"]["sec"], 3000)
        self.assertIsNotNone(all_st["loopObsession"])
        self.assertGreaterEqual(all_st["loopObsession"]["maxPlaysOneDay"], 6)
        self.assertGreater(mus_st["topArtists"][0]["sharePct"], 10)
        self.assertIsNotNone(mus_st["crownRivalry"])
        self.assertGreater(len(all_st["phases"]), 1)
        self.assertTrue(bool(all_st["dayVsNight"]["dayLeader"]))
        self.assertTrue(bool(all_st["dayVsNight"]["nightLeader"]))
        # Personas comportamentais humanas (zero clichê de hardware)
        self.assertEqual(all_st["persona"]["title"], "Guardião do Álbum Completo")
        self.assertEqual(mus_st["persona"]["title"], "Guardião do Álbum Completo")
        self.assertEqual(pod_st["persona"]["title"], "Devorador de Podcasts")

    def test_04_oled_128x64_four_colors_and_1bit_matrix_engine(self):
        res = self._run_node(
            """
            const demo = generateDemoEvents();
            const st = computeWrappedAnalytics(demo, 'all', 'all');
            const oled1 = buildNowPlayingOled128x64(st);
            const oled2 = buildRtaAndTopTracksOled128x64(st);
            const oledFest = buildFestivalLineupOled(st, 218);
            let lit1 = 0, lit2 = 0, litFest = 0;
            for (let y = 0; y < 64; y++) {
                for (let x = 0; x < 128; x++) {
                    if (oled1.buf[y][x]) lit1++;
                    if (oled2.buf[y][x]) lit2++;
                }
            }
            for (let y = 0; y < 218; y++) {
                for (let x = 0; x < 128; x++) {
                    if (oledFest.buf[y][x]) litFest++;
                }
            }
            const colors = ['oled_white', 'oled_blue', 'oled_yellow', 'oled_green', 'oled_dual'];
            const palCheck = colors.map(k => ({
                key: k,
                on: WRP_PALETTES[k]?.oledOn,
                top: WRP_PALETTES[k]?.oledTop,
                dual: WRP_PALETTES[k]?.dualZone
            }));
            console.log(JSON.stringify({ w1: oled1.w, h1: oled1.h, wf: oledFest.w, hf: oledFest.h, lit1, lit2, litFest, palCheck }));
            """
        )
        self.assertEqual(res["w1"], 128)
        self.assertEqual(res["h1"], 64)
        self.assertEqual(res["wf"], 128)
        self.assertEqual(res["hf"], 218)
        self.assertGreater(res["lit1"], 400)
        self.assertGreater(res["lit2"], 400)
        self.assertGreater(res["litFest"], 1500)
        expected_colors = {
            "oled_white": "#f4f9ff",
            "oled_blue": "#00e5ff",
            "oled_yellow": "#ffd600",
            "oled_green": "#00ff66",
            "oled_dual": "#00e5ff",
        }
        for item in res["palCheck"]:
            self.assertEqual(item["on"], expected_colors[item["key"]])
        dual_entry = [x for x in res["palCheck"] if x["key"] == "oled_dual"][0]
        self.assertTrue(dual_entry["dual"])
        self.assertEqual(dual_entry["top"], "#ffd600")

    def test_03_e2e_pc_to_smartphone_bridge_simulator(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            ana_dir = os.path.join(tmpdir, ".mps3_analytics")
            os.makedirs(ana_dir, exist_ok=True)
            sample_jsonl = (
                '{"ts":"2026-10-27T22:15:00","epoch":1793148900,"wday":2,"hour":22,'
                '"id":"abc123","path":"Musicas/Time.flac","cat":"music","title":"Time",'
                '"artist":"Pink Floyd","album":"DSOTM","codec":"FLAC","sr":192000,"bits":24,'
                '"dur_s":413,"listen_s":413,"pct":100,"scrobble":true,"completed":true,'
                '"end":"eof","seeks":0,"vol":78,"eq_on":true,"eq_preset":"Studio Hi-Res"}\n'
            )
            with open(os.path.join(ana_dir, "events_2026.jsonl"), "w", encoding="utf-8") as f:
                f.write(sample_jsonl)

            paths = pack_web.get_paths(PROJECT_ROOT)
            server = ThreadingHTTPServer(("127.0.0.1", 0), web_dev_simulator.SimulatorRequestHandler)
            port = server.server_address[1]
            sd_state = web_dev_simulator.VirtualSdState(tmpdir, port=port)
            web_dev_simulator.SimulatorRequestHandler.sd = sd_state
            web_dev_simulator.SimulatorRequestHandler.paths = paths

            t = threading.Thread(target=server.serve_forever, daemon=True)
            t.start()
            try:
                base = f"http://127.0.0.1:{port}"
                # 1. Verifica /api/status retornando IP LAN para o QR Code
                st = json.loads(urllib.request.urlopen(f"{base}/api/status", timeout=3).read().decode("utf-8"))
                self.assertIn("ip", st)
                self.assertTrue(st["ip"].endswith(f":{port}"))

                # 2. Verifica /api/analytics/manifest e leitura do JSONL
                man = json.loads(urllib.request.urlopen(f"{base}/api/analytics/manifest", timeout=3).read().decode("utf-8"))
                self.assertEqual(man["status"], "ok")
                self.assertEqual(len(man["files"]), 1)
                self.assertEqual(man["files"][0]["name"], "events_2026.jsonl")

                dl_path = quote(man["files"][0]["download_path"])
                raw_jsonl = urllib.request.urlopen(f"{base}/api/download?path={dl_path}", timeout=3).read().decode("utf-8")
                self.assertIn("Pink Floyd", raw_jsonl)

                # 3. Simula PC enviando PNG 1080x1920 para /.mps3_analytics/share/mps3_wrapped.png
                fake_png = b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR_MPS3_WRAPPED_TEST"
                share_rel = quote("/.mps3_analytics/share/mps3_wrapped.png")
                put_req = urllib.request.Request(
                    f"{base}/api/upload?path={share_rel}&idx=1&count=1&batchTotal={len(fake_png)}",
                    data=fake_png,
                    method="PUT",
                )
                with urllib.request.urlopen(put_req, timeout=3) as put_resp:
                    self.assertEqual(put_resp.status, 200)

                # 4. Simula Smartphone abrindo link do QR Code e baixando imagem com dl=1
                landing_html = urllib.request.urlopen(f"{base}/?share=wrapped&v=test1", timeout=3).read().decode("utf-8")
                self.assertIn("wrappedMobileLanding", landing_html)

                with urllib.request.urlopen(f"{base}/api/download?path={share_rel}&dl=1", timeout=3) as img_resp:
                    self.assertEqual(img_resp.status, 200)
                    self.assertEqual(img_resp.headers.get("Content-Type"), "image/png")
                    self.assertIn("attachment", img_resp.headers.get("Content-Disposition", ""))
                    self.assertEqual(img_resp.read(), fake_png)
            finally:
                server.shutdown()
                server.server_close()


if __name__ == "__main__":
    unittest.main()
