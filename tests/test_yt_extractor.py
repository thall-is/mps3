#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Suíte de Testes Modulares do Extrator YouTube 2026 (tests/test_yt_extractor.py)
-------------------------------------------------------------------------------
Valida o subsistema isolado do Scraper YouTube (`09a_yt_extractor.js` + `tools/yt_scraper_core.py`)
sem depender do hardware MPS3:
1. Paridade de versão, protobuf `tabParams` (`videos`, `streams`, `podcasts`) e clientes InnerTube
   entre o módulo JS do navegador (`09a_yt_extractor.js`) e o núcleo Python (`yt_scraper_core.py`).
2. Presença do canal `Desce a Letra Show` (`https://www.youtube.com/@DESCEALETRASHOW/streams`)
   com `channel_id="UCCD93px0UneWNFg_HxV4naQ"` e `tab="streams"` nos canais padrão.
3. Detecção automática de abas (`/streams`, `/live`, `/podcasts`, `/videos`) em JS e Python.
4. Filtragem cirúrgica de vídeos exclusivos para membros (`CLUBINHO DOS MEMBROS` / `BADGE_MEMBERS_ONLY`),
   lives ativas (`BADGE_STYLE_TYPE_LIVE_NOW`) e estreias agendadas (`upcomingEventData`),
   garantindo que SEMPRE os episódios públicos concluídos mais recentes sejam selecionados.
5. Teste Ao Vivo (Live YouTube Integration) contra `https://www.youtube.com/@DESCEALETRASHOW/streams`,
   validando listagem real, filtragem de membros e download de stream M4A (`itag=140`, `ftypdash`).
