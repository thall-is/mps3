#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Camada 1 de Testes Automatizados — Motor JS do Scraper & Planner (tests/test_web_scraper_engine.py)
---------------------------------------------------------------------------------------------------
Executa diretamente o código JavaScript real de `components/wifi_transfer/web/src/js/09_podcast_sync.js`
dentro do runtime V8 (Node.js) e cruza resultados com o `hashlib.sha256` do Python para validar:
1. `sha256Bytes()` Pure-JS em vetores vazios, ASCII curtos e blocos binários multi-chunk (> 64 bytes).
2. `sanitizePodSafeName()` e `buildPodEpisodeFilename()` (remoção de acentos, caracteres FAT32, [videoId]).
3. `BrowserSyncPlanner.computeSyncPlan()` nos 6 cenários críticos de retenção:
   - Top-3 episódios mais recentes por canal.
   - Episódio já ouvido em `played_history` (`played_ledger`) nunca é re-baixado.
   - Episódio antigo (`played: false`) fora do Top-3 NUNCA é deletado (`keptUnplayed`).
   - Episódio antigo (`played: true`) fora do Top-3 é agendado em `deletes` e libera espaço.
   - Reserva de segurança de 32 MB (`SAFETY_RESERVE_BYTES`) bloqueia downloads quando SD está cheio.
