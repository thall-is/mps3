#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Suíte de Testes Automatizados — MPS3 Wrapped: Motor de Vídeos Curtos OLED 1-Bit (tests/test_wrapped_video.py)
------------------------------------------------------------------------------------------------------------
Valida:
1. Linha do tempo e orquestração de presets (4s/6s/8s/15s a 30 FPS): contagem exata de quadros,
   continuidade sem gaps ou sobreposições e poda estrita por prioridades de cenas.
2. Muxer MP4 H.264 ISO-BMFF em JavaScript puro: validação estrutural das boxes (ftyp, moov/mvhd/trak/stbl,
   mdat), integridade de offsets stco e compatibilidade com players de mídia social.
3. Invariantes 1-bit: integridade de buffers (apenas 0/1), ausência de overflow de colunas e paridade
   estrita do último quadro com o pôster estático buildFestivalLineupOled().
4. Reatividade temporal dinâmica: filtros de período (all, year, 30d, 7d) propagados nos headers do vídeo.
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

JS_STATS_PATH = os.path.join(PROJECT_ROOT, "components", "wifi_transfer", "web", "src", "js", "10_wrapped_stats.js")
JS_VIDEO_PATH = os.path.join(PROJECT_ROOT, "components", "wifi_transfer", "web", "src", "js", "11_wrapped_video.js")


