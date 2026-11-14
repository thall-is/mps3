#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Suíte de Testes Automatizados — Arquitetura Web Modular (tests/test_web_modular_build.py)
----------------------------------------------------------------------------------------
Valida:
1. Existência e integridade UTF-8 de todos os 19 arquivos modulares em `web/src/`.
2. Ausência de diretivas `@include` órfãs ou não expandidas após a montagem.
3. Unicidade de atributos `id="..."` em todas as views HTML (`src/views/*.html`).
4. Paridade de 100% dos IDs de DOM mapeados no objeto `el` e modais principais.
5. Presença de todas as funções JS críticas de cada subsistema.
6. Sincronia byte-a-byte entre `src/` -> `web/index.html` -> `include/index_html_gz.h`.
"""

import gzip
import os
import re
import sys
import tempfile
import unittest

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(PROJECT_ROOT, "tools"))

import pack_web  # noqa: E402


EXPECTED_MODULES = [
    "js/01_theme_head.js",
    "css/01_theme_vars.css",
    "css/02_layout_table.css",
    "css/03_player_eq_modals.css",
    "css/04_wrapped_stats.css",
    "views/01_explorer.html",
    "views/02_player_menus.html",
    "views/03_modals_fs.html",
    "views/04_modal_ota.html",
    "views/05_modal_podcasts.html",
    "views/06_modals_cfg_eq.html",
    "views/07_modal_wrapped.html",
    "js/02_core_auth_utils.js",
    "js/03_status_explorer.js",
    "js/04_web_audio_player.js",
    "js/05_file_ops_modals.js",
    "js/06_upload_metadata.js",
    "js/07_wifi_and_eq.js",
    "js/08_ota_updater.js",
    "js/09a_yt_extractor.js",
    "js/09_podcast_sync.js",
    "js/10_wrapped_stats.js",
    "js/11_wrapped_video.js",
]

REQUIRED_DOM_IDS = [
    "siteFavicon",
    "header-status",
    "batInfo",
    "sdTrafficBox",
    "sdDot",
    "sdTrafficText",
    "sdInfoText",
    "sdBar",
    "upload-status-box",
    "upFileName",
    "upFilePct",
    "upFileBar",
    "upOverallLabel",
    "upOverallPct",
    "upOverallBar",
    "upSpeed",
    "upEta",
    "queue",
    "podBtn",
    "eqBtn",
    "mkdirBtn",
    "themeToggleBtn",
    "configBtn",
    "otaBtn",
    "batchBar",
    "batchCountText",
    "btnBatchDownload",
    "btnBatchMove",
    "btnBatchCopy",
    "btnBatchDelete",
    "btnBatchClear",
    "filterInput",
    "crumbs",
    "chkSelectAll",
    "tbody",
    "dropzone",
    "inpFiles",
    "inpFolder",
    "player-bar",
    "audioEl",
    "albumArt",
    "trackTitle",
    "trackArtist",
    "playBtn",
    "timeCur",
    "progressCont",
    "progressFill",
    "timeTot",
    "fs-player",
    "fsArt",
    "visualizer",
    "fsTitle",
    "fsArtist",
    "eqBass",
    "eqMid",
    "eqTreble",
    "ctxMenu",
    "mkdirModal",
    "moveModal",
    "otaModal",
    "podModal",
    "configModal",
    "eqModal",
    "wrappedBtn",
    "wrappedModal",
    "wrappedCanvas",
    "wrpQrBridgeBox",
    "wrpVideoControlsBox",
    "btnWrpGenerateVideo",
    "wrappedMobileLanding",
    "wrappedMobileVideo",
    "cloudSyncStatusBadge",
    "btnCloudCheckUpdate",
    "btnCloudResetCache",
]

REQUIRED_JS_FUNCTIONS = [
    "updateFaviconAndUI",
    "setTheme",
    "toggleTheme",
    "fmtSize",
    "fmtTime",
    "initWebAudio",
    "drawVisualizer",
    "toggleFullscreen",
    "fetchStatus",
    "renderTable",
    "updateBatchBar",
    "load",
    "playFromFolder",
    "loadTrack",
    "togglePlay",
    "prevTrack",
    "nextTrack",
    "openMkdirModal",
    "closeMkdirModal",
    "openMoveModal",
    "closeMoveModal",
    "renderQueue",
    "getFilesFromEntry",
    "loadNetworks",
    "deleteNetwork",
    "renderEqBands",
    "loadEq",
    "saveEq",
    "openOtaModal",
    "closeOtaModal",
    "openPodModal",
    "closePodModal",
    "sha256Bytes",
    "sanitizePodSafeName",
    "buildPodEpisodeFilename",
    "parseYoutubeAtomXml",
    "parsePodcastRssXml",
    "scrapeYoutubeSubscription",
    "resolveYoutubeAudioStream",
    "encodeAudioBufferToWavMono16",
    "openWrappedModal",
    "closeWrappedModal",
    "computeWrappedAnalytics",
    "renderWrappedCanvas",
    "sendWrappedToPhoneViaMps3",
    "generateQrSvg",
    "fitStoryboard",
    "blitFast",
    "toggleWrappedVideoPreview",
    "generateWrappedVideo",
    "downloadWrappedVideo",
    "sendWrappedVideoToPhoneViaMps3",
]


class TestWebModularBuild(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.paths = pack_web.get_paths(PROJECT_ROOT)
        cls.src_dir = cls.paths["src_dir"]
        cls.template_path = cls.paths["template_path"]
        cls.html_path = cls.paths["html_path"]
        cls.header_path = cls.paths["header_path"]

    def test_01_all_modular_files_exist_and_have_headers(self):
        self.assertTrue(os.path.exists(self.template_path), "index.template.html ausente")
        for rel_mod in EXPECTED_MODULES:
            mod_path = os.path.join(self.src_dir, rel_mod)
            self.assertTrue(os.path.exists(mod_path), f"Módulo ausente: {rel_mod}")
            with open(mod_path, "r", encoding="utf-8") as f:
                content = f.read()
            self.assertGreater(len(content.strip()), 50, f"Módulo vazio ou truncado: {rel_mod}")
            pack_web.check_utf8_and_mojibake(mod_path, content)

    def test_02_unique_html_ids_across_views(self):
        duplicates = pack_web.validate_unique_html_ids(self.src_dir)
        self.assertEqual(duplicates, [], f"IDs duplicados detectados nas views: {duplicates}")

    def test_03_assemble_from_src_completeness(self):
        assembled_html, included = pack_web.assemble_from_src(self.src_dir, self.template_path)
        self.assertEqual(included, EXPECTED_MODULES)
        self.assertNotIn("@include", assembled_html)
        self.assertTrue(assembled_html.lower().startswith("<!doctype html>"))

        for dom_id in REQUIRED_DOM_IDS:
            pattern = re.compile(rf"""\bid\s*=\s*["']{re.escape(dom_id)}["']""")
            self.assertRegex(
                assembled_html,
                pattern,
                f"Elemento DOM obrigatório id='{dom_id}' não encontrado no HTML montado",
            )

        for fn_name in REQUIRED_JS_FUNCTIONS:
            fn_pattern = re.compile(rf"\bfunction\s+{re.escape(fn_name)}\s*\(")
            self.assertRegex(
                assembled_html,
                fn_pattern,
                f"Função JS obrigatória '{fn_name}()' não encontrada no HTML montado",
            )

    def test_04_error_detection_on_missing_include(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            bad_tpl = os.path.join(tmpdir, "index.template.html")
            with open(bad_tpl, "w", encoding="utf-8") as f:
                f.write("<!doctype html>\n<!-- @include js/nao_existe.js -->\n")
            with self.assertRaises(FileNotFoundError):
                pack_web.assemble_from_src(tmpdir, bad_tpl)

    def test_05_pack_and_gzip_header_sync(self):
        rc = pack_web.pack_web(check_only=False)
        self.assertEqual(rc, 0)

        with open(self.html_path, "rb") as f:
            raw_html = f.read()

        with open(self.header_path, "r", encoding="utf-8") as f:
            header_text = f.read()

        hex_vals = re.findall(r"0x([0-9a-fA-F]{2})", header_text)
        gz_bytes = bytes(int(h, 16) for h in hex_vals)
        decompressed = gzip.decompress(gz_bytes)
        self.assertEqual(
            decompressed,
            raw_html,
            "O payload descompactado de index_html_gz.h não coincide byte-a-byte com web/index.html",
        )

    def test_06_export_cloud_artifacts(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            rc = pack_web.pack_web(check_only=True, export_cloud_dir=tmpdir)
            self.assertEqual(rc, 0)
            cloud_html = os.path.join(tmpdir, "index.html")
            cloud_ver = os.path.join(tmpdir, "version.json")
            cloud_nojekyll = os.path.join(tmpdir, ".nojekyll")
            self.assertTrue(os.path.exists(cloud_html), "index.html não exportado no diretório cloud")
            self.assertTrue(os.path.exists(cloud_ver), "version.json não exportado no diretório cloud")
            self.assertTrue(os.path.exists(cloud_nojekyll), ".nojekyll não exportado no diretório cloud")
            import json
            with open(cloud_ver, "r", encoding="utf-8") as vf:
                manifest = json.load(vf)
            self.assertEqual(len(manifest.get("hash", "")), 12)
            self.assertEqual(len(manifest.get("sha256", "")), 64)
            self.assertGreater(manifest.get("size", 0), 100000)


if __name__ == "__main__":
    unittest.main()