4. `encodeAudioBufferToWavMono16()` gera cabeçalho RIFF/WAVE PCM 16-bit mono válido.
"""

import hashlib
import json
import os
import subprocess
import unittest

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
JS_EXTRACTOR_PATH = os.path.join(
    PROJECT_ROOT, "components", "wifi_transfer", "web", "src", "js", "09a_yt_extractor.js"
)
JS_MODULE_PATH = os.path.join(
    PROJECT_ROOT, "components", "wifi_transfer", "web", "src", "js", "09_podcast_sync.js"
)


def extract_pure_logic_js():
    with open(JS_EXTRACTOR_PATH, "r", encoding="utf-8") as f:
        ext_js = f.read()
    with open(JS_MODULE_PATH, "r", encoding="utf-8") as f:
        full_js = f.read()
    # Extrai do início até antes da seção 11.6 (que faz bind de eventos DOM)
    marker = "// --- 11.6 ESTADO DA UI"
    idx = full_js.find(marker)
    assert idx != -1, "Marcador da seção 11.6 não encontrado em 09_podcast_sync.js"
    return ext_js + "\n" + full_js[:idx]


class TestWebScraperEngineJS(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.core_js = extract_pure_logic_js()

    def run_js_expr(self, js_snippet):
        script = (
            self.core_js
            + "\n(async () => {\n"
            + js_snippet
            + "\n})().catch(e => { console.error(e); process.exit(1); });"
        )
        res = subprocess.run(
            ["node", "-"],
            input=script,
            capture_output=True,
            text=True,
            encoding="utf-8",
            timeout=10,
        )
        if res.returncode != 0:
            self.fail(f"Falha ao executar JS no Node:\nSTDERR: {res.stderr}\nSTDOUT: {res.stdout}")
        return json.loads(res.stdout.strip())

    def test_01_sha256_pure_js_matches_python_hashlib(self):
        vectors = [
            b"",
            b"abc",
            b"MPS3 Podcast Sync Integrity Check 2026 - UTF8",
            bytes(range(256)) * 17,  # 4352 bytes (múltiplos blocos de 64 bytes)
        ]
        for payload in vectors:
            expected_hex = hashlib.sha256(payload).hexdigest()
            arr_json = json.dumps(list(payload))
            actual_hex = self.run_js_expr(
                f"const h = sha256Bytes(new Uint8Array({arr_json})); console.log(JSON.stringify(h));"
            )
            self.assertEqual(actual_hex, expected_hex)

    def test_02_sanitize_and_filename_builder(self):
        out = self.run_js_expr(
            """
            const s1 = sanitizePodSafeName('Café & Ciência: Edição #42? *Especial* <2026>');
            const fname = buildPodEpisodeFilename({
                id: 'dQw4w9WgXcQ',
                title: 'Xadrez Verbal #380: Oriente Médio & Geopolítica',
                mtime: 1729382400, // 2024-10-20 UTC
                ext: 'm4a'
            });
            console.log(JSON.stringify({ s1, fname }));
            """
        )
        self.assertNotIn(":", out["s1"])
        self.assertNotIn("?", out["s1"])
        self.assertNotIn("*", out["s1"])
        self.assertIn("Cafe", out["s1"])
        self.assertEqual(
            out["fname"],
            "20241020 - Xadrez Verbal #380 Oriente Medio & Geopolitica [dQw4w9WgXcQ].m4a",
        )

    def test_03_planner_keep_recent_top3_and_played_retention(self):
        plan = self.run_js_expr(
            """
            const remote = [
                { id: 'v5', program: 'TechCast', title: 'Ep 5 Novo', mtime: 5000, size_bytes: 5000000, ext: 'm4a' },
                { id: 'v4', program: 'TechCast', title: 'Ep 4 Ja Ouvido', mtime: 4000, size_bytes: 5000000, ext: 'm4a' },
                { id: 'v3', program: 'TechCast', title: 'Ep 3 No SD', mtime: 3000, size_bytes: 5000000, ext: 'm4a' },
                { id: 'v2', program: 'TechCast', title: 'Ep 2 Antigo Ouvido', mtime: 2000, size_bytes: 5000000, ext: 'm4a' },
                { id: 'v1', program: 'TechCast', title: 'Ep 1 Antigo Nao Ouvido', mtime: 1000, size_bytes: 5000000, ext: 'm4a' }
            ];
            const manifest = {
                free_bytes: 500 * 1024 * 1024,
                podcasts: [
                    { program: 'TechCast', filename: '19700101 - Ep 3 No SD [v3].m4a', size: 5000000, played: false },
                    { program: 'TechCast', filename: '19700101 - Ep 2 Antigo Ouvido [v2].m4a', size: 5000000, played: true },
                    { program: 'TechCast', filename: '19700101 - Ep 1 Antigo Nao Ouvido [v1].m4a', size: 5000000, played: false }
                ],
                played_history: [
                    'TechCast/19700101 - Ep 4 Ja Ouvido [v4].m4a'
                ]
            };
            const plan = BrowserSyncPlanner.computeSyncPlan(remote, manifest, { keepRecent: 3, autoCleanPlayed: true });
            console.log(JSON.stringify(plan));
            """
        )
        # Top 3 são v5, v4, v3:
        # - v5: novo -> deve entrar em downloads
        # - v4: já consta em played_history -> deve entrar em skippedPlayed (NÃO baixa de novo)
        # - v3: já está no SD -> ignorado (e preservado em keptUnplayed)
        # Fora do Top 3 no SD (v2 e v1):
        # - v2 (played: true) -> deve entrar em deletes
        # - v1 (played: false) -> NUNCA deletado, deve entrar em keptUnplayed
        dl_ids = [d["id"] for d in plan["downloads"]]
        skip_ids = [s["id"] for s in plan["skippedPlayed"]]
        del_fnames = [d["filename"] for d in plan["deletes"]]
        kept_fnames = [k["filename"] for k in plan["keptUnplayed"]]

        self.assertEqual(dl_ids, ["v5"])
        self.assertEqual(skip_ids, ["v4"])
        self.assertEqual(del_fnames, ["19700101 - Ep 2 Antigo Ouvido [v2].m4a"])
        self.assertIn("19700101 - Ep 1 Antigo Nao Ouvido [v1].m4a", kept_fnames)

    def test_04_planner_respects_32mb_safety_reserve(self):
        plan = self.run_js_expr(
            """
            const remote = [
                { id: 'big1', program: 'Pod', title: 'Grande 1', mtime: 2000, size_bytes: 20 * 1024 * 1024, ext: 'm4a' },
                { id: 'big2', program: 'Pod', title: 'Grande 2', mtime: 1000, size_bytes: 20 * 1024 * 1024, ext: 'm4a' }
            ];
            // 55 MB livres - 32 MB reserva = 23 MB disponíveis (cabe big1 de 20MB, não cabe big2 de 20MB)
            const manifest = {
                free_bytes: 55 * 1024 * 1024,
                podcasts: [],
                played_history: []
            };
            const plan = BrowserSyncPlanner.computeSyncPlan(remote, manifest, { keepRecent: 3 });
            console.log(JSON.stringify(plan));
            """
        )
        self.assertEqual(len(plan["downloads"]), 2)
        self.assertTrue(plan["downloads"][0]["selected"])
        self.assertFalse(plan["downloads"][1]["selected"])
        self.assertTrue(plan["downloads"][1].get("skipped_low_space"))

    def test_05_wav_mono16_encoder_valid_riff_header(self):
        header_info = self.run_js_expr(
            """
            const fakeBuffer = {
                numberOfChannels: 2,
                sampleRate: 44100,
                length: 4410,
                getChannelData: () => new Float32Array(4410).fill(0.25)
            };
            const wav = encodeAudioBufferToWavMono16(fakeBuffer, 22050);
            const riff = String.fromCharCode(wav[0], wav[1], wav[2], wav[3]);
            const wave = String.fromCharCode(wav[8], wav[9], wav[10], wav[11]);
            console.log(JSON.stringify({ byteLength: wav.byteLength, riff, wave }));
            """
        )
        self.assertEqual(header_info["riff"], "RIFF")
        self.assertEqual(header_info["wave"], "WAVE")
        # 4410 samples @ 44.1kHz -> 2205 samples @ 22.05kHz -> 44 + 2205*2 = 4454 bytes
        self.assertEqual(header_info["byteLength"], 44 + 2205 * 2)


if __name__ == "__main__":
    unittest.main()