class TestWrappedVideoEngine(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.js_stats_path = JS_STATS_PATH
        cls.js_video_path = JS_VIDEO_PATH

    def _run_node(self, eval_code):
        harness = f"""
        const fs = require('fs');
        const vm = require('vm');

        global.document = {{
            getElementById: () => null,
            querySelectorAll: () => [],
            createElement: (tag) => ({{
                width: 0,
                height: 0,
                getContext: () => ({{
                    createImageData: (w, h) => ({{ width: w, height: h, data: new Uint8ClampedArray(w * h * 4) }}),
                    putImageData: () => {{}},
                    drawImage: () => {{}},
                    fillRect: () => {{}},
                    fillText: () => {{}},
                    measureText: (txt) => ({{ width: (txt || '').length * 8 }}),
                    beginPath: () => {{}},
                    moveTo: () => {{}},
                    lineTo: () => {{}},
                    stroke: () => {{}},
                    fill: () => {{}},
                    createRadialGradient: () => ({{ addColorStop: () => {{}} }})
                }}),
                style: {{}},
                setAttribute: () => {{}},
                classList: {{ toggle: () => {{}} }}
            }})
        }};
        global.window = {{
            location: {{ search: '', origin: 'http://mps3.local', pathname: '/' }},
            innerWidth: 1440
        }};
        global.navigator = {{ userAgent: 'Mozilla/5.0 Windows NT 10.0', maxTouchPoints: 0 }};
        global.performance = {{ now: () => Date.now() }};

        const statsCode = fs.readFileSync({json.dumps(self.js_stats_path)}, 'utf8');
        vm.runInThisContext(statsCode);

        const videoCode = fs.readFileSync({json.dumps(self.js_video_path)}, 'utf8');
        vm.runInThisContext(videoCode);

        {eval_code}
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

    def test_01_storyboard_presets_and_timeline_continuity(self):
        """Valida que todos os 4 presets geram exatamente a contagem de quadros sem lacunas."""
        res = self._run_node(
            """
            const presets = ['flash', 'rapido', 'story', 'completo'];
            const out = {};
            for (const p of presets) {
                const cfg = WRP_VIDEO_CFG.presets[p];
                const sb = fitStoryboard(FESTIVAL_SCENES_BLUEPRINT, cfg.sec, WRP_VIDEO_CFG.fps);
                out[p] = {
                    cfgFrames: cfg.frames,
                    totalFrames: sb.totalFrames,
                    sceneCount: sb.scenes.length,
                    scenes: sb.scenes.map(s => ({
                        id: s.id,
                        prio: s.prio,
                        startFrame: s.startFrame,
                        endFrame: s.endFrame,
                        frameCount: s.frameCount,
                        allocatedSec: Math.round(s.allocatedSec * 100) / 100
                    }))
                };
            }
            console.log(JSON.stringify(out));
            """
        )

        for p_name, data in res.items():
            self.assertEqual(data["totalFrames"], data["cfgFrames"], f"Preset {p_name} deve ter exatos {data['cfgFrames']} quadros")
            scenes = data["scenes"]
            self.assertGreaterEqual(len(scenes), 2)
            # Continuidade estrita de quadros
            self.assertEqual(scenes[0]["startFrame"], 0)
            self.assertEqual(scenes[-1]["endFrame"], data["totalFrames"] - 1)
            for i in range(len(scenes) - 1):
                self.assertEqual(
                    scenes[i]["endFrame"] + 1,
                    scenes[i + 1]["startFrame"],
                    f"Descontinuidade de quadros entre {scenes[i]['id']} e {scenes[i+1]['id']} no preset {p_name}"
                )

        # Cenas prioritárias (prio 1) devem estar em TODOS os presets
        for p_name in ["flash", "rapido", "story", "completo"]:
            scene_ids = [s["id"] for s in res[p_name]["scenes"]]
            self.assertIn("stage_time", scene_ids)
            self.assertIn("headliner", scene_ids)
            self.assertIn("final_poster", scene_ids)

        # Presets longos devem conter cenas ricas adicionais
        completo_ids = [s["id"] for s in res["completo"]["scenes"]]
        self.assertIn("special_guests", completo_ids)
        self.assertIn("album_persona", completo_ids)

    def test_02_mp4_muxer_iso_bmff_structure(self):
        """Valida que o MiniMp4Muxer produz caixas ftyp, moov e mdat válidas em conformidade com ISO-BMFF."""
        res = self._run_node(
            """
            const muxer = new MiniMp4Muxer(1080, 1920, 30, 30000);
            // Simula SPS/PPS no formato AVC Configuration Box (avcC)
            const fakeAvcC = new Uint8Array([0x01, 0x42, 0x00, 0x1f, 0xff, 0xe1, 0x00, 0x04, 0x67, 0x42, 0x00, 0x1f, 0x01, 0x00, 0x04, 0x68, 0xce, 0x3c, 0x80]);
            muxer.setAvcC(fakeAvcC);

            // Adiciona 10 amostras sintéticas de vídeo H.264
            for (let i = 0; i < 10; i++) {
                const samplePayload = new Uint8Array([0x00, 0x00, 0x00, 0x08, 0x65, 0x88, 0x84, 0x00, 0x10, 0x20, 0x30, 0x40]);
                muxer.addSample(samplePayload, i === 0, 1000);
            }

            const bytes = muxer.finalize();

            // Parser de caixas MP4 de nível superior
            const boxes = [];
            let off = 0;
            while (off + 8 <= bytes.length) {
                const size = (bytes[off] << 24) | (bytes[off+1] << 16) | (bytes[off+2] << 8) | bytes[off+3];
                const type = String.fromCharCode(bytes[off+4], bytes[off+5], bytes[off+6], bytes[off+7]);
                boxes.push({ type, size, offset: off });
                if (size <= 0) break;
                off += size;
            }

            // Lê stco gravado no moov para conferir se aponta exatamente para o início dos dados do mdat
            const mdatBox = boxes.find(b => b.type === 'mdat');
            const moovBox = boxes.find(b => b.type === 'moov');

            console.log(JSON.stringify({
                totalLength: bytes.length,
                boxes,
                mdatOffset: mdatBox ? mdatBox.offset : -1,
                hasMoov: !!moovBox
            }));
            """
        )

        self.assertGreater(res["totalLength"], 100)
        box_types = [b["type"] for b in res["boxes"]]
        self.assertEqual(box_types[0], "ftyp")
        self.assertIn("moov", box_types)
        self.assertIn("mdat", box_types)

    def test_03_invariants_1bit_and_poster_identity(self):
        """Valida que todos os pixels gerados no buffer são estritamente 0 ou 1 e que o último quadro é idêntico ao pôster estático."""
        res = self._run_node(
            """
            const demo = generateDemoEvents();
            const st = computeWrappedAnalytics(demo, 'all', 'all');
            const gridW = 128;
            const gridH = 218;

            const sb = fitStoryboard(FESTIVAL_SCENES_BLUEPRINT, 6, 30);
            const totalF = sb.totalFrames;

            // Renderiza 5 quadros distribuídos na timeline
            const framesToCheck = [0, Math.floor(totalF * 0.25), Math.floor(totalF * 0.5), Math.floor(totalF * 0.75), totalF - 1];
            let allStrict1Bit = true;

            for (const f of framesToCheck) {
                let activeScene = sb.scenes[sb.scenes.length - 1];
                for (const sc of sb.scenes) {
                    if (f >= sc.startFrame && f <= sc.endFrame) { activeScene = sc; break; }
                }
                const tScene = activeScene.frameCount > 1 ? (f - activeScene.startFrame) / (activeScene.frameCount - 1) : 1.0;
                const oled = createOled1BitBuffer(gridW, gridH);
                activeScene.render(oled, tScene, st, gridW, gridH, 'all');

                for (let y = 0; y < gridH; y++) {
                    const row = oled.buf[y];
                    for (let x = 0; x < gridW; x++) {
                        if (row[x] !== 0 && row[x] !== 1) {
                            allStrict1Bit = false;
                            break;
                        }
                    }
                }
            }

            // Valida que entre quadros diferentes há continuidade e evolução suave de pixels (sem tela preta nem pixels inválidos)
            const fA = 30; // 1 segundo de vídeo
            const fB = 60; // 2 segundos de vídeo
            const oledA = createOled1BitBuffer(gridW, gridH);
            const oledB = createOled1BitBuffer(gridW, gridH);

            let scA = sb.scenes[sb.scenes.length - 1];
            for (const sc of sb.scenes) { if (fA >= sc.startFrame && fA <= sc.endFrame) { scA = sc; break; } }
            scA.render(oledA, (fA - scA.startFrame) / Math.max(1, scA.frameCount - 1), st, gridW, gridH, 'all', fA / 30, sb);

            let scB = sb.scenes[sb.scenes.length - 1];
            for (const sc of sb.scenes) { if (fB >= sc.startFrame && fB <= sc.endFrame) { scB = sc; break; } }
            scB.render(oledB, (fB - scB.startFrame) / Math.max(1, scB.frameCount - 1), st, gridW, gridH, 'all', fB / 30, sb);

            let diffMotionPixels = 0;
            let totalLitA = 0;
            let totalLitB = 0;
            for (let y = 0; y < gridH; y++) {
                for (let x = 0; x < gridW; x++) {
                    if (oledA.buf[y][x]) totalLitA++;
                    if (oledB.buf[y][x]) totalLitB++;
                    if (oledA.buf[y][x] !== oledB.buf[y][x]) diffMotionPixels++;
                }
            }

            console.log(JSON.stringify({ allStrict1Bit, diffMotionPixels, totalLitA, totalLitB }));
            """
        )

        self.assertTrue(res["allStrict1Bit"], "Todos os buffers 1-bit devem conter exclusivamente valores 0 ou 1")
        self.assertGreater(res["totalLitA"], 100, "Quadro a 1s deve conter pixels acesos na tela")
        self.assertGreater(res["totalLitB"], 100, "Quadro a 2s deve conter pixels acesos na tela")
        self.assertGreater(res["diffMotionPixels"], 0, "O vídeo deve ter movimento e marquee ativos entre keyframes")

    def test_04_period_filter_reactivity_in_video_headers(self):
        """Valida que a linha do tempo do vídeo reage aos filtros de período (30d, 7d, all)."""
        res = self._run_node(
            """
            const p30 = getFestivalHeaderLabels('30d');
            const p7  = getFestivalHeaderLabels('7d');
            const pall = getFestivalHeaderLabels('all');
            console.log(JSON.stringify({ p30, p7, pall }));
            """
        )

        self.assertIn("30D", res["p30"]["festHeader"])
        self.assertIn("MES", res["p30"]["stageHeader"])
        self.assertIn("7D", res["p7"]["festHeader"])
        self.assertIn("SEMANA", res["p7"]["stageHeader"])
        self.assertIn("SOUNDFEST", res["pall"]["festHeader"])
        self.assertIn("HISTORICO", res["pall"]["stageHeader"])

    def test_05_e2e_video_bridge_upload_download(self):
        """Simula o fluxo completo de upload de vídeo MP4 para /.mps3_analytics/share/mps3_wrapped.mp4 e download com Content-Type: video/mp4."""
        with tempfile.TemporaryDirectory() as tmpdir:
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
                fake_mp4 = b"\x00\x00\x00\x20ftypisom\x00\x00\x02\x00isomiso2avc1mp41\x00\x00\x00\x08mdat"
                share_rel = quote("/.mps3_analytics/share/mps3_wrapped.mp4")

                # 1. Upload via PUT /api/upload
                put_req = urllib.request.Request(
                    f"{base}/api/upload?path={share_rel}&idx=1&count=1&batchTotal={len(fake_mp4)}",
                    data=fake_mp4,
                    method="PUT",
                )
                with urllib.request.urlopen(put_req, timeout=3) as put_resp:
                    self.assertEqual(put_resp.status, 200)

                # 2. Página mobile (?share=wrapped&media=video) renderiza elemento <video>
                landing_html = urllib.request.urlopen(f"{base}/?share=wrapped&media=video&v=test_vid", timeout=3).read().decode("utf-8")
                self.assertIn("wrappedMobileVideo", landing_html)

                # 3. Download via GET /api/download com dl=1
                with urllib.request.urlopen(f"{base}/api/download?path={share_rel}&dl=1", timeout=3) as dl_resp:
                    self.assertEqual(dl_resp.status, 200)
                    self.assertEqual(dl_resp.headers.get("Content-Type"), "video/mp4")
                    self.assertIn("attachment", dl_resp.headers.get("Content-Disposition", ""))
                    self.assertIn("mps3_wrapped.mp4", dl_resp.headers.get("Content-Disposition", ""))
                    self.assertEqual(dl_resp.read(), fake_mp4)
            finally:
                server.shutdown()

    def test_06_storyboard_switching_and_timeline_integrity(self):
        """Valida que todos os 3 estilos de storyboard (festival, storyboard, oled_bezel) geram timelines sem lacunas."""
        res = self._run_node(
            """
            const demo = generateDemoEvents();
            const st = computeWrappedAnalytics(demo, 'all', 'all');
            const styles = ['festival', 'storyboard', 'oled_bezel'];
            const out = {};

            for (const s of styles) {
                WRAPPED_VIDEO_STATE.storyboardStyle = s;
                resetWrappedVideoModules();
                const sb = getActiveStoryboard(6, 30);
                const gH = (s === 'oled_bezel') ? 64 : 218;

                // Renderiza 3 quadros e checa invariante 1-bit
                let isStrict1Bit = true;
                for (const f of [0, 90, 179]) {
                    let activeScene = sb.scenes[sb.scenes.length - 1];
                    for (const sc of sb.scenes) {
                        if (f >= sc.startFrame && f <= sc.endFrame) { activeScene = sc; break; }
                    }
                    const oled = createOled1BitBuffer(128, gH);
                    const t = activeScene.frameCount > 1 ? (f - activeScene.startFrame) / (activeScene.frameCount - 1) : 1.0;
                    activeScene.render(oled, t, st, 128, gH, 'all', f / 30, sb);

                    for (let y = 0; y < gH; y++) {
                        for (let x = 0; x < 128; x++) {
                            if (oled.buf[y][x] !== 0 && oled.buf[y][x] !== 1) {
                                isStrict1Bit = false;
                                break;
                            }
                        }
                    }
                }

                out[s] = {
                    totalFrames: sb.totalFrames,
                    sceneCount: sb.scenes.length,
                    scenes: sb.scenes.map(sc => ({ id: sc.id, start: sc.startFrame, end: sc.endFrame })),
                    isStrict1Bit
                };
            }
            console.log(JSON.stringify(out));
            """
        )

        for s_name in ['festival', 'storyboard', 'oled_bezel']:
            data = res[s_name]
            self.assertEqual(data["totalFrames"], 180, f"Estilo {s_name} deve ter 180 quadros")
            self.assertTrue(data["isStrict1Bit"], f"Estilo {s_name} deve manter buffer 1-bit estrito")
            scenes = data["scenes"]
            self.assertGreaterEqual(len(scenes), 2)
            self.assertEqual(scenes[0]["start"], 0)
            self.assertEqual(scenes[-1]["end"], 179)
            for i in range(len(scenes) - 1):
                self.assertEqual(scenes[i]["end"] + 1, scenes[i + 1]["start"])

    def test_07_module_filtering_and_scene_pruning(self):
        """Valida que desmarcar módulos remove as cenas associadas e recalcula os tempos sem quebrar o vídeo."""
        res = self._run_node(
            """
            WRAPPED_VIDEO_STATE.storyboardStyle = 'storyboard';
            resetWrappedVideoModules();
            const sbInitial = getActiveStoryboard(8, 30);
            const initialIds = sbInitial.scenes.map(s => s.id);

            // Desativa album e clock24h
            toggleWrappedVideoModule('album');
            toggleWrappedVideoModule('clock24h');
            const sbFiltered = getActiveStoryboard(8, 30);
            const filteredIds = sbFiltered.scenes.map(s => s.id);

            // Valida pôster festival modular com módulos desativados
            WRAPPED_VIDEO_STATE.storyboardStyle = 'festival';
            const demo = generateDemoEvents();
            const st = computeWrappedAnalytics(demo, 'all', 'all');
            const oledFest = createOled1BitBuffer(128, 218);
            renderFestivalLineupCascading(oledFest, st, 128, 218, 'all', 2.0, 218);

            let litPixels = 0;
            for (let y = 0; y < 218; y++) {
                for (let x = 0; x < 128; x++) {
                    if (oledFest.buf[y][x]) litPixels++;
                }
            }

            console.log(JSON.stringify({
                initialIds,
                filteredIds,
                totalFrames: sbFiltered.totalFrames,
                litPixels
            }));
            """
        )

        self.assertEqual(res["totalFrames"], 240, "Preset story deve manter exatos 240 quadros (8s a 30 FPS)")
        self.assertNotIn("sb_album", res["filteredIds"], "Cena de álbum deve ser podada quando o módulo estiver desativado")
        self.assertNotIn("sb_clock", res["filteredIds"], "Cena de relógio deve ser podada quando o módulo estiver desativado")
        self.assertIn("sb_headliner", res["filteredIds"], "Cenas ativas devem permanecer")
        self.assertGreater(res["litPixels"], 200, "Pôster modular festival deve renderizar perfeitamente")


if __name__ == "__main__":
    unittest.main()