"""

import json
import os
import subprocess
import sys
import urllib.request
import unittest

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(PROJECT_ROOT, "tools"))

import yt_scraper_core  # noqa: E402

JS_EXTRACTOR_PATH = os.path.join(
    PROJECT_ROOT, "components", "wifi_transfer", "web", "src", "js", "09a_yt_extractor.js"
)
JS_SYNC_PATH = os.path.join(
    PROJECT_ROOT, "components", "wifi_transfer", "web", "src", "js", "09_podcast_sync.js"
)


# Fixture realista da aba /streams do @DESCEALETRASHOW (YouTube InnerTube 2026 lockupViewModel)
DLSHOW_STREAMS_FIXTURE = {
    "contents": {
        "twoColumnBrowseResultsRenderer": {
            "tabs": [
                {
                    "tabRenderer": {
                        "title": "Ao vivo",
                        "selected": True,
                        "content": {
                            "richGridRenderer": {
                                "contents": [
                                    # Item 0: Live ativa no momento (deve ser ignorada)
                                    {
                                        "richItemRenderer": {
                                            "content": {
                                                "lockupViewModel": {
                                                    "contentId": "LIVE_NOW_11",
                                                    "metadata": {
                                                        "lockupMetadataViewModel": {
                                                            "title": {"content": "DLSHOW AO VIVO AGORA"}
                                                        }
                                                    },
                                                    " overlays": [
                                                        {
                                                            "badgeStyle": "BADGE_STYLE_TYPE_LIVE_NOW",
                                                            "text": "AO VIVO",
                                                        }
                                                    ],
                                                }
                                            }
                                        }
                                    },
                                    # Item 1: Episódio público mais recente (#1 real!)
                                    {
                                        "richItemRenderer": {
                                            "content": {
                                                "lockupViewModel": {
                                                    "contentId": "XYFMsxvEtmY",
                                                    "metadata": {
                                                        "lockupMetadataViewModel": {
                                                            "title": {
                                                                "content": "FLÁVIO, LIBERA O SIGILO // DLSHOW 660"
                                                            }
                                                        }
                                                    },
                                                }
                                            }
                                        }
                                    },
                                    # Item 2: Exclusivo para membros (CLUBINHO DOS MEMBROS -> deve ser ignorado!)
                                    {
                                        "richItemRenderer": {
                                            "content": {
                                                "lockupViewModel": {
                                                    "contentId": "JBFomEmr5Os",
                                                    "metadata": {
                                                        "lockupMetadataViewModel": {
                                                            "title": {"content": "CLUBINHO DOS MEMBROS"}
                                                        }
                                                    },
                                                    "badges": [
                                                        {
                                                            "badgeViewModel": {
                                                                "badgeStyle": "BADGE_MEMBERS_ONLY",
                                                                "badgeText": "Só para membros",
                                                                "iconName": "SPONSORSHIP_STAR",
                                                            }
                                                        }
                                                    ],
                                                }
                                            }
                                        }
                                    },
                                    # Item 3: Segundo episódio público mais recente (#2 real!)
                                    {
                                        "richItemRenderer": {
                                            "content": {
                                                "lockupViewModel": {
                                                    "contentId": "Dz56Nq-6AuE",
                                                    "metadata": {
                                                        "lockupMetadataViewModel": {
                                                            "title": {
                                                                "content": "VORCAROVERSO É MAIOR DO QUE A GENTE IMAGINAVA // DLSHOW 659"
                                                            }
                                                        }
                                                    },
                                                }
                                            }
                                        }
                                    },
                                    # Item 4: Terceiro episódio público mais recente (#3 real!)
                                    {
                                        "richItemRenderer": {
                                            "content": {
                                                "lockupViewModel": {
                                                    "contentId": "MiZu0sOxrC4",
                                                    "metadata": {
                                                        "lockupMetadataViewModel": {
                                                            "title": {
                                                                "content": "VITÓRIA DO TRABALHADOR, DERROTA DO FLÁVIO // DLSHOW 658"
                                                            }
                                                        }
                                                    },
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
    }
}


class TestModularYoutubeExtractor(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(JS_EXTRACTOR_PATH, "r", encoding="utf-8") as f:
            cls.extractor_js = f.read()
        with open(JS_SYNC_PATH, "r", encoding="utf-8") as f:
            full_sync = f.read()
        idx = full_sync.find("// --- 11.6 ESTADO DA UI")
        cls.sync_core_js = full_sync[:idx]

    def run_js(self, snippet: str):
        script = (
            self.extractor_js
            + "\n"
            + self.sync_core_js
            + "\n(async () => {\n"
            + snippet
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
            self.fail(f"Erro no Node.js:\n{res.stderr}")
        return json.loads(res.stdout.strip())

    def test_01_js_and_python_extractor_version_and_params_parity(self):
        js_info = self.run_js(
            """
            console.log(JSON.stringify({
                version: MPS3_YT_EXTRACTOR.VERSION,
                tabParams: MPS3_YT_EXTRACTOR.CONFIG.tabParams,
                defaultChannels: DEFAULT_POD_SUBSCRIPTIONS
            }));
            """
        )
        self.assertEqual(js_info["version"], yt_scraper_core.EXTRACTOR_VERSION)
        self.assertEqual(js_info["tabParams"], yt_scraper_core.CONFIG["tab_params"])

        # Verifica os 4 canais padrão configurados (3 RSS Oficiais + 1 YouTube /streams)
        defaults = js_info["defaultChannels"]
        self.assertEqual(len(defaults), 4)
        self.assertEqual(defaults[0]["name"], "NerdCast")
        self.assertEqual(defaults[0]["type"], "rss")
        self.assertEqual(defaults[0]["url"], "https://feeds.megaphone.fm/JNPD6227286900")
        self.assertEqual(defaults[1]["name"], "Vortex")
        self.assertEqual(defaults[1]["type"], "rss")
        self.assertEqual(defaults[1]["url"], "https://anchor.fm/s/10a0d0948/podcast/rss")
        self.assertEqual(defaults[2]["name"], "Hora Boa Podcast")
        self.assertEqual(defaults[2]["type"], "rss")
        self.assertEqual(defaults[2]["url"], "https://anchor.fm/s/1068cff58/podcast/rss")
        self.assertEqual(defaults[3]["name"], "Desce a Letra Show")
        self.assertEqual(defaults[3]["type"], "youtube")
        self.assertEqual(defaults[3]["url"], "https://www.youtube.com/@DESCEALETRASHOW/streams")
        self.assertEqual(defaults[3]["channel_id"], "UCCD93px0UneWNFg_HxV4naQ")
        self.assertEqual(defaults[3]["tab"], "streams")

    def test_01b_rss_parser_megaphone_duration_and_anchor_unwrap(self):
        sample_xml = """<?xml version="1.0" encoding="UTF-8"?>
        <rss version="2.0" xmlns:itunes="http://www.itunes.com/dtds/podcast-1.0.dtd">
          <channel>
            <item>
              <title><![CDATA[NerdCast 1052 - RPG Western 3]]></title>
              <itunes:duration>13825</itunes:duration>
              <enclosure url="https://traffic.megaphone.fm/JNPD8331850819.mp3?updated=1776479726" length="0" type="audio/mpeg"/>
            </item>
            <item>
              <title><![CDATA[O erro com as bets]]></title>
              <itunes:duration>01:08:15</itunes:duration>
              <enclosure url="https://anchor.fm/s/1068cff58/podcast/play/118207144/https%3A%2F%2Fd3ctxlq1ktw2nl.cloudfront.net%2Fstaging%2Fep.mp3" length="65581349" type="audio/mpeg"/>
            </item>
          </channel>
        </rss>"""
        parsed = self.run_js(
            f"console.log(JSON.stringify(parsePodcastRssXml({json.dumps(sample_xml)}, 'NerdCast')));"
        )
        self.assertEqual(len(parsed), 2)
        self.assertEqual(parsed[0]["duration"], 13825)
        self.assertGreater(parsed[0]["size_bytes"], 100 * 1024 * 1024)
        self.assertEqual(
            parsed[1]["direct_url"],
            "https://d3ctxlq1ktw2nl.cloudfront.net/staging/ep.mp3",
        )
        self.assertEqual(parsed[1]["duration"], 4095)

    def test_02_subscription_target_and_tab_detection(self):
        urls = [
            ("https://www.youtube.com/@DESCEALETRASHOW/streams", "streams", "EgdzdHJlYW1z8gYECgJ6AA=="),
            ("https://www.youtube.com/@FlowPodcast/podcasts", "podcasts", "Eghwb2RjYXN0c_IGBQoDugEA"),
            ("https://www.youtube.com/channel/UCCD93px0UneWNFg_HxV4naQ", "videos", "EgZ2aWRlb3PyBgQKAjoA"),
        ]
        for url, expected_tab, expected_first_param in urls:
            py_res = yt_scraper_core.parse_subscription_target(url)
            js_res = self.run_js(
                f"console.log(JSON.stringify(MPS3_YT_EXTRACTOR.parseSubscriptionTarget({json.dumps(url)})));"
            )
            self.assertEqual(py_res["tab"], expected_tab)
            self.assertEqual(js_res["tab"], expected_tab)
            self.assertEqual(py_res["tab_order"][0], expected_first_param)
            self.assertEqual(js_res["tabParamsOrder"][0], expected_first_param)

    def test_03_offline_fixture_filters_members_and_live_preserving_newest_order(self):
        # 1. Valida em Python
        py_videos, py_skipped = yt_scraper_core.extract_videos_from_browse_tree(
            DLSHOW_STREAMS_FIXTURE, return_skipped=True
        )
        self.assertEqual(
            [v["videoId"] for v in py_videos],
            ["XYFMsxvEtmY", "Dz56Nq-6AuE", "MiZu0sOxrC4"],
        )
        skipped_ids = {s["videoId"]: s["reason"] for s in py_skipped}
        self.assertEqual(skipped_ids.get("JBFomEmr5Os"), "members_only")
        self.assertEqual(skipped_ids.get("LIVE_NOW_11"), "live_now")

        # 2. Valida o mesmo fixture no motor JS (09a_yt_extractor.js)
        js_videos = self.run_js(
            f"""
            const tree = {json.dumps(DLSHOW_STREAMS_FIXTURE)};
            const out = MPS3_YT_EXTRACTOR.extractVideosFromBrowseTree(tree);
            console.log(JSON.stringify(out));
            """
        )
        self.assertEqual(
            [v["videoId"] for v in js_videos],
            ["XYFMsxvEtmY", "Dz56Nq-6AuE", "MiZu0sOxrC4"],
        )

    def test_03b_select_best_audio_format_filters_auto_dubbed_track(self):
        """
        Garante que selectBestAudioFormat rejeita faixas de dublagem automática IA (en-US / dubbed-auto)
        que vêm antes da faixa original (pt-BR) na lista adaptiveFormats do YouTube, prevenindo HTTP 403
        no googlevideo.com e evitando baixar áudio dublado em inglês.
        """
        mock_player = {
            "streamingData": {
                "adaptiveFormats": [
                    {
                        "itag": 139,
                        "mimeType": 'audio/mp4; codecs="mp4a.40.5"',
                        "contentLength": "34217680",
                        "audioTrack": {
                            "displayName": "Inglês (US)",
                            "id": "en-US.10",
                            "audioIsDefault": False,
                            "isAutoDubbed": True,
                        },
                        "url": "https://rr1.googlevideo.com/videoplayback?itag=139&xtags=acont%3Ddubbed-auto%3Alang%3Den-US",
                    },
                    {
                        "itag": 139,
                        "mimeType": 'audio/mp4; codecs="mp4a.40.5"',
                        "contentLength": "34217259",
                        "audioTrack": {
                            "displayName": "Português (BR) original",
                            "id": "pt-BR.4",
                            "audioIsDefault": True,
                        },
                        "url": "https://rr1.googlevideo.com/videoplayback?itag=139&xtags=acont%3Doriginal%3Alang%3Dpt-BR",
                    },
                    {
                        "itag": 140,
                        "mimeType": 'audio/mp4; codecs="mp4a.40.2"',
                        "contentLength": "90813183",
                        "audioTrack": {
                            "displayName": "Inglês (US)",
                            "id": "en-US.10",
                            "audioIsDefault": False,
                            "isAutoDubbed": True,
                        },
                        "url": "https://rr1.googlevideo.com/videoplayback?itag=140&xtags=acont%3Ddubbed-auto%3Alang%3Den-US",
                    },
                    {
                        "itag": 140,
                        "mimeType": 'audio/mp4; codecs="mp4a.40.2"',
                        "contentLength": "90811861",
                        "audioTrack": {
                            "displayName": "Português (BR) original",
                            "id": "pt-BR.4",
                            "audioIsDefault": True,
                        },
                        "url": "https://rr1.googlevideo.com/videoplayback?itag=140&xtags=acont%3Doriginal%3Alang%3Dpt-BR",
                    },
                ]
            }
        }
        chosen = self.run_js(
            f"""
            const p = {json.dumps(mock_player)};
            console.log(JSON.stringify(MPS3_YT_EXTRACTOR.selectBestAudioFormat(p)));
            """
        )
        self.assertEqual(chosen["itag"], 139)
        self.assertEqual(chosen["contentLength"], 34217259)
        self.assertIn("acont%3Doriginal", chosen["url"])
        self.assertNotIn("dubbed-auto", chosen["url"])

    def test_04_live_youtube_scrape_desce_a_letra_show_streams_without_mps3(self):
        """
        Teste Ao Vivo (sem MPS3): Conecta no YouTube real, raspa https://www.youtube.com/@DESCEALETRASHOW/streams,
        confirma que vídeos 'CLUBINHO DOS MEMBROS' foram filtrados, que os mais recentes vêm em ordem
        decrescente de timestamp e que o stream M4A (itag=140) do episódio #1 mais recente baixa bytes reais.
        """
        feed = yt_scraper_core.yt_fetch_channel_feed(
            "https://www.youtube.com/@DESCEALETRASHOW/streams", tab="streams", max_items=5
        )
        self.assertEqual(feed["id"], "UCCD93px0UneWNFg_HxV4naQ")
        self.assertEqual(feed["tab"], "streams")
        self.assertEqual(feed["tabParamUsed"], "EgdzdHJlYW1z8gYECgJ6AA==")
        self.assertGreaterEqual(len(feed["relatedStreams"]), 3)

        # Garante que nenhum episódio de membros vazou para relatedStreams
        for ep in feed["relatedStreams"]:
            self.assertNotIn("CLUBINHO DOS MEMBROS", ep["title"].upper())

        # Garante que a ordenação é estritamente do mais recente para o mais antigo
        timestamps = [ep["uploaded"] for ep in feed["relatedStreams"]]
        self.assertEqual(timestamps, sorted(timestamps, reverse=True))

        # Testa extração e download multi-fragmento (&range=start-end) cruzando a barreira de 1 MB
        # (garante que o perfil VISIONOS 1.02 + &range= não sofre HTTP 403 Forbidden após 1 MB!)
        newest_ep = feed["relatedStreams"][0]
        mime, total_b, meta, frag_gen = yt_scraper_core.yt_stream_audio_fragments(
            newest_ep["videoId"],
            max_bytes=2560 * 1024,      # 2.5 MB totais
            chunk_size=1280 * 1024,     # 2 fragmentos de 1.25 MB (> 1 MB!)
        )
        self.assertIn("audio/mp4", mime)
        self.assertEqual(meta["clientUsed"], "VISIONOS")
        self.assertIn(meta["itag"], (139, 140))

        chunk = b"".join(frag_gen)
        self.assertEqual(len(chunk), 2560 * 1024)
        self.assertIn(b"ftyp", chunk[:32])


if __name__ == "__main__":
    unittest.main(verbosity=2)
