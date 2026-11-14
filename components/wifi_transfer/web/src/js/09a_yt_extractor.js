/* ==========================================================================
 * MÓDULO JS 09A: Extrator Modular YouTube & RSS (Arquitetura estilo yt-dlp)
 * --------------------------------------------------------------------------
 * Isola todas as regras, versões de cliente InnerTube, parâmetros de abas
 * (/streams, /videos, /podcasts) e seletores de schema JSON (lockupViewModel
 * e videoRenderer) do YouTube em um único módulo facilmente atualizável.
 *
 * Quando o YouTube alterar versões de cliente ou estrutura JSON:
 *   1. Atualize `MPS3_YT_EXTRACTOR.CONFIG` ou `extractVideosFromBrowseTree()`
 *   2. Rode `python -m unittest tests/test_yt_extractor.py -v` (sem precisar do MPS3)
 *   3. Rode `python tools/pack_web.py`
 * ========================================================================== */

        const MPS3_YT_EXTRACTOR = {
            VERSION: '2026.10.3',
            FRAGMENT_CHUNK_SIZE: 10 * 1024 * 1024, // 10 MB (idêntico ao CHUNK_SIZE = 10 << 20 do yt-dlp)
            COMPACT_THRESHOLD_BYTES: 50 * 1024 * 1024, // Acima de 50 MB (lives > 45m), prefere itag=139 (~43 MB)

            // Tabela declarativa de parâmetros e perfis InnerTube (estilo yt-dlp 2026)
            CONFIG: {
                browseClient: {
                    clientName: 'WEB',
                    clientVersion: '2.20250101.00.00',
                    hl: 'pt',
                    gl: 'BR'
                },
                // Ordem espelhada do yt-dlp (_DEFAULT_JSLESS_CLIENTS = ('visionos',)):
                // VISIONOS não exige JS player nem GVS PoToken para downloads completos
                playerClients: [
                    {
                        name: 'VISIONOS',
                        clientId: '101',
                        clientName: 'VISIONOS',
                        clientVersion: '1.02',
                        deviceMake: 'Apple',
                        deviceModel: 'RealityDevice17,1',
                        osName: 'visionOS',
                        osVersion: '26.5.23O471',
                        hl: 'pt',
                        gl: 'BR',
                        userAgent: 'Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15'
                    },
                    {
                        name: 'ANDROID',
                        clientId: '3',
                        clientName: 'ANDROID',
                        clientVersion: '20.03.35',
                        androidSdkVersion: 34,
                        osName: 'Android',
                        osVersion: '14',
                        hl: 'pt',
                        gl: 'BR',
                        userAgent: 'com.google.android.youtube/20.03.35 (Linux; U; Android 14) gzip'
                    },
                    {
                        name: 'IOS',
                        clientId: '5',
                        clientName: 'IOS',
                        clientVersion: '20.03.02',
                        deviceMake: 'Apple',
                        deviceModel: 'iPhone16,2',
                        osName: 'iPhone',
                        osVersion: '18.2.1.22C161',
                        hl: 'pt',
                        gl: 'BR',
                        userAgent: 'com.google.ios.youtube/20.03.02 (iPhone16,2; U; CPU iOS 18_2_1 like Mac OS X; pt_BR)'
                    }
                ],
                // Protobuf base64 params das abas de canal do YouTube
                tabParams: {
                    streams: 'EgdzdHJlYW1z8gYECgJ6AA==',   // Aba "Ao vivo" (/streams)
                    videos: 'EgZ2aWRlb3PyBgQKAjoA',        // Aba "Vídeos" (/videos)
                    podcasts: 'Eghwb2RjYXN0c_IGBQoDugEA'   // Aba "Podcasts" (/podcasts)
                },
                // Badges que indicam vídeos restritos a membros pagantes ou lives ainda não encerradas
                excludedBadgeStyles: [
                    'BADGE_MEMBERS_ONLY',
                    'BADGE_STYLE_TYPE_MEMBERS_ONLY',
                    'BADGE_STYLE_TYPE_LIVE_NOW',
                    'THUMBNAIL_OVERLAY_TIME_STATUS_RENDERER_STYLE_LIVE',
                    'THUMBNAIL_OVERLAY_TIME_STATUS_RENDERER_STYLE_UPCOMING'
                ],
                excludedBadgePatterns: [
                    /membros/i,
                    /members\s*only/i,
                    /^ao\s*vivo$/i,
                    /^live$/i,
                    /em\s*breve/i,
                    /upcoming/i,
                    /agendad[oa]/i
                ]
            },

            // Converte "2:03:45" ou "57:15" em segundos (ex: 7425s)
            parseDurationText(text) {
                const clean = String(text || '').trim();
                if (!/^\d+(?::\d{1,2}){1,2}$/.test(clean)) return 0;
                const parts = clean.split(':').map(n => parseInt(n, 10));
                if (parts.some(isNaN)) return 0;
                if (parts.length === 3) return parts[0] * 3600 + parts[1] * 60 + parts[2];
                if (parts.length === 2) return parts[0] * 60 + parts[1];
                return 0;
            },

            // Identifica a aba preferencial (/streams, /podcasts ou /videos) e referência do canal a partir da URL ou objeto
            parseSubscriptionTarget(sub) {
                const isStr = typeof sub === 'string';
                const rawUrl = isStr ? sub.trim() : String((sub && sub.url) || '').trim();
                let preferredTab = (!isStr && sub && sub.tab) ? String(sub.tab).toLowerCase() : '';
                if (!preferredTab) {
                    if (/\/(?:streams|live)(?:[/?#]|$)/i.test(rawUrl)) preferredTab = 'streams';
                    else if (/\/podcasts(?:[/?#]|$)/i.test(rawUrl)) preferredTab = 'podcasts';
                    else preferredTab = 'videos';
                }

                const tabOrder = [preferredTab];
                for (const fallbackTab of ['videos', 'streams', 'podcasts']) {
                    if (!tabOrder.includes(fallbackTab)) tabOrder.push(fallbackTab);
                }

                let channelRef = (!isStr && sub && sub.channel_id) ? String(sub.channel_id).trim() : '';
                if (!channelRef && rawUrl) {
                    const ucMatch = rawUrl.match(/channel\/(UC[A-Za-z0-9_-]{20,})/);
                    const handleMatch = rawUrl.match(/(@[A-Za-z0-9_.-]+)/);
                    if (ucMatch) channelRef = ucMatch[1];
                    else if (handleMatch) channelRef = handleMatch[1];
                    else if (/^UC[A-Za-z0-9_-]{20,}$/.test(rawUrl)) channelRef = rawUrl;
                }

                const tabParamsOrder = tabOrder.map(t => this.CONFIG.tabParams[t]).filter(Boolean);
                return {
                    channelRef,
                    tab: preferredTab,
                    preferredTab,
                    tabOrder,
                    tabParamsOrder
                };
            },

            // Verifica se um item do YouTube é restrito a membros, live ativa ou estreia não concluída
            isExcludedVideoNode(badgesList, metadataTexts, rawNode) {
                if (rawNode && (rawNode.upcomingEventData || rawNode.isLive)) return true;
                const rawJson = JSON.stringify(rawNode || {});
                if (
                    rawJson.includes('BADGE_MEMBERS_ONLY') ||
                    rawJson.includes('SPONSORSHIP_STAR') ||
                    rawJson.includes('Só para membros') ||
                    rawJson.includes('Members only') ||
                    rawJson.includes('BADGE_STYLE_TYPE_MEMBERS_ONLY') ||
                    rawJson.includes('BADGE_STYLE_TYPE_LIVE_NOW') ||
                    rawJson.includes('"AO VIVO"') ||
                    rawJson.includes('upcomingEventData') ||
                    rawJson.includes('BADGE_UPCOMING') ||
                    rawJson.includes('Em breve') ||
                    rawJson.includes('Upcoming')
                ) {
                    return true;
                }

                for (const b of badgesList) {
                    const style = String(b.style || '');
                    const icon = String(b.icon || '');
                    const txt = String(b.text || '').trim();
                    if (this.CONFIG.excludedBadgeStyles.includes(style)) return true;
                    if (icon === 'SPONSORSHIP_STAR') return true;
                    for (const pat of this.CONFIG.excludedBadgePatterns) {
                        if (pat.test(txt)) return true;
                    }
                }

                for (const mtxt of metadataTexts) {
                    if (/membros|members\s*only|em\s*breve|agendad|estreia\s*em/i.test(String(mtxt))) {
                        return true;
                    }
                }
                return false;
            },

            // Percorre a árvore JSON do InnerTube (/youtubei/v1/browse ou ytInitialData)
            // extraindo vídeos públicos concluídos na ordem exata de mais recente -> mais antigo
            extractVideosFromBrowseTree(rootData, programName = '', maxItems = 10) {
                const found = [];
                const seenIds = new Set();
                const self = this;

                function inspectLockupViewModel(lvm) {
                    const vid = String(lvm.contentId || '').trim();
                    if (!vid || vid.length !== 11 || seenIds.has(vid)) return;

                    const metaRoot = (lvm.metadata && lvm.metadata.lockupMetadataViewModel) || {};
                    const title = (metaRoot.title && metaRoot.title.content) ? String(metaRoot.title.content).trim() : '';
                    if (!title) return;

                    const badges = [];
                    const metaTexts = [];

                    // 1. Inspeciona metadataRows (ex: "136 mil visualizações", "Transmitido há 11 h", badge "Só para membros")
                    const rows = (((metaRoot.metadata || {}).contentMetadataViewModel || {}).metadataRows) || [];
                    for (const r of rows) {
                        for (const part of (r.metadataParts || [])) {
                            const txt = part.text && part.text.content;
                            if (txt) metaTexts.push(txt);
                        }
                        for (const b of (r.badges || [])) {
                            const bvm = b.badgeViewModel || {};
                            badges.push({
                                style: bvm.badgeStyle || '',
                                text: bvm.badgeText || '',
                                icon: bvm.iconName || ''
                            });
                        }
                    }

                    // 2. Inspeciona overlays da thumbnail (duração ex: "2:03:45" ou badge "AO VIVO")
                    let durationSec = 0;
                    const overlays = (((lvm.contentImage || {}).thumbnailViewModel || {}).overlays) || [];
                    for (const ov of overlays) {
                        const tb = ov.thumbnailBottomOverlayViewModel;
                        if (tb && Array.isArray(tb.badges)) {
                            for (const b of tb.badges) {
                                const tbvm = b.thumbnailBadgeViewModel || {};
                                const btxt = String(tbvm.text || '').trim();
                                badges.push({
                                    style: tbvm.badgeStyle || '',
                                    text: btxt,
                                    icon: ''
                                });
                                const dSec = self.parseDurationText(btxt);
                                if (dSec > 0) durationSec = dSec;
                            }
                        }
                    }

                    if (self.isExcludedVideoNode(badges, metaTexts, lvm)) return;

                    seenIds.add(vid);
                    found.push({
                        videoId: vid,
                        title,
                        duration: durationSec || 900,
                        metaSummary: metaTexts.join(' • ')
                    });
                }

                function inspectVideoRenderer(vr) {
                    const vid = String(vr.videoId || '').trim();
                    if (!vid || vid.length !== 11 || seenIds.has(vid)) return;

                    const runs = (vr.title && vr.title.runs) || [];
                    const title = runs.length ? String(runs[0].text || '').trim() : String((vr.title && vr.title.simpleText) || '').trim();
                    if (!title) return;

                    const badges = [];
                    for (const b of (vr.badges || [])) {
                        const mb = b.metadataBadgeRenderer || {};
                        badges.push({
                            style: mb.style || '',
                            text: mb.label || '',
                            icon: (mb.icon && mb.icon.iconType) || ''
                        });
                    }
                    for (const ov of (vr.thumbnailOverlays || [])) {
                        const ts = ov.thumbnailOverlayTimeStatusRenderer;
                        if (ts) {
                            badges.push({
                                style: ts.style || '',
                                text: (ts.text && ts.text.simpleText) || '',
                                icon: ''
                            });
                        }
                    }

                    const pubText = (vr.publishedTimeText && vr.publishedTimeText.simpleText) || '';
                    const lenText = (vr.lengthText && vr.lengthText.simpleText) || '';
                    const durationSec = self.parseDurationText(lenText);

                    if (self.isExcludedVideoNode(badges, [pubText], vr)) return;
                    if (vr.thumbnailOverlays && durationSec === 0 && !lenText) return;

                    seenIds.add(vid);
                    found.push({
                        videoId: vid,
                        title,
                        duration: durationSec || 900,
                        metaSummary: pubText
                    });
                }

                function walk(node) {
                    if (!node || typeof node !== 'object' || found.length >= maxItems) return;
                    if (Array.isArray(node)) {
                        for (const item of node) {
                            if (found.length >= maxItems) break;
                            walk(item);
                        }
                        return;
                    }
                    if (node.lockupViewModel) {
                        inspectLockupViewModel(node.lockupViewModel);
                    } else if (node.videoRenderer) {
                        inspectVideoRenderer(node.videoRenderer);
                    }
                    for (const k of Object.keys(node)) {
                        if (found.length >= maxItems) break;
                        walk(node[k]);
                    }
                }

                walk(rootData);

                // Garante ordenação estrita dos mais recentes no topo (idx 0 = maior mtime)
                const baseSec = Math.floor(Date.now() / 1000);
                return found.slice(0, maxItems).map((item, idx) => {
                    const mtime = baseSec - (idx * 60);
                    const estBytes = Math.max(512 * 1024, item.duration * 16000);
                    const epObj = {
                        id: item.videoId,
                        videoId: item.videoId,
                        program: programName,
                        title: item.title,
                        mtime,
                        duration: item.duration,
                        source_type: 'youtube',
                        video_id: item.videoId,
                        ext: 'm4a',
                        size_bytes: estBytes
                    };
                    if (typeof buildPodEpisodeFilename === 'function') {
                        epObj.filename = buildPodEpisodeFilename(epObj, 'm4a');
                    }
                    return epObj;
                });
            },

            // Seleciona o melhor stream de áudio M4A/Opus de uma resposta /youtubei/v1/player
            selectBestAudioFormat(playerData, preferCompact = false) {
                const formats = Array.isArray(playerData)
                    ? playerData
                    : ((playerData && playerData.streamingData && playerData.streamingData.adaptiveFormats) || []);
                const allAudio = formats.filter(f => f && String(f.mimeType || '').startsWith('audio/') && f.url);
                if (!allAudio.length) return null;

                const nonDrc = allAudio.filter(f => !f.isDrc);
                const baseAudio = nonDrc.length ? nonDrc : allAudio;

                // Prioriza a faixa de áudio original (pt-BR / audioIsDefault) em vez de dublagem automática IA (dubbed-auto)
                const origTracks = baseAudio.filter(f => {
                    if (f.audioTrack) {
                        if (f.audioTrack.isAutoDubbed) return false;
                        if (f.audioTrack.audioIsDefault) return true;
                    }
                    const u = String(f.url || '');
                    if (u.includes('dubbed-auto')) return false;
                    if (u.includes('acont%3Doriginal') || u.includes('acont=original')) return true;
                    return !u.includes('acont%3Ddubbed') && !u.includes('acont=dubbed');
                });
                const audioFormats = origTracks.length ? origTracks : baseAudio;

                const itag140 = audioFormats.find(f => f.itag === 140);
                const itag139 = audioFormats.find(f => f.itag === 139);
                let chosen = null;

                if (itag140 && itag139) {
                    const clen140 = parseInt(itag140.contentLength || '0', 10) || 0;
                    chosen = (preferCompact || clen140 > this.COMPACT_THRESHOLD_BYTES) ? itag139 : itag140;
                } else {
                    chosen = itag140 || itag139 || audioFormats.find(f => String(f.mimeType).includes('mp4')) || audioFormats[0];
                }

                const isMp4 = String(chosen.mimeType).includes('mp4');
                return {
                    url: chosen.url,
                    ext: isMp4 ? 'm4a' : 'webm',
                    mime: chosen.mimeType || (isMp4 ? 'audio/mp4' : 'audio/webm'),
                    itag: chosen.itag || 0,
                    contentLength: parseInt(chosen.contentLength || '0', 10) || 0
                };
            },

            // Constrói lista de URLs fragmentadas com &range=start-end (evita HTTP 403 e throttling do googlevideo.com)
            buildFragmentUrls(baseUrl, contentLength, chunkSize = this.FRAGMENT_CHUNK_SIZE) {
                const sep = String(baseUrl).includes('?') ? '&' : '?';
                if (!contentLength || contentLength <= 0) {
                    return [`${baseUrl}${sep}range=0-${chunkSize - 1}`];
                }
                const urls = [];
                for (let start = 0; start < contentLength; start += chunkSize) {
                    const end = Math.min(contentLength - 1, start + chunkSize - 1);
                    urls.push(`${baseUrl}${sep}range=${start}-${end}`);
                }
                return urls;
            }
        };
