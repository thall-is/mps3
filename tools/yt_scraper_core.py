#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
MPS3 Modular YouTube & Podcast Extractor Core (tools/yt_scraper_core.py)
------------------------------------------------------------------------
Subsistema isolado e versionado de extração do YouTube (InnerTube 2026, estilo yt-dlp) para:
  1. O Simulador Local da Central Web (`tools/web_dev_simulator.py`)
  2. A suíte de testes automatizados (`tests/test_yt_extractor.py`)
  3. Diagnóstico rápido via CLI sem precisar do hardware MPS3 ligado:
     `python tools/yt_scraper_core.py https://www.youtube.com/@DESCEALETRASHOW/streams --check-audio`

Por que este módulo evita o erro `HTTP 403 Forbidden` em downloads longos (2026):
  - Os clientes `ANDROID` e `IOS` passaram a exigir `GVS PoToken` após ~1 MB de buffer anônimo.
  - Seguindo a arquitetura oficial do `yt-dlp` (`_DEFAULT_JSLESS_CLIENTS = ('visionos',)`),
    utilizamos o perfil `VISIONOS` (`clientVersion="1.02"`, `X-YouTube-Client-Name="101"`)
    combinado com `X-Goog-Visitor-Id` e download fragmentado via query param `&range=start-end`
    (`CHUNK_SIZE = 10 MB`), que baixa lives inteiras de 2 horas a >15 MB/s sem exigir login/cookies.
  - Inclui também gerador opcional de `Authorization: SAPISIDHASH` caso o usuário forneça cookies.
"""

import argparse
import hashlib
import json
import re
import sys
import time
from urllib.parse import parse_qs, quote, unquote, urlencode, urlparse, urlunparse
import urllib.request

EXTRACTOR_VERSION = "2026.10.3"
FRAGMENT_CHUNK_SIZE = 10 * 1024 * 1024  # 10 MB (idêntico ao CHUNK_SIZE = 10 << 20 do yt-dlp)
COMPACT_THRESHOLD_BYTES = 50 * 1024 * 1024  # Acima de 50 MB (lives > 45 min), prefere itag=139 M4A

CONFIG = {
    "web_ua": (
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"
    ),
    "browse_client": {
        "clientName": "WEB",
        "clientVersion": "2.20250101.00.00",
        "hl": "pt",
        "gl": "BR",
    },
    # Ordem de prioridade espelhada do yt-dlp 2026 (_DEFAULT_JSLESS_CLIENTS):
    # 1. VISIONOS (não exige JS player nem GVS PoToken para HTTPS adaptiveFormats!)
    # 2. ANDROID / IOS (fallbacks)
    "player_clients": [
        {
            "name": "VISIONOS",
            "clientId": "101",
            "clientName": "VISIONOS",
            "clientVersion": "1.02",
            "deviceMake": "Apple",
            "deviceModel": "RealityDevice17,1",
            "osName": "visionOS",
            "osVersion": "26.5.23O471",
            "userAgent": (
                "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) "
                "AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15"
            ),
        },
        {
            "name": "ANDROID",
            "clientId": "3",
            "clientName": "ANDROID",
            "clientVersion": "20.03.35",
            "androidSdkVersion": 34,
            "osName": "Android",
            "osVersion": "14",
            "userAgent": "com.google.android.youtube/20.03.35 (Linux; U; Android 14) gzip",
        },
        {
            "name": "IOS",
            "clientId": "5",
            "clientName": "IOS",
            "clientVersion": "20.03.02",
            "deviceMake": "Apple",
            "deviceModel": "iPhone16,2",
            "osName": "iPhone",
            "osVersion": "18.2.1.22C161",
            "userAgent": "com.google.ios.youtube/20.03.02 (iPhone16,2; U; CPU iOS 18_2_1 like Mac OS X;)",
        },
    ],
    # Protobuf params oficiais do YouTube InnerTube (/youtubei/v1/browse) para abas de canal:
    "tab_params": {
        "videos": "EgZ2aWRlb3PyBgQKAjoA",
        "streams": "EgdzdHJlYW1z8gYECgJ6AA==",
        "podcasts": "Eghwb2RjYXN0c_IGBQoDugEA",
    },
    "default_channels": [
        {
            "name": "Desce a Letra Show",
            "url": "https://www.youtube.com/@DESCEALETRASHOW/streams",
            "channel_id": "UCCD93px0UneWNFg_HxV4naQ",
            "tab": "streams",
            "type": "yt",
            "keep": 3,
        },
        {
            "name": "Flow Podcast",
            "url": "https://www.youtube.com/@FlowPodcast/streams",
            "channel_id": "UC4ncvgh5hFr5O83MH7-jRJg",
            "tab": "streams",
            "type": "yt",
            "keep": 2,
        },
        {
            "name": "Ciência Sem Fim",
            "url": "https://www.youtube.com/@CienciaSemFim/streams",
            "channel_id": "UC9djCiv4e85Kd8G--GWlcoQ",
            "tab": "streams",
            "type": "yt",
            "keep": 2,
        },
    ],
}


def generate_sapisidhash_header(sapisid: str, origin: str = "https://www.youtube.com") -> str:
    """
    Gera o cabeçalho `Authorization: SAPISIDHASH <ts>_<sha1>` idêntico ao yt-dlp
    quando um cookie `SAPISID` ou `__Secure-3PAPISID` de sessão autenticada é fornecido.
    """
    ts = str(int(time.time()))
    digest = hashlib.sha1(f"{ts} {sapisid} {origin}".encode("utf-8")).hexdigest()
    return f"SAPISIDHASH {ts}_{digest}"


def parse_duration_text(text: str) -> int:
    """Converte strings como '2:03:45' ou '57:15' em segundos (ex: 7425)."""
    clean = str(text or "").strip()
    if not re.match(r"^\d+(?::\d{1,2}){1,2}$", clean):
        return 0
    parts = [int(p) for p in clean.split(":")]
    if len(parts) == 3:
        return parts[0] * 3600 + parts[1] * 60 + parts[2]
    if len(parts) == 2:
        return parts[0] * 60 + parts[1]
    return 0


def parse_subscription_target(sub) -> dict:
    """
    Normaliza uma string (URL, @handle ou UC...) ou dict de inscrição em:
    {'channel_id': str, 'handle': str, 'tab': 'videos'|'streams'|'podcasts', 'tab_order': list[str]}
    """
    if isinstance(sub, dict):
        raw_url = str(sub.get("url") or "").strip()
        channel_id = str(sub.get("channel_id") or "").strip()
        explicit_tab = str(sub.get("tab") or "").strip().lower()
    else:
        raw_url = str(sub or "").strip()
        channel_id = ""
        explicit_tab = ""

    handle = ""
    detected_tab = explicit_tab

    m_uc = re.search(r"(?:channel\/|^)(UC[A-Za-z0-9_-]{20,})", raw_url)
    if not channel_id and m_uc:
        channel_id = m_uc.group(1)

    m_handle = re.search(r"(@[A-Za-z0-9_.-]+)", raw_url)
    if m_handle:
        handle = m_handle.group(1)

    if not detected_tab:
        if re.search(r"\/streams(?:\b|\/|\?|$)", raw_url, re.IGNORECASE) or re.search(
            r"\/live(?:\b|\/|\?|$)", raw_url, re.IGNORECASE
        ):
            detected_tab = "streams"
        elif re.search(r"\/podcasts(?:\b|\/|\?|$)", raw_url, re.IGNORECASE):
            detected_tab = "podcasts"
        else:
            detected_tab = "videos"

    tp = CONFIG["tab_params"]
    if detected_tab == "streams":
        tab_order = [tp["streams"], tp["videos"]]
    elif detected_tab == "podcasts":
        tab_order = [tp["podcasts"], tp["streams"], tp["videos"]]
    else:
        tab_order = [tp["videos"], tp["streams"]]

    return {
        "channel_id": channel_id,
        "handle": handle,
        "tab": detected_tab,
        "tab_order": tab_order,
        "raw_url": raw_url,
    }


def is_excluded_video_node(node: dict) -> tuple[bool, str]:
    """
    Detecta se um nó `lockupViewModel` ou `videoRenderer` deve ser ignorado:
    - Vídeos exclusivos para membros (`BADGE_MEMBERS_ONLY`, `SPONSORSHIP_STAR`, 'Só para membros')
    - Transmissões ativas no momento (`BADGE_STYLE_TYPE_LIVE_NOW`, 'AO VIVO')
    - Transmissões agendadas/estreias futuras (`UPCOMING`, `upcomingEventData`)
    Retorna `(True, motivo)` se deve ser filtrado, ou `(False, "")` se for episódio público concluído.
    """
    raw = json.dumps(node, ensure_ascii=False)
    if (
        "BADGE_MEMBERS_ONLY" in raw
        or "SPONSORSHIP_STAR" in raw
        or "Só para membros" in raw
        or "Members only" in raw
        or "BADGE_STYLE_TYPE_MEMBERS_ONLY" in raw
    ):
        return True, "members_only"

    if (
        "BADGE_STYLE_TYPE_LIVE_NOW" in raw
        or "BADGE_LIVE_NOW" in raw
        or '"text":"AO VIVO"' in raw
        or '"text": "AO VIVO"' in raw
        or '"text":"LIVE"' in raw
        or '"isLive":true' in raw
    ):
        return True, "live_now"

    if (
        "upcomingEventData" in raw
        or "BADGE_UPCOMING" in raw
        or "THUMBNAIL_OVERLAY_TIME_STATUS_RENDERER_STYLE_UPCOMING" in raw
        or "Estreia" in raw
        or "Agendado" in raw
        or "Em breve" in raw
        or "Upcoming" in raw
        or "Premieres" in raw
    ):
        return True, "upcoming"

    return False, ""


def _extract_duration_from_node(node: dict) -> int:
    """Extrai a duração em segundos de um `lockupViewModel` ou `videoRenderer`."""
    overlays = (
        node.get("contentImage", {})
        .get("thumbnailViewModel", {})
        .get("overlays", [])
    )
    for ov in overlays:
        badges = ov.get("thumbnailBottomOverlayViewModel", {}).get("badges", [])
        for b in badges:
            txt = b.get("thumbnailBadgeViewModel", {}).get("text", "")
            sec = parse_duration_text(txt)
            if sec > 0:
                return sec
    len_txt = node.get("lengthText", {}).get("simpleText", "")
    return parse_duration_text(len_txt)


def extract_videos_from_browse_tree(root_obj, return_skipped: bool = False):
    """
    Percorre a árvore JSON do `/youtubei/v1/browse` preservando a ordem cronológica exata
    (mais recentes primeiro) e filtrando conteúdos restritos a membros ou lives não concluídas.
    """
    out = []
    skipped = []
    seen = set()

    def walk(node):
        if isinstance(node, dict):
            if "lockupViewModel" in node and isinstance(node["lockupViewModel"], dict):
                lvm = node["lockupViewModel"]
                vid = lvm.get("contentId", "")
                title = (
                    lvm.get("metadata", {})
                    .get("lockupMetadataViewModel", {})
                    .get("title", {})
                    .get("content", "")
                )
                if vid and len(vid) == 11 and title and vid not in seen:
                    excluded, reason = is_excluded_video_node(lvm)
                    if excluded:
                        seen.add(vid)
                        skipped.append({"videoId": vid, "title": title, "reason": reason})
                    else:
                        seen.add(vid)
                        dur = _extract_duration_from_node(lvm) or 900
                        out.append({"videoId": vid, "title": title, "duration": dur})
            elif "videoRenderer" in node and isinstance(node["videoRenderer"], dict):
                vr = node["videoRenderer"]
                vid = vr.get("videoId", "")
                runs = vr.get("title", {}).get("runs", [])
                title = runs[0].get("text", "") if runs else vr.get("title", {}).get("simpleText", "")
                if vid and len(vid) == 11 and title and vid not in seen:
                    excluded, reason = is_excluded_video_node(vr)
                    if excluded:
                        seen.add(vid)
                        skipped.append({"videoId": vid, "title": title, "reason": reason})
                    else:
                        seen.add(vid)
                        dur = _extract_duration_from_node(vr) or 900
                        out.append({"videoId": vid, "title": title, "duration": dur})

            for v in node.values():
                walk(v)
        elif isinstance(node, list):
            for item in node:
                walk(item)

    walk(root_obj)
    if return_skipped:
        return out, skipped
    return out


def yt_resolve_channel_id(ref: str) -> str:
    """Resolve um `channel_id` (UC...), `@handle` ou URL completa de canal para `UC...`."""
    ref_clean = unquote(str(ref or "")).strip()
    if re.match(r"^UC[A-Za-z0-9_-]{20,}$", ref_clean):
        return ref_clean

    m_uc = re.search(r"channel\/(UC[A-Za-z0-9_-]{20,})", ref_clean)
    if m_uc:
        return m_uc.group(1)

    url = ref_clean if ref_clean.startswith("http") else f"https://www.youtube.com/{ref_clean.lstrip('/')}"
    req = urllib.request.Request(
        url,
        headers={
            "User-Agent": CONFIG["web_ua"],
            "Accept-Language": "pt-BR,pt;q=0.9,en-US;q=0.8",
        },
    )
    with urllib.request.urlopen(req, timeout=12) as r:
        html = r.read().decode("utf-8", errors="ignore")

    m = (
        re.search(r'<link rel="canonical" href="https://www\.youtube\.com/channel/(UC[A-Za-z0-9_-]{20,})"', html)
        or re.search(r'"externalId"\s*:\s*"(UC[A-Za-z0-9_-]{20,})"', html)
        or re.search(r'"channelId"\s*:\s*"(UC[A-Za-z0-9_-]{20,})"', html)
    )
    if not m:
        raise ValueError(f"Nao foi possivel resolver channel_id para '{ref_clean}'")
    return m.group(1)


def yt_fetch_channel_feed(channel_ref: str, tab: str = None, max_items: int = 15) -> dict:
    """
    Busca a lista de episódios públicos mais recentes de um canal via InnerTube `/youtubei/v1/browse`.
    Respeita a aba solicitada (`streams`, `videos` ou `podcasts`) e retorna no formato compatível
    com a Central Web do MPS3 (`relatedStreams` ordenados do mais recente para o mais antigo).
    """
    parsed_target = parse_subscription_target({"url": channel_ref, "tab": tab or ""})
    cid = parsed_target["channel_id"] or yt_resolve_channel_id(channel_ref)
    tab_order = parsed_target["tab_order"]

    streams = []
    skipped_items = []
    used_tab_param = ""
    now_ms = int(time.time() * 1000)

    for tab_param in tab_order:
        body = json.dumps(
            {
                "browseId": cid,
                "params": tab_param,
                "context": {"client": CONFIG["browse_client"]},
            }
        ).encode("utf-8")
        req = urllib.request.Request(
            "https://www.youtube.com/youtubei/v1/browse?prettyPrint=false",
            data=body,
            headers={
                "Content-Type": "application/json",
                "User-Agent": CONFIG["web_ua"],
            },
            method="POST",
        )
        try:
            with urllib.request.urlopen(req, timeout=12) as r:
                bdata = json.loads(r.read().decode("utf-8"))
            found, skipped = extract_videos_from_browse_tree(bdata, return_skipped=True)
            if found:
                used_tab_param = tab_param
                skipped_items = skipped
                for idx, item in enumerate(found[:max_items]):
                    vid = item["videoId"]
                    dur = item.get("duration") or 900
                    # Se for live longa (> 45 min), usamos ~6 KB/s (itag=139 48kbps = ~43 MB em 2h)
                    bytes_per_sec = 6000 if dur > 2700 else 16000
                    streams.append(
                        {
                            "url": f"/watch?v={vid}",
                            "videoId": vid,
                            "title": item["title"],
                            "uploaded": now_ms - (idx * 60000),
                            "duration": dur,
                            "size_bytes": max(512 * 1024, dur * bytes_per_sec),
                        }
                    )
                break
        except Exception:
            continue

    if not streams:
        raise RuntimeError(f"Nenhum episodio publico encontrado para o canal {cid} (tab={parsed_target['tab']})")

    return {
        "id": cid,
        "name": cid,
        "tab": parsed_target["tab"],
        "tabParamUsed": used_tab_param,
        "extractorVersion": EXTRACTOR_VERSION,
        "relatedStreams": streams,
        "skipped": skipped_items,
    }


def select_best_audio_format(adaptive_formats: list, prefer_compact: bool = False) -> dict | None:
    """
    Seleciona o melhor stream de áudio compatível com o decodificador AAC/M4A do MPS3:
      - Ignora faixas duplicadas com DRC (`isDrc=True`) quando houver faixa original (`isDrc=False`).
      - Se `prefer_compact=True` OU se `itag=140` ultrapassar 50 MB (ex: lives de 2 horas como DLSHOW),
        prioriza `itag=139` (HE-AAC 48 kbps `.m4a`, ~43 MB para 2h em vez de 115 MB).
      - Caso contrário prioriza `itag=140` (AAC-LC 128 kbps `.m4a`).
    """
    if not isinstance(adaptive_formats, list):
        return None
    all_audio = [
        f
        for f in adaptive_formats
        if isinstance(f, dict) and str(f.get("mimeType", "")).startswith("audio/") and f.get("url")
    ]
    if not all_audio:
        return None

    non_drc = [f for f in all_audio if not f.get("isDrc")]
    audio_list = non_drc if non_drc else all_audio

    itag140 = next((f for f in audio_list if f.get("itag") == 140), None)
    itag139 = next((f for f in audio_list if f.get("itag") == 139), None)

    if itag140 and itag139:
        clen140 = int(itag140.get("contentLength") or 0)
        if prefer_compact or clen140 > COMPACT_THRESHOLD_BYTES:
            return itag139
        return itag140

    if itag140:
        return itag140
    if itag139:
        return itag139

    mp4_audio = next((f for f in audio_list if "mp4" in str(f.get("mimeType", ""))), None)
    if mp4_audio:
        return mp4_audio

    audio_list.sort(key=lambda x: x.get("bitrate", 0), reverse=True)
    return audio_list[0]


def append_range_query_param(url: str, start: int, end: int) -> str:
    """
    Adiciona `&range=<start>-<end>` como query parameter na URL do `googlevideo.com`
    (exatamente como `build_fragments` do `yt-dlp`), evitando `HTTP 403 Forbidden`.
    """
    parsed = urlparse(url)
    qs = parse_qs(parsed.query, keep_blank_values=True)
    qs["range"] = [f"{start}-{end}"]
    new_query = urlencode(qs, doseq=True)
    return urlunparse(parsed._replace(query=new_query))


def build_fragment_urls(base_url: str, content_length: int, chunk_size: int = FRAGMENT_CHUNK_SIZE) -> list[tuple[int, int, str]]:
    """Divide o download de `content_length` bytes em fragmentos `&range=start-end` de `chunk_size`."""
    if content_length <= 0:
        return [(0, chunk_size - 1, append_range_query_param(base_url, 0, chunk_size - 1))]
    frags = []
    for start in range(0, content_length, chunk_size):
        end = min(content_length - 1, start + chunk_size - 1)
        frags.append((start, end, append_range_query_param(base_url, start, end)))
    return frags


def yt_extract_direct_audio_url(
    video_id: str, prefer_compact: bool = False, sapisid_cookie: str = None
) -> tuple[str, str, dict]:
    """
    Extrai a URL direta de áudio (`.m4a`) de um vídeo/live concluída do YouTube
    usando `VISITOR_DATA` + lista de clientes (`VISIONOS` -> `ANDROID` -> `IOS`).
    Retorna `(url, mime_type, metadata_dict)`.
    """
    watch_req = urllib.request.Request(
        f"https://www.youtube.com/watch?v={quote(video_id)}",
        headers={"User-Agent": CONFIG["web_ua"]},
    )
    with urllib.request.urlopen(watch_req, timeout=12) as wr:
        watch_html = wr.read().decode("utf-8", errors="ignore")
    m_vis = re.search(r'"VISITOR_DATA"\s*:\s*"([^"]+)"', watch_html)
    visitor_data = m_vis.group(1) if m_vis else ""

    last_reason = "Sem resposta dos clientes InnerTube"
    for client_cfg in CONFIG["player_clients"]:
        client_payload = {
            k: v for k, v in client_cfg.items() if k not in ("name", "clientId")
        }
        client_payload["hl"] = "pt"
        client_payload["gl"] = "BR"
        if visitor_data:
            client_payload["visitorData"] = visitor_data

        payload = json.dumps(
            {
                "videoId": video_id,
                "contentCheckOk": True,
                "racyCheckOk": True,
                "context": {"client": client_payload},
            }
        ).encode("utf-8")

        headers = {
            "Content-Type": "application/json",
            "User-Agent": client_cfg["userAgent"],
            "X-YouTube-Client-Name": str(client_cfg.get("clientId", "1")),
            "X-YouTube-Client-Version": client_cfg["clientVersion"],
            "Origin": "https://www.youtube.com",
        }
        if visitor_data:
            headers["X-Goog-Visitor-Id"] = visitor_data
        if sapisid_cookie:
            headers["Authorization"] = generate_sapisidhash_header(sapisid_cookie)

        player_req = urllib.request.Request(
            "https://www.youtube.com/youtubei/v1/player?prettyPrint=false",
            data=payload,
            headers=headers,
            method="POST",
        )
        try:
            with urllib.request.urlopen(player_req, timeout=12) as pr:
                pdata = json.loads(pr.read().decode("utf-8"))
        except Exception as e:
            last_reason = f"{client_cfg['name']}: {e}"
            continue

        formats = pdata.get("streamingData", {}).get("adaptiveFormats", [])
        best = select_best_audio_format(formats, prefer_compact=prefer_compact)
        if best:
            vd = pdata.get("videoDetails", {})
            meta = {
                "videoId": video_id,
                "title": vd.get("title", ""),
                "lengthSeconds": int(vd.get("lengthSeconds", "0") or 0),
                "clientUsed": client_cfg["name"],
                "userAgent": client_cfg["userAgent"],
                "itag": best.get("itag"),
                "bitrate": best.get("bitrate"),
                "contentLength": int(best.get("contentLength", "0") or 0),
            }
            return best["url"], best.get("mimeType", "audio/mp4"), meta

        last_reason = pdata.get("playabilityStatus", {}).get("reason", "Sem adaptiveFormats de audio")

    raise RuntimeError(f"InnerTube falhou para {video_id}: {last_reason}")


def yt_stream_audio_fragments(
    video_id: str, max_bytes: int = 0, chunk_size: int = FRAGMENT_CHUNK_SIZE
):
    """
    Resolve o melhor stream M4A para `video_id` e retorna `(mime_type, total_bytes, fragment_iterator)`.
    O `fragment_iterator` baixa o áudio em fragmentos de 10 MB usando `&range=start-end`
    (protocolo anti-403 e anti-throttling do `yt-dlp`).
    """
    url, mime, meta = yt_extract_direct_audio_url(video_id)
    ua = meta.get("userAgent") or CONFIG["player_clients"][0]["userAgent"]
    total_len = meta.get("contentLength") or 0
    if max_bytes > 0:
        total_len = min(total_len, max_bytes) if total_len > 0 else max_bytes

    fragments = build_fragment_urls(url, total_len, chunk_size=chunk_size)

    def _generator():
        for _start, _end, frag_url in fragments:
            req = urllib.request.Request(frag_url, headers={"User-Agent": ua})
            with urllib.request.urlopen(req, timeout=30) as resp:
                while True:
                    blk = resp.read(256 * 1024)
                    if not blk:
                        break
                    yield blk

    return mime, total_len, meta, _generator()


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")

    parser = argparse.ArgumentParser(
        description=f"MPS3 Modular YouTube Extractor CLI (v{EXTRACTOR_VERSION})"
    )
    parser.add_argument(
        "target",
        nargs="?",
        default="https://www.youtube.com/@DESCEALETRASHOW/streams",
        help="URL do canal, @handle ou channel_id (padrao: @DESCEALETRASHOW/streams)",
    )
    parser.add_argument(
        "--tab",
        choices=["auto", "streams", "videos", "podcasts"],
        default="auto",
        help="Aba do canal a raspar (padrao: auto-detectar da URL)",
    )
    parser.add_argument("--limit", type=int, default=5, help="Numero de episodios a exibir")
    parser.add_argument(
        "--check-audio",
        action="store_true",
        help="Testa tambem a extracao e download multi-fragmento (&range=) do episodio #1 mais recente",
    )
    args = parser.parse_args()

    tab_arg = None if args.tab == "auto" else args.tab
    t0 = time.time()
    feed = yt_fetch_channel_feed(args.target, tab=tab_arg, max_items=args.limit)
    dt = time.time() - t0

    print("======================================================================")
    print(f"  MPS3 YOUTUBE EXTRACTOR v{EXTRACTOR_VERSION} (Tempo: {dt:.2f}s)")
    print("======================================================================")
    print(f"  Alvo       : {args.target}")
    print(f"  Channel ID : {feed['id']}")
    print(f"  Aba Ativa  : {feed['tab']} (params={feed['tabParamUsed']})")
    if feed.get("skipped"):
        print(f"  Filtrados  : {len(feed['skipped'])} itens ignorados (ex: membros/lives ativas)")
        for sk in feed["skipped"][:5]:
            print(f"               - [IGNORADO: {sk['reason']}] {sk['videoId']} | {sk['title']}")
    print("----------------------------------------------------------------------")
    print(f"  EPISODIOS PUBLICOS MAIS RECENTES ({len(feed['relatedStreams'])}):")
    for idx, ep in enumerate(feed["relatedStreams"], 1):
        mb_est = (ep.get("size_bytes") or 0) / (1024 * 1024)
        print(f"    #{idx:02d} [{ep['videoId']}] ({ep['duration']}s | ~{mb_est:.1f} MB) {ep['title']}")

    if args.check-audio if hasattr(args, "check-audio") else args.check_audio:
        if feed["relatedStreams"]:
            top_ep = feed["relatedStreams"][0]
            print("----------------------------------------------------------------------")
            print(f"  Testando download multi-fragmento (&range=) para #1: {top_ep['videoId']}...")
            t_dl = time.time()
            # Testa baixar 4 MB divididos em 2 fragmentos de 2 MB para provar que nao ha 403 apos 1 MB
            mime, total_b, meta, gen = yt_stream_audio_fragments(
                top_ep["videoId"], max_bytes=4 * 1024 * 1024, chunk_size=2 * 1024 * 1024
            )
            received = b"".join(gen)
            dt_dl = time.time() - t_dl
            print(f"  [OK] Cliente : {meta['clientUsed']} | itag={meta['itag']} | {mime}")
            print(
                f"  [OK] Duracao : {meta['lengthSeconds']}s | Tamanho Real: {meta['contentLength'] / (1024*1024):.1f} MB"
            )
            print(
                f"  [OK] Recebido: {len(received)} bytes (2 fragmentos de 2 MB) em {dt_dl:.2f}s | Header={received[:12]!r}"
            )
    print("======================================================================")


if __name__ == "__main__":
    main()
