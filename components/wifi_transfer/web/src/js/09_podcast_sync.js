/* ==========================================================================
 * MÓDULO JS 09: Motor de Podcasts v3.0 (Browser YouTube/RSS Scraper + Sync Atômico)
 * --------------------------------------------------------------------------
 * - Hash SHA-256 Pure-JS (compatível com HTTP plano sem window.crypto.subtle)
 * - Persistência de canais em /Podcasts/.subscriptions.json no SD + localStorage
 * - Scraper Híbrido no Navegador:
 *     1. Userscript Bridge (window.__MPS3_YT_BRIDGE__) se instalado
 *     2. Pool Multi-API CORS (Piped / Invidious ?local=true / YouTube Atom XML)
 *     3. Parser RSS 2.0 / Atom nativo para feeds de podcast
 * - BrowserSyncPlanner: Port 1:1 de tools/podcast_sync/podcast_sync/planner.py
 *   (Retenção Top-N por canal, proteção de não-ouvidos, histórico played_ledger)
 * - Protocolo Transacional MPS3: /api/sync/manifest -> /api/sync/begin ->
 *   /Podcasts/.staging + /api/sync/verify (SHA-256) -> /api/move -> /api/sync/done
 * ========================================================================== */

        // --- 11.1 SHA-256 PURE-JS (FUNCIONA EM HTTP PLANO http://mps3.local) ---
        const SHA256_K = new Uint32Array([
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
        ]);

        async function computeSha256(data) {
            if (window.crypto && window.crypto.subtle && window.crypto.subtle.digest) {
                try {
                    const bytes = (data instanceof Uint8Array) ? data : new Uint8Array(data);
                    const hashBuf = await window.crypto.subtle.digest('SHA-256', bytes);
                    return Array.from(new Uint8Array(hashBuf)).map(b => b.toString(16).padStart(2, '0')).join('');
                } catch (e) {
                    console.warn('Fallback para sha256Bytes (Pure-JS):', e);
                }
            }
            return sha256Bytes(data);
        }

        function sha256Bytes(data) {
            const bytes = (data instanceof Uint8Array) ? data : new Uint8Array(data);
            const len = bytes.length;
            const bitLenHi = Math.floor((len * 8) / 0x100000000);
            const bitLenLo = (len * 8) >>> 0;
            const totalBlocks = (((len + 9 + 63) >> 6) << 6);
            const padded = new Uint8Array(totalBlocks);
            padded.set(bytes, 0);
            padded[len] = 0x80;
            const view = new DataView(padded.buffer);
            view.setUint32(totalBlocks - 8, bitLenHi, false);
            view.setUint32(totalBlocks - 4, bitLenLo, false);

            let h0 = 0x6a09e667, h1 = 0xbb67ae85, h2 = 0x3c6ef372, h3 = 0xa54ff53a;
            let h4 = 0x510e527f, h5 = 0x9b05688c, h6 = 0x1f83d9ab, h7 = 0x5be0cd19;
            const w = new Uint32Array(64);

            for (let offset = 0; offset < totalBlocks; offset += 64) {
                for (let i = 0; i < 16; i++) {
                    w[i] = view.getUint32(offset + i * 4, false);
                }
                for (let i = 16; i < 64; i++) {
                    const v15 = w[i - 15], v2 = w[i - 2];
                    const s0 = ((v15 >>> 7) | (v15 << 25)) ^ ((v15 >>> 18) | (v15 << 14)) ^ (v15 >>> 3);
                    const s1 = ((v2 >>> 17) | (v2 << 15)) ^ ((v2 >>> 19) | (v2 << 13)) ^ (v2 >>> 10);
                    w[i] = (w[i - 16] + s0 + w[i - 7] + s1) >>> 0;
                }

                let a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, h = h7;
                for (let i = 0; i < 64; i++) {
                    const S1 = ((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7));
                    const ch = (e & f) ^ ((~e) & g);
                    const temp1 = (h + S1 + ch + SHA256_K[i] + w[i]) >>> 0;
                    const S0 = ((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10));
                    const maj = (a & b) ^ (a & c) ^ (b & c);
                    const temp2 = (S0 + maj) >>> 0;

                    h = g; g = f; f = e;
                    e = (d + temp1) >>> 0;
                    d = c; c = b; b = a;
                    a = (temp1 + temp2) >>> 0;
                }

                h0 = (h0 + a) >>> 0; h1 = (h1 + b) >>> 0; h2 = (h2 + c) >>> 0; h3 = (h3 + d) >>> 0;
                h4 = (h4 + e) >>> 0; h5 = (h5 + f) >>> 0; h6 = (h6 + g) >>> 0; h7 = (h7 + h) >>> 0;
            }

            const hex = (n) => (n >>> 0).toString(16).padStart(8, '0');
            return hex(h0) + hex(h1) + hex(h2) + hex(h3) + hex(h4) + hex(h5) + hex(h6) + hex(h7);
        }

        // --- 11.2 SANITIZAÇÃO DE NOMES E IDENTIFICAÇÃO DE EPISÓDIOS (FAT32 SAFE) ---
        function sanitizePodSafeName(raw, maxLen = 72) {
            if (!raw) return 'Episodio';
            const noAccents = String(raw).normalize('NFD').replace(/[\u0300-\u036f]/g, '');
            const clean = noAccents
                .replace(/[<>:"/\\|?*\x00-\x1F]/g, ' ')
                .replace(/\s+/g, ' ')
                .trim()
                .replace(/^\.+|\.+$/g, '');
            return (clean || 'Episodio').slice(0, maxLen).trim();
        }

        function extractEpisodeKeyFromName(filename) {
            const name = String(filename || '').trim();
            // 1. Tenta extrair [video_id] entre colchetes no final do stem
            const bracketMatch = name.match(/\[([A-Za-z0-9_-]{6,20})\]\.[^.]+$/);
            if (bracketMatch) return 'id:' + bracketMatch[1].toLowerCase();
            // 2. Fallback: stem normalizado sem extensão
            const stem = name.replace(/\.[^.]+$/, '').toLowerCase().replace(/\s+/g, ' ').trim();
            return 'stem:' + stem;
        }

        function buildPodEpisodeFilename(ep, extOverride) {
            const ext = (extOverride || ep.ext || 'm4a').replace(/^\.+/, '').toLowerCase();
            if (ep.filename && !extOverride) return ep.filename;
            let datePrefix = '00000000';
            if (ep.mtime && ep.mtime > 0) {
                const d = new Date(ep.mtime * 1000);
                const y = d.getUTCFullYear();
                const m = String(d.getUTCMonth() + 1).padStart(2, '0');
                const day = String(d.getUTCDate()).padStart(2, '0');
                datePrefix = `${y}${m}${day}`;
            }
            const safeTitle = sanitizePodSafeName(ep.title || 'Episodio', 56);
            const idSuffix = ep.id ? ` [${sanitizePodSafeName(ep.id, 16)}]` : '';
            return `${datePrefix} - ${safeTitle}${idSuffix}.${ext}`;
        }

        // --- 11.3 BROWSER SYNC PLANNER (PORT 1:1 DE planner.py) ---
        const BrowserSyncPlanner = {
            KEEP_RECENT_DEFAULT: 3,
            SAFETY_RESERVE_BYTES: 32 * 1024 * 1024,

            computeSyncPlan(remoteEpisodes, manifest, options = {}) {
                const keepRecent = parseInt(options.keepRecent || this.KEEP_RECENT_DEFAULT, 10) || 3;
                const autoCleanPlayed = options.autoCleanPlayed !== false;
                const safetyReserve = options.safetyReserveBytes !== undefined ? options.safetyReserveBytes : this.SAFETY_RESERVE_BYTES;

                const sdPodcasts = (manifest && Array.isArray(manifest.podcasts)) ? manifest.podcasts : [];
                const playedHistory = new Set(
                    ((manifest && Array.isArray(manifest.played_history)) ? manifest.played_history : [])
                        .map(s => String(s).toLowerCase().trim())
                );

                // Indexa arquivos atualmente no SD por canal e por chave de episódio
                const sdByProgram = new Map();
                const sdKeyMap = new Map();
                for (const item of sdPodcasts) {
                    const prog = item.program || 'Geral';
                    const fname = item.filename || '';
                    const fullRelLower = `${prog}/${fname}`.toLowerCase();
                    const epKey = `${prog.toLowerCase()}::${extractEpisodeKeyFromName(fname)}`;
                    const isPlayed = Boolean(item.played) || playedHistory.has(fullRelLower) || playedHistory.has(fname.toLowerCase());
                    const entry = {
                        program: prog,
                        filename: fname,
                        path: item.path || (item.rel_path ? '/' + String(item.rel_path).replace(/^\/+/, '') : `/Podcasts/${prog}/${fname}`),
                        size: item.size || 0,
                        played: isPlayed,
                        epKey
                    };
                    if (!sdByProgram.has(prog.toLowerCase())) sdByProgram.set(prog.toLowerCase(), []);
                    sdByProgram.get(prog.toLowerCase()).push(entry);
                    sdKeyMap.set(epKey, entry);
                    sdKeyMap.set(fullRelLower, entry);
                }

                // Agrupa candidatos remotos por canal ordenados por data decrescente
                const remoteByProg = new Map();
                for (const ep of (remoteEpisodes || [])) {
                    const prog = ep.program || 'Geral';
                    const key = prog.toLowerCase();
                    if (!remoteByProg.has(key)) remoteByProg.set(key, []);
                    remoteByProg.get(key).push(ep);
                }

                const downloads = [];
                const deletes = [];
                const skippedPlayed = [];
                const keptUnplayed = [];

                for (const [progLower, epList] of remoteByProg.entries()) {
                    epList.sort((a, b) => (b.mtime || 0) - (a.mtime || 0));
                    const topRecent = epList.slice(0, keepRecent);
                    const topKeys = new Set();

                    for (const ep of topRecent) {
                        const fname = buildPodEpisodeFilename(ep);
                        const fullRelLower = `${ep.program}/${fname}`.toLowerCase();
                        const epKey = `${progLower}::${ep.id ? 'id:' + String(ep.id).toLowerCase() : extractEpisodeKeyFromName(fname)}`;
                        topKeys.add(epKey);
                        topKeys.add(fullRelLower);

                        const alreadyOnSd = sdKeyMap.get(epKey) || sdKeyMap.get(fullRelLower);
                        const wasPlayedInLedger =
                            playedHistory.has(fullRelLower) ||
                            playedHistory.has(fname.toLowerCase()) ||
                            (ep.id && Array.from(playedHistory).some(h => h.includes(`[${String(ep.id).toLowerCase()}]`)));

                        if (alreadyOnSd) {
                            if (!alreadyOnSd.played) keptUnplayed.push(alreadyOnSd);
                            continue;
                        }
                        if (wasPlayedInLedger) {
                            skippedPlayed.push(ep);
                            continue;
                        }
                        downloads.push({
                            ...ep,
                            filename: fname,
                            selected: true
                        });
                    }

                    // Avalia arquivos no SD deste canal que saíram do Top N
                    const sdItems = sdByProgram.get(progLower) || [];
                    for (const sdItem of sdItems) {
                        const fullRelLower = `${sdItem.program}/${sdItem.filename}`.toLowerCase();
                        const inTop = topKeys.has(sdItem.epKey) || topKeys.has(fullRelLower);
                        if (!inTop) {
                            if (sdItem.played && autoCleanPlayed) {
                                deletes.push(sdItem);
                            } else {
                                // Regra de Ouro: NUNCA apaga episódio não ouvido!
                                keptUnplayed.push(sdItem);
                            }
                        }
                    }
                }

                // Ordena downloads por data decrescente e verifica orçamento de espaço no SD
                downloads.sort((a, b) => (b.mtime || 0) - (a.mtime || 0));
                const freeBytes = (manifest && typeof manifest.free_bytes === 'number') ? manifest.free_bytes : (8 * 1024 * 1024 * 1024);
                const bytesToFree = deletes.reduce((acc, d) => acc + (d.size || 0), 0);
                let budgetBytes = Math.max(0, freeBytes + bytesToFree - safetyReserve);

                for (const dl of downloads) {
                    const estSize = dl.size_bytes || (15 * 1024 * 1024);
                    if (estSize <= budgetBytes) {
                        budgetBytes -= estSize;
                    } else {
                        dl.selected = false;
                        dl.skipped_low_space = true;
                    }
                }

                return {
                    downloads,
                    deletes,
                    skippedPlayed,
                    keptUnplayed,
                    bytesToFree,
                    budgetRemaining: budgetBytes
                };
            }
        };

        // --- 11.4 SCRAPER YOUTUBE (MODULAR VIA MPS3_YT_EXTRACTOR) & RSS OFICIAL ---
        const DEFAULT_POD_SUBSCRIPTIONS = [
            {
                id: 'sub_nerdcast',
                name: 'NerdCast',
                type: 'rss',
                url: 'https://feeds.megaphone.fm/JNPD6227286900',
                enabled: true
            },
            {
                id: 'sub_vortex',
                name: 'Vortex',
                type: 'rss',
                url: 'https://anchor.fm/s/10a0d0948/podcast/rss',
                enabled: true
            },
            {
                id: 'sub_horaboa',
                name: 'Hora Boa Podcast',
                type: 'rss',
                url: 'https://anchor.fm/s/1068cff58/podcast/rss',
                enabled: true
            },
            {
                id: 'sub_dlshow',
                name: 'Desce a Letra Show',
                type: 'youtube',
                url: 'https://www.youtube.com/@DESCEALETRASHOW/streams',
                channel_id: 'UCCD93px0UneWNFg_HxV4naQ',
                tab: 'streams',
                enabled: true
            }
        ];

        // Busca Feeds RSS oficiais via API pública do Apple Podcasts / iTunes (100% legal e com CORS=*)
        async function discoverOfficialRssFeeds(query, limit = 5) {
            const q = String(query || '').trim();
            if (!q) return [];
            const apiUrl = `https://itunes.apple.com/search?media=podcast&country=BR&limit=${limit}&term=${encodeURIComponent(q)}`;
            const res = await fetchWithTimeout(apiUrl, {}, 10000);
            if (!res.ok) throw new Error('Falha ao consultar diretório Apple Podcasts (HTTP ' + res.status + ')');
            const data = await res.json();
            return (data.results || [])
                .filter(it => it && it.feedUrl)
                .map(it => ({
                    name: String(it.collectionName || '').trim(),
                    artist: String(it.artistName || '').trim(),
                    feedUrl: String(it.feedUrl || '').trim(),
                    trackCount: it.trackCount || 0
                }));
        }

        const DEFAULT_API_INSTANCES = [
            'https://pipedapi.kavin.rocks',
            'https://piped-api.garudalinux.org',
            'https://inv.tux.pizza',
            'https://invidious.nerdvpn.de',
            'https://vid.puffyan.us'
        ];

        function getConfiguredApiInstances() {
            const inp = document.getElementById('podCustomApiInstances');
            const raw = (inp && inp.value && inp.value.trim()) || localStorage.getItem('mps3_pod_api_pool') || DEFAULT_API_INSTANCES.join(', ');
            const list = raw.split(',').map(s => s.trim().replace(/\/+$/, '')).filter(Boolean);
            if (typeof window !== 'undefined' && window.location && (window.location.hostname === '127.0.0.1' || window.location.hostname === 'localhost')) {
                const origin = window.location.origin.replace(/\/+$/, '');
                if (origin && !list.includes(origin)) list.unshift(origin);
            }
            return list;
        }

        function parseXmlDocument(xmlString) {
            if (typeof DOMParser !== 'undefined') {
                const parser = new DOMParser();
                return parser.parseFromString(xmlString, 'text/xml');
            }
            // Fallback leve para ambientes headless/Node.js ou Web Workers sem DOMParser
            function makeNode(rawXml) {
                return {
                    getElementsByTagName(tagName) {
                        const escaped = tagName.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
                        const re = new RegExp(`<${escaped}(\\s[^>]*)?>([\\s\\S]*?)<\\/${escaped}>|<${escaped}(\\s[^>]*)?\\/>`, 'gi');
                        const matches = [];
                        let m;
                        while ((m = re.exec(rawXml)) !== null) {
                            const attrsRaw = m[1] || m[3] || '';
                            const inner = m[2] || '';
                            const node = makeNode(inner);
                            node.textContent = inner.replace(/<!\[CDATA\[([\s\S]*?)\]\]>/g, '$1').trim();
                            node.getAttribute = (attr) => {
                                const are = new RegExp(`${attr}\\s*=\\s*["']([^"']*)["']`, 'i');
                                const am = are.exec(attrsRaw);
                                return am ? am[1] : null;
                            };
                            matches.push(node);
                        }
                        return matches;
                    }
                };
            }
            return makeNode(String(xmlString || ''));
        }

        function parseYoutubeAtomXml(xmlString, programName, maxItems = 8) {
            const doc = parseXmlDocument(xmlString);
            const entries = Array.from(doc.getElementsByTagName('entry'));
            const out = [];
            for (const entry of entries.slice(0, maxItems)) {
                const vidNode = entry.getElementsByTagName('yt:videoId')[0] || entry.getElementsByTagName('videoId')[0];
                const titleNode = entry.getElementsByTagName('title')[0];
                const pubNode = entry.getElementsByTagName('published')[0] || entry.getElementsByTagName('updated')[0];
                if (!vidNode || !titleNode) continue;
                const videoId = (vidNode.textContent || '').trim();
                const title = (titleNode.textContent || '').trim();
                const mtime = pubNode ? Math.floor(new Date(pubNode.textContent.trim()).getTime() / 1000) : Math.floor(Date.now() / 1000);
                const epObj = {
                    id: videoId,
                    program: programName,
                    title,
                    mtime: isFinite(mtime) ? mtime : Math.floor(Date.now() / 1000),
                    source_type: 'youtube',
                    video_id: videoId,
                    ext: 'm4a',
                    size_bytes: 14 * 1024 * 1024
                };
                epObj.filename = buildPodEpisodeFilename(epObj, 'm4a');
                out.push(epObj);
            }
            return out;
        }

        function parseRssDurationSec(itemNode) {
            const durNode = itemNode.getElementsByTagName('itunes:duration')[0] || itemNode.getElementsByTagName('duration')[0];
            if (!durNode || !durNode.textContent) return 0;
            const raw = durNode.textContent.trim();
            if (/^\d+$/.test(raw)) return parseInt(raw, 10) || 0;
            if (typeof MPS3_YT_EXTRACTOR !== 'undefined' && typeof MPS3_YT_EXTRACTOR.parseDurationText === 'function') {
                return MPS3_YT_EXTRACTOR.parseDurationText(raw);
            }
            return 0;
        }

        function parsePodcastRssXml(xmlString, programName, maxItems = 8) {
            const doc = parseXmlDocument(xmlString);
            const items = Array.from(doc.getElementsByTagName('item'));
            const out = [];
            for (const item of items.slice(0, maxItems)) {
                const titleNode = item.getElementsByTagName('title')[0];
                const encNode = item.getElementsByTagName('enclosure')[0];
                const pubNode = item.getElementsByTagName('pubDate')[0];
                const guidNode = item.getElementsByTagName('guid')[0];
                if (!encNode) continue;
                let mediaUrl = encNode.getAttribute('url') || '';
                if (!mediaUrl) continue;

                // Desembrulha URLs de redirecionamento do Anchor.fm (/podcast/play/.../https%3A%2F%2F...)
                if (mediaUrl.includes('/podcast/play/') && /https?%3A%2F%2F/i.test(mediaUrl)) {
                    const tail = mediaUrl.split('/podcast/play/')[1] || '';
                    const slashIdx = tail.indexOf('/');
                    if (slashIdx !== -1) {
                        try {
                            mediaUrl = decodeURIComponent(tail.slice(slashIdx + 1));
                        } catch (e) {}
                    }
                }

                const title = titleNode ? (titleNode.textContent || '').trim() : 'Episodio';
                const lenAttr = parseInt(encNode.getAttribute('length') || '0', 10);
                const durationSec = parseRssDurationSec(item);
                const mime = (encNode.getAttribute('type') || '').toLowerCase();
                let ext = 'mp3';
                if (mime.includes('mp4') || mime.includes('m4a') || mediaUrl.toLowerCase().includes('.m4a')) ext = 'm4a';
                else if (mime.includes('ogg') || mime.includes('opus') || mediaUrl.toLowerCase().includes('.ogg')) ext = 'ogg';
                else if (mime.includes('wav') || mediaUrl.toLowerCase().includes('.wav')) ext = 'wav';

                const rawGuid = guidNode ? (guidNode.textContent || '').trim() : mediaUrl;
                const shortId = sha256Bytes(new TextEncoder().encode(rawGuid)).slice(0, 10);
                const parsedDate = pubNode ? Math.floor(new Date(pubNode.textContent.trim()).getTime() / 1000) : Math.floor(Date.now() / 1000);
                const estBytes = lenAttr > 0 ? lenAttr : (durationSec > 0 ? durationSec * 16000 : 12 * 1024 * 1024);

                const epObj = {
                    id: shortId,
                    program: programName,
                    title,
                    mtime: isFinite(parsedDate) ? parsedDate : Math.floor(Date.now() / 1000),
                    duration: durationSec || 0,
                    source_type: 'rss',
                    direct_url: mediaUrl,
                    ext,
                    size_bytes: estBytes
                };
                epObj.filename = buildPodEpisodeFilename(epObj, ext);
                out.push(epObj);
            }
            return out;
        }

        async function fetchWithTimeout(url, options = {}, timeoutMs = 12000) {
            const ctrl = new AbortController();
            const tid = setTimeout(() => ctrl.abort(), timeoutMs);
            try {
                return await fetch(url, { ...options, signal: ctrl.signal });
            } finally {
                clearTimeout(tid);
            }
        }

        // Ponte Zero-CORS nativa no próprio ESP32-S3 (/api/yt/innertube)
        const YT_BROWSE_FIELDS_MASK =
            'responseContext.visitorData,' +
            'contents.twoColumnBrowseResultsRenderer.tabs.tabRenderer.content.richGridRenderer.contents.richItemRenderer.content(' +
            'lockupViewModel(contentId,metadata.lockupMetadataViewModel(title.content,metadata.contentMetadataViewModel.metadataRows(metadataParts.text.content,badges.badgeViewModel(badgeStyle,badgeText,iconName))),contentImage.thumbnailViewModel.overlays.thumbnailBottomOverlayViewModel.badges.thumbnailBadgeViewModel(badgeStyle,text)),' +
            'videoRenderer(videoId,title,lengthText,badges,thumbnailOverlays,publishedTimeText,upcomingEventData))';

        const YT_PLAYER_FIELDS_MASK =
            'playabilityStatus,videoDetails(videoId,title,lengthSeconds),streamingData.adaptiveFormats(itag,url,mimeType,bitrate,contentLength,isDrc,audioTrack)';

        async function mps3InnertubeRelay(meta, postBodyObj = null, timeoutMs = 18000) {
            const bodyStr = postBodyObj
                ? (typeof postBodyObj === 'string' ? postBodyObj : JSON.stringify(postBodyObj))
                : '';
            const envelope = JSON.stringify(meta) + (bodyStr ? ('\n' + bodyStr) : '');
            const res = await fetchWithTimeout('/api/yt/innertube', {
                method: 'POST',
                headers: { 'Content-Type': 'text/plain;charset=UTF-8' },
                body: envelope
            }, timeoutMs);
            if (!res.ok) {
                throw new Error('Relay HTTP ' + res.status);
            }
            return res;
        }

        async function scrapeYoutubeViaMps3Relay(sub, maxItems = 6) {
            if (typeof MPS3_YT_EXTRACTOR === 'undefined') return [];
            const target = MPS3_YT_EXTRACTOR.parseSubscriptionTarget(sub);
            const browseClient = MPS3_YT_EXTRACTOR.CONFIG.browseClient || {
                clientName: 'WEB',
                clientVersion: '2.20250101.00.00',
                hl: 'pt',
                gl: 'BR'
            };

            let channelId = /^UC[A-Za-z0-9_-]{20,}$/.test(target.channelRef) ? target.channelRef : '';
            if (!channelId) {
                const rawTargetUrl = (sub.url && sub.url.startsWith('http'))
                    ? sub.url
                    : `https://www.youtube.com/${String(target.channelRef || '').replace(/^\/+/, '')}`;
                const resResolve = await mps3InnertubeRelay({
                    url: 'https://www.youtube.com/youtubei/v1/navigation/resolve_url?prettyPrint=false&fields=endpoint.browseEndpoint(browseId,params)',
                    method: 'POST'
                }, {
                    url: rawTargetUrl,
                    context: { client: browseClient }
                }, 12000);
                const rData = await resResolve.json();
                channelId = (rData && rData.endpoint && rData.endpoint.browseEndpoint && rData.endpoint.browseEndpoint.browseId) || '';
                if (!channelId) throw new Error('Não foi possível resolver o browseId do canal: ' + rawTargetUrl);
                sub.channel_id = channelId;
            }

            const tabParamsList = (target.tabParamsOrder && target.tabParamsOrder.length)
                ? target.tabParamsOrder
                : ['EgdzdHJlYW1z8gYECgJ6AA==', 'EgZ2aWRlb3PyBgQKAjoA'];

            for (const tabParams of tabParamsList) {
                try {
                    const resBrowse = await mps3InnertubeRelay({
                        url: `https://www.youtube.com/youtubei/v1/browse?prettyPrint=false&fields=${YT_BROWSE_FIELDS_MASK}`,
                        method: 'POST'
                    }, {
                        browseId: channelId,
                        params: tabParams,
                        context: { client: browseClient }
                    }, 18000);
                    const bdata = await resBrowse.json();
                    if (bdata && bdata.responseContext && bdata.responseContext.visitorData) {
                        window.__mps3YtVisitorData = bdata.responseContext.visitorData;
                    }
                    const items = MPS3_YT_EXTRACTOR.extractVideosFromBrowseTree(bdata, sub.name, maxItems);
                    if (Array.isArray(items) && items.length > 0) {
                        return items;
                    }
                } catch (e) {}
            }
            return [];
        }

        async function resolveYoutubeStreamViaMps3Relay(videoId) {
            if (typeof MPS3_YT_EXTRACTOR === 'undefined') return null;
            const browseClient = MPS3_YT_EXTRACTOR.CONFIG.browseClient || {
                clientName: 'WEB',
                clientVersion: '2.20250101.00.00',
                hl: 'pt',
                gl: 'BR'
            };

            if (!window.__mps3YtVisitorData) {
                try {
                    const visRes = await mps3InnertubeRelay({
                        url: 'https://www.youtube.com/youtubei/v1/browse?prettyPrint=false&fields=responseContext.visitorData',
                        method: 'POST'
                    }, {
                        browseId: 'FEwhat_to_watch',
                        context: { client: browseClient }
                    }, 10000);
                    const visJson = await visRes.json();
                    if (visJson && visJson.responseContext && visJson.responseContext.visitorData) {
                        window.__mps3YtVisitorData = visJson.responseContext.visitorData;
                    }
                } catch (e) {}
            }

            const visitorData = window.__mps3YtVisitorData || '';
            const clients = MPS3_YT_EXTRACTOR.CONFIG.playerClients || [];

            for (const c of clients) {
                try {
                    const clientPayload = {
                        clientName: c.clientName,
                        clientVersion: c.clientVersion,
                        hl: c.hl || 'pt',
                        gl: c.gl || 'BR'
                    };
                    if (c.deviceMake) clientPayload.deviceMake = c.deviceMake;
                    if (c.deviceModel) clientPayload.deviceModel = c.deviceModel;
                    if (c.osName) clientPayload.osName = c.osName;
                    if (c.osVersion) clientPayload.osVersion = c.osVersion;
                    if (c.androidSdkVersion) clientPayload.androidSdkVersion = c.androidSdkVersion;
                    if (visitorData) clientPayload.visitorData = visitorData;

                    const pRes = await mps3InnertubeRelay({
                        url: `https://www.youtube.com/youtubei/v1/player?prettyPrint=false&fields=${YT_PLAYER_FIELDS_MASK}`,
                        method: 'POST',
                        ua: c.userAgent || '',
                        client_name: String(c.clientId || '101'),
                        client_version: String(c.clientVersion || '1.02'),
                        visitor_id: visitorData
                    }, {
                        videoId: videoId,
                        contentCheckOk: true,
                        racyCheckOk: true,
                        context: { client: clientPayload }
                    }, 15000);

                    const pData = await pRes.json();
                    const chosen = MPS3_YT_EXTRACTOR.selectBestAudioFormat(pData);
                    if (chosen && chosen.url) {
                        chosen.userAgent = c.userAgent || '';
                        return chosen;
                    }
                } catch (e) {}
            }
            return null;
        }

        async function scrapeYoutubeSubscription(sub, maxItems = 6) {
            // 1. Se o Userscript Bridge estiver instalado no navegador, usa acesso direto sem CORS
            if (window.__MPS3_YT_BRIDGE__ && typeof window.__MPS3_YT_BRIDGE__.fetchChannelVideos === 'function') {
                const items = await window.__MPS3_YT_BRIDGE__.fetchChannelVideos(sub, maxItems);
                if (Array.isArray(items) && items.length > 0) return items;
            }

            // 2. Se sub.url já aponta para um feed XML ou endpoint JSON mock/direto
            if (sub.url && (sub.url.includes('/feeds/videos.xml') || sub.url.endsWith('.xml'))) {
                const res = await fetchWithTimeout(sub.url);
                if (res.ok) {
                    const txt = await res.text();
                    return parseYoutubeAtomXml(txt, sub.name, maxItems);
                }
            }

            // 3. Usa o Relay Zero-CORS nativo do ESP32-S3 (/api/yt/innertube)
            try {
                const relayItems = await scrapeYoutubeViaMps3Relay(sub, maxItems);
                if (Array.isArray(relayItems) && relayItems.length > 0) {
                    return relayItems;
                }
            } catch (e) {
                console.warn('Relay nativo MPS3 indisponível, tentando instâncias de fallback:', e);
            }

            // Extrai identificador do canal (UC... ou @handle) e aba alvo (/streams, /videos, /podcasts)
            const target = (typeof MPS3_YT_EXTRACTOR !== 'undefined')
                ? MPS3_YT_EXTRACTOR.parseSubscriptionTarget(sub)
                : { channelRef: (sub.channel_id || '').trim(), preferredTab: sub.tab || 'videos' };
            const channelRef = target.channelRef;
            const tabQuery = `?tab=${encodeURIComponent(target.preferredTab || 'videos')}`;

            const instances = getConfiguredApiInstances();
            let lastErr = null;

            for (const base of instances) {
                try {
                    const candidateUrls = [];
                    if (channelRef.startsWith('UC')) {
                        candidateUrls.push(`${base}/channels/${channelRef}${tabQuery}`);
                        candidateUrls.push(`${base}/api/v1/channels/${channelRef}/${target.preferredTab || 'videos'}`);
                        candidateUrls.push(`${base}/feed/channel/${channelRef}`);
                    } else if (channelRef.startsWith('@')) {
                        candidateUrls.push(`${base}/c/${encodeURIComponent(channelRef)}${tabQuery}`);
                        candidateUrls.push(`${base}/api/v1/channels/${encodeURIComponent(channelRef)}/${target.preferredTab || 'videos'}`);
                    } else if (sub.url && sub.url.startsWith('http')) {
                        candidateUrls.push(sub.url);
                    }

                    for (const apiUrl of candidateUrls) {
                        const res = await fetchWithTimeout(apiUrl, {}, 9000);
                        if (!res.ok) continue;
                        const ctype = (res.headers.get('content-type') || '').toLowerCase();
                        if (ctype.includes('xml') || apiUrl.includes('feed')) {
                            const xmlTxt = await res.text();
                            const parsed = parseYoutubeAtomXml(xmlTxt, sub.name, maxItems);
                            if (parsed.length > 0) return parsed;
                            continue;
                        }
                        const data = await res.json();

                        // Se a resposta for uma árvore InnerTube bruta, delega ao extrator modular
                        if (data && data.contents && typeof MPS3_YT_EXTRACTOR !== 'undefined') {
                            const extracted = MPS3_YT_EXTRACTOR.extractVideosFromBrowseTree(data, sub.name, maxItems);
                            if (extracted.length > 0) {
                                extracted.forEach(ep => { ep.api_base = base; });
                                return extracted;
                            }
                        }

                        const rawVideos = Array.isArray(data) ? data : (data.relatedStreams || data.videos || []);
                        if (!Array.isArray(rawVideos) || !rawVideos.length) continue;

                        const episodes = [];
                        const baseSec = Math.floor(Date.now() / 1000);
                        for (let idx = 0; idx < rawVideos.length && episodes.length < maxItems; idx++) {
                            const v = rawVideos[idx];
                            if (v.membersOnly || v.upcoming || v.isLive) continue;
                            let vid = v.videoId || '';
                            if (!vid && v.url) {
                                const m = String(v.url).match(/(?:v=|\/watch\?v=|\/shorts\/)([A-Za-z0-9_-]{6,20})/);
                                if (m) vid = m[1];
                            }
                            if (!vid) continue;
                            const mtime = v.uploaded ? Math.floor(v.uploaded / 1000) : (v.published || (baseSec - idx * 60));
                            const duration = v.duration || v.lengthSeconds || 900;
                            const estBytes = Math.max(1024 * 512, duration * 16000);
                            const epObj = {
                                id: vid,
                                program: sub.name,
                                title: v.title || vid,
                                mtime,
                                duration,
                                source_type: 'youtube',
                                video_id: vid,
                                direct_url: v.audioUrl || v.direct_url || null,
                                ext: v.ext || 'm4a',
                                size_bytes: v.size_bytes || estBytes,
                                api_base: base
                            };
                            epObj.filename = buildPodEpisodeFilename(epObj, epObj.ext);
                            episodes.push(epObj);
                        }
                        if (episodes.length > 0) return episodes;
                    }
                } catch (e) {
                    lastErr = e;
                }
            }
            throw lastErr || new Error(`Não foi possível raspar o canal YouTube "${sub.name}" nas instâncias configuradas.`);
        }

        async function resolveYoutubeAudioStream(ep) {
            if (ep.direct_url) {
                return { url: ep.direct_url, ext: ep.ext || 'm4a', mime: 'audio/mp4' };
            }
            const videoId = ep.video_id || ep.id;
            if (window.__MPS3_YT_BRIDGE__ && typeof window.__MPS3_YT_BRIDGE__.resolveAudioStream === 'function') {
                const br = await window.__MPS3_YT_BRIDGE__.resolveAudioStream(videoId);
                if (br && br.url) return br;
            }

            // Tenta primeiro via Relay Zero-CORS nativo do ESP32-S3 (/api/yt/innertube com VISIONOS 1.02)
            try {
                const relResolved = await resolveYoutubeStreamViaMps3Relay(videoId);
                if (relResolved && relResolved.url) return relResolved;
            } catch (e) {}

            const instances = ep.api_base ? [ep.api_base, ...getConfiguredApiInstances()] : getConfiguredApiInstances();
            for (const base of instances) {
                try {
                    // 1. Formato Piped: GET /streams/<videoId>
                    const rPiped = await fetchWithTimeout(`${base}/streams/${videoId}`, {}, 10000);
                    if (rPiped.ok) {
                        const j = await rPiped.json();
                        const streams = j.audioStreams || [];
                        if (streams.length > 0) {
                            // Prioriza M4A (AAC) ou Opus/WebM de ~128kbps
                            streams.sort((a, b) => {
                                const aM4a = String(a.mimeType || '').includes('mp4') ? 1 : 0;
                                const bM4a = String(b.mimeType || '').includes('mp4') ? 1 : 0;
                                if (aM4a !== bM4a) return bM4a - aM4a;
                                return (b.bitrate || 0) - (a.bitrate || 0);
                            });
                            const best = streams[0];
                            const isMp4 = String(best.mimeType || '').includes('mp4');
                            return {
                                url: best.url,
                                ext: isMp4 ? 'm4a' : 'webm',
                                mime: best.mimeType || (isMp4 ? 'audio/mp4' : 'audio/webm')
                            };
                        }
                    }

                    // 2. Formato Invidious: GET /api/v1/videos/<videoId> (com ?local=true para proxy CORS)
                    const rInv = await fetchWithTimeout(`${base}/api/v1/videos/${videoId}?local=true`, {}, 10000);
                    if (rInv.ok) {
                        const j = await rInv.json();
                        const adaptive = (j.adaptiveFormats || []).filter(f => String(f.type || '').startsWith('audio/'));
                        if (adaptive.length > 0) {
                            const m4aStream = adaptive.find(f => String(f.type || '').includes('mp4')) || adaptive[0];
                            let streamUrl = m4aStream.url;
                            if (streamUrl.startsWith('/')) streamUrl = base + streamUrl;
                            const isMp4 = String(m4aStream.type || '').includes('mp4');
                            return {
                                url: streamUrl,
                                ext: isMp4 ? 'm4a' : 'webm',
                                mime: m4aStream.type || 'audio/mp4'
                            };
                        }
                    }
                } catch (e) {}
            }
            throw new Error(`Stream de áudio não encontrado para o vídeo ${videoId}.`);
        }

        // --- 11.5 CONVERSÃO DE ÁUDIO INTELIGENTE NO NAVEGADOR (WEB AUDIO API) ---
        function encodeAudioBufferToWavMono16(audioBuffer, targetSampleRate = 22050) {
            const numChannels = audioBuffer.numberOfChannels;
            const srcRate = audioBuffer.sampleRate;
            const srcLen = audioBuffer.length;
            const ch0 = audioBuffer.getChannelData(0);
            const ch1 = numChannels > 1 ? audioBuffer.getChannelData(1) : null;

            const ratio = srcRate / targetSampleRate;
            const outSamples = Math.max(1, Math.floor(srcLen / ratio));
            const wavBytes = new Uint8Array(44 + outSamples * 2);
            const view = new DataView(wavBytes.buffer);

            const writeStr = (off, s) => { for (let i = 0; i < s.length; i++) wavBytes[off + i] = s.charCodeAt(i); };
            writeStr(0, 'RIFF');
            view.setUint32(4, 36 + outSamples * 2, true);
            writeStr(8, 'WAVE');
            writeStr(12, 'fmt ');
            view.setUint32(16, 16, true);
            view.setUint16(20, 1, true); // PCM
            view.setUint16(22, 1, true); // Mono (Voz / Podcast compacto)
            view.setUint32(24, targetSampleRate, true);
            view.setUint32(28, targetSampleRate * 2, true);
            view.setUint16(32, 2, true);
            view.setUint16(34, 16, true);
            writeStr(36, 'data');
            view.setUint32(40, outSamples * 2, true);

            let offset = 44;
            for (let i = 0; i < outSamples; i++) {
                const srcIdx = Math.min(srcLen - 1, Math.floor(i * ratio));
                let sample = ch1 ? (ch0[srcIdx] + ch1[srcIdx]) * 0.5 : ch0[srcIdx];
                sample = Math.max(-1, Math.min(1, sample));
                view.setInt16(offset, sample < 0 ? sample * 0x8000 : sample * 0x7FFF, true);
                offset += 2;
            }
            return wavBytes;
        }

        async function processEpisodeAudioBytes(rawBytes, ext, forceReencode) {
            const nativeExts = ['mp3', 'm4a', 'aac', 'wav', 'flac', 'ogg', 'opus', 'webm'];
            if (!forceReencode && nativeExts.includes(String(ext || '').toLowerCase())) {
                return { bytes: rawBytes, ext: ext.toLowerCase() };
            }
            const AudioCtx = window.AudioContext || window.webkitAudioContext;
            if (!AudioCtx) {
                return { bytes: rawBytes, ext: ext || 'm4a' };
            }
            const ctx = new AudioCtx();
            try {
                const copyBuf = rawBytes.buffer.slice(rawBytes.byteOffset, rawBytes.byteOffset + rawBytes.byteLength);
                const decoded = await ctx.decodeAudioData(copyBuf);
                const wavBytes = encodeAudioBufferToWavMono16(decoded, 22050);
                return { bytes: wavBytes, ext: 'wav' };
            } finally {
                if (ctx.close) ctx.close().catch(() => {});
            }
        }

        // --- 11.6 ESTADO DA UI, ASSINATURAS NO SD E ORQUESTRAÇÃO DE SINCRONIA ---
        let podSourceMode = 'browser'; // 'browser' | 'server'
        let podSubscriptions = [];
        let podPendingList = [];
        let podPlannedDeletes = [];
        let podSyncXhr = null;
        let podSyncAbortCtrl = null;
        let isPodSyncing = false;

        async function pullEpisodeDirectOnMps3(mediaUrl, stagingPath, resolvedContentLength, resolvedUserAgent, idx, totalCount, batchTotal = 0, displayTitle = '') {
            let cleanUrl = String(mediaUrl || '');
            const embMatch = cleanUrl.match(/\/podcast\/play\/[^/]+\/(https?%3A%2F%2F.+)$/i);
            if (embMatch) {
                try { cleanUrl = decodeURIComponent(embMatch[1]); } catch (e) {}
            }
            const isGv = cleanUrl.includes('googlevideo.com');
            const chunkSz = 8 * 1024 * 1024;
            const fragUrls = (isGv && resolvedContentLength > 0 && typeof MPS3_YT_EXTRACTOR !== 'undefined')
                ? MPS3_YT_EXTRACTOR.buildFragmentUrls(cleanUrl, resolvedContentLength, chunkSz)
                : [cleanUrl];

            let offsetBytes = 0;
            const decoder = new TextDecoder();

            for (let f = 0; f < fragUrls.length; f++) {
                if (!isPodSyncing) throw new Error('Cancelado pelo usuário');
                podSyncAbortCtrl = new AbortController();

                const resp = await fetch('/api/sync/pull_direct', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({
                        url: fragUrls[f],
                        staging_path: stagingPath,
                        title: displayTitle || '',
                        ua: resolvedUserAgent || '',
                        append: f > 0 ? 1 : 0,
                        idx: idx,
                        count: totalCount,
                        total_bytes: resolvedContentLength || 0,
                        batch_total: batchTotal || 0,
                        offset_bytes: offsetBytes
                    }),
                    signal: podSyncAbortCtrl.signal
                });

                if (!resp.ok) {
                    throw new Error(`Pull direto falhou (HTTP ${resp.status})`);
                }

                let fragWritten = 0;
                let finalErr = null;
                let buffer = '';

                const processLine = (line) => {
                    const trimmed = line.trim();
                    if (!trimmed) return;
                    let msg;
                    try { msg = JSON.parse(trimmed); } catch (e) { return; }
                    if (msg.rx !== undefined) {
                        const tot = msg.total || resolvedContentLength || 0;
                        if (tot > 0) {
                            const pct = Math.min(99, Math.round((msg.rx * 100) / tot));
                            document.getElementById('podItemBar').style.width = pct + '%';
                            document.getElementById('podItemPct').textContent = pct + '%';
                        }
                        const mbRx = (msg.rx / (1024 * 1024)).toFixed(1);
                        const kbs = msg.speed_kbs || 0;
                        const spdTxt = kbs >= 1024 ? (kbs / 1024).toFixed(2) + ' MB/s' : Math.round(kbs) + ' KB/s';
                        document.getElementById('podSpeedInfo').textContent = `${mbRx} MB (${spdTxt})`;
                    }
                    if (msg.done) {
                        if (msg.status === 'ok') {
                            fragWritten = msg.bytes || 0;
                        } else {
                            finalErr = msg.error || ('HTTP ' + (msg.http_status || 502));
                        }
                    }
                };

                if (resp.body && typeof resp.body.getReader === 'function') {
                    const reader = resp.body.getReader();
                    while (true) {
                        const { done, value } = await reader.read();
                        if (done) break;
                        if (value) {
                            buffer += decoder.decode(value, { stream: true });
                            const lines = buffer.split('\n');
                            buffer = lines.pop() || '';
                            for (const ln of lines) processLine(ln);
                        }
                    }
                    if (buffer.trim()) processLine(buffer);
                } else {
                    const fullTxt = await resp.text();
                    for (const ln of fullTxt.split('\n')) processLine(ln);
                }

                podSyncAbortCtrl = null;
                if (finalErr) throw new Error(finalErr);
                offsetBytes += fragWritten;
            }

            document.getElementById('podItemBar').style.width = '100%';
            document.getElementById('podItemPct').textContent = '100%';
            return offsetBytes;
        }

        function setPodSourceMode(mode) {
            podSourceMode = (mode === 'server') ? 'server' : 'browser';
            const tabBrowser = document.getElementById('podTabBrowserBtn');
            const tabServer = document.getElementById('podTabServerBtn');
            const panelBrowser = document.getElementById('podBrowserPanel');
            const panelServer = document.getElementById('podServerPanel');
            const checkBtn = document.getElementById('btnCheckPodcasts');

            if (podSourceMode === 'browser') {
                if (tabBrowser) tabBrowser.className = 'btn-primary';
                if (tabServer) tabServer.className = 'btn-secondary';
                if (panelBrowser) panelBrowser.style.display = 'flex';
                if (panelServer) panelServer.style.display = 'none';
                if (checkBtn) checkBtn.textContent = '🔍 Raspar Canais & Planejar Sincronia com o SD';
            } else {
                if (tabBrowser) tabBrowser.className = 'btn-secondary';
                if (tabServer) tabServer.className = 'btn-primary';
                if (panelBrowser) panelBrowser.style.display = 'none';
                if (panelServer) panelServer.style.display = 'block';
                if (checkBtn) checkBtn.textContent = '🔍 Verificar Novos Episódios no Servidor';
            }
        }

        async function loadPodSubscriptions() {
            // 1. Tenta carregar de /Podcasts/.subscriptions.json no Cartão SD do MPS3
            try {
                const res = await fetchWithTimeout('/api/download?path=' + encodeURIComponent('/Podcasts/.subscriptions.json'), {}, 4000);
                if (res.ok) {
                    const data = await res.json();
                    if (Array.isArray(data.subscriptions) && data.subscriptions.length > 0 && (data.version || 0) >= 4) {
                        podSubscriptions = data.subscriptions;
                        localStorage.setItem('mps3_pod_subs_v4', JSON.stringify(podSubscriptions));
                        renderPodSubscriptions();
                        return;
                    }
                }
            } catch (e) {}

            // 2. Fallback para localStorage ou canais padrão (garantindo os 4 canais oficiais novos)
            try {
                const saved = JSON.parse(localStorage.getItem('mps3_pod_subs_v4') || 'null');
                if (Array.isArray(saved) && saved.length > 0) {
                    podSubscriptions = saved;
                } else {
                    podSubscriptions = JSON.parse(JSON.stringify(DEFAULT_POD_SUBSCRIPTIONS));
                    localStorage.setItem('mps3_pod_subs_v4', JSON.stringify(podSubscriptions));
                }
            } catch (e) {
                podSubscriptions = JSON.parse(JSON.stringify(DEFAULT_POD_SUBSCRIPTIONS));
            }
            renderPodSubscriptions();
        }

        async function savePodSubscriptionsToSd(showFeedback = true) {
            localStorage.setItem('mps3_pod_subs_v4', JSON.stringify(podSubscriptions));
            const statusMsg = document.getElementById('podStatusMsg');
            try {
                const payload = JSON.stringify({ version: 4, updated_at: Math.floor(Date.now() / 1000), subscriptions: podSubscriptions }, null, 2);
                const blob = new Blob([payload], { type: 'application/json' });
                const res = await fetch('/api/upload?path=' + encodeURIComponent('/Podcasts/.subscriptions.json'), {
                    method: 'PUT',
                    body: blob
                });
                if (showFeedback && statusMsg) {
                    statusMsg.textContent = res.ok ? '✓ Canais salvos no SD!' : 'Salvo localmente (SD ocupado)';
                }
            } catch (e) {
                if (showFeedback && statusMsg) statusMsg.textContent = 'Salvo no navegador.';
            }
        }

        function renderPodSubscriptions() {
            const listEl = document.getElementById('podSubsList');
            const badgeEl = document.getElementById('podSubsCountBadge');
            if (!listEl) return;
            const activeCount = podSubscriptions.filter(s => s.enabled !== false).length;
            if (badgeEl) badgeEl.textContent = `${activeCount}/${podSubscriptions.length}`;

            if (!podSubscriptions.length) {
                listEl.innerHTML = '<div style="color:var(--text-muted);text-align:center;padding:6px;">Nenhum canal adicionado. Clique em "➕ Adicionar Canal".</div>';
                return;
            }

            listEl.innerHTML = podSubscriptions.map((sub, idx) => {
                const icon = sub.type === 'rss' ? '📡' : '▶️';
                const checked = sub.enabled !== false ? 'checked' : '';
                return `<div style="display:flex;align-items:center;gap:8px;padding:4px 8px;background:var(--bg-card);border:1px solid var(--border-color);border-radius:5px;">
                    <input type="checkbox" class="pod-sub-toggle" data-idx="${idx}" ${checked} style="cursor:pointer;accent-color:var(--accent-color);">
                    <span title="${sub.type.toUpperCase()}">${icon}</span>
                    <strong style="font-size:0.9em;color:var(--text-primary);white-space:nowrap;">${sub.name}</strong>
                    <span style="flex:1;color:var(--text-muted);font-size:0.8em;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;">${sub.url}</span>
                    <button type="button" class="pod-sub-del" data-idx="${idx}" style="background:none;border:none;color:#dc2626;cursor:pointer;font-size:0.95em;padding:0 4px;" title="Remover canal">✕</button>
                </div>`;
            }).join('');

            listEl.querySelectorAll('.pod-sub-toggle').forEach(cb => {
                cb.onchange = () => {
                    const i = parseInt(cb.dataset.idx, 10);
                    podSubscriptions[i].enabled = cb.checked;
                    localStorage.setItem('mps3_pod_subs_v4', JSON.stringify(podSubscriptions));
                    renderPodSubscriptions();
                };
            });

            listEl.querySelectorAll('.pod-sub-del').forEach(btn => {
                btn.onclick = () => {
                    const i = parseInt(btn.dataset.idx, 10);
                    podSubscriptions.splice(i, 1);
                    localStorage.setItem('mps3_pod_subs_v4', JSON.stringify(podSubscriptions));
                    renderPodSubscriptions();
                };
            });
        }

        function getPodServerUrl() {
            let srv = (document.getElementById('podServerUrl').value || '').trim();
            if (!srv) srv = localStorage.getItem('mps3_pod_srv') || '';
            srv = srv.replace(/\/+$/, '');
            return srv;
        }

        function openPodModal() {
            const saved = localStorage.getItem('mps3_pod_srv') || '';
            document.getElementById('podServerUrl').value = saved;
            const apiInput = document.getElementById('podCustomApiInstances');
            if (apiInput && !apiInput.value) {
                apiInput.value = localStorage.getItem('mps3_pod_api_pool') || DEFAULT_API_INSTANCES.join(', ');
            }
            const bridgeBadge = document.getElementById('podBridgeBadge');
            if (bridgeBadge) {
                if (window.__MPS3_YT_BRIDGE__) {
                    bridgeBadge.textContent = '⚡ Userscript Direct';
                    bridgeBadge.style.color = '#10b981';
                } else {
                    bridgeBadge.textContent = 'Browser v3.0';
                }
            }
            document.getElementById('podModal').style.display = 'flex';
            loadPodSubscriptions();
        }

        function closePodModal() {
            if (isPodSyncing) {
                if (!confirm("Sincronização em andamento! Deseja cancelar?")) return;
                if (podSyncXhr) {
                    podSyncXhr.abort();
                    podSyncXhr = null;
                }
                if (podSyncAbortCtrl) {
                    podSyncAbortCtrl.abort();
                    podSyncAbortCtrl = null;
                }
                isPodSyncing = false;
            }
            document.getElementById('podModal').style.display = 'none';
        }

        document.getElementById('podBtn').onclick = openPodModal;
        document.getElementById('btnClosePod').onclick = closePodModal;
        document.getElementById('btnCancelPodSync').onclick = closePodModal;

        document.getElementById('btnPodToggleAddSub').onclick = () => {
            const box = document.getElementById('podAddSubBox');
            box.style.display = box.style.display === 'none' ? 'block' : 'none';
        };

        document.getElementById('btnPodToggleApiCfg').onclick = () => {
            const box = document.getElementById('podApiCfgBox');
            box.style.display = box.style.display === 'none' ? 'block' : 'none';
        };

        document.getElementById('btnPodSaveSubsSd').onclick = () => savePodSubscriptionsToSd(true);

        const btnSearchItunes = document.getElementById('btnPodSearchItunes');
        if (btnSearchItunes) {
            btnSearchItunes.onclick = async () => {
                const nameInp = document.getElementById('podNewSubName');
                const resBox = document.getElementById('podItunesResults');
                const q = (nameInp.value || '').trim();
                if (!q) {
                    alert('Digite o nome do podcast no campo acima (ex: NerdCast, Vórtex, Hora Boa) para buscar o Feed RSS oficial.');
                    return;
                }
                resBox.style.display = 'block';
                resBox.innerHTML = '<div style="color:var(--text-muted);padding:4px;">Buscando no diretório Apple Podcasts...</div>';
                try {
                    const items = await discoverOfficialRssFeeds(q, 5);
                    if (!items.length) {
                        resBox.innerHTML = '<div style="color:#dc2626;padding:4px;">Nenhum Feed RSS público encontrado para "' + q + '". (Alguns mesacasts como o Desce a Letra Show são exclusivos do YouTube).</div>';
                        return;
                    }
                    resBox.innerHTML = items.map((it, idx) =>
                        `<div class="itunes-rss-opt" data-idx="${idx}" style="padding:5px 7px;border-bottom:1px solid var(--border-color);cursor:pointer;display:flex;justify-content:space-between;align-items:center;gap:6px;">
                            <div style="overflow:hidden;text-overflow:ellipsis;white-space:nowrap;">
                                <strong>📡 ${it.name}</strong> <span style="color:var(--text-muted);">(${it.artist} • ${it.trackCount} eps)</span>
                            </div>
                            <span style="color:var(--accent-color);font-weight:bold;white-space:nowrap;">Usar RSS ↵</span>
                        </div>`
                    ).join('');
                    resBox.querySelectorAll('.itunes-rss-opt').forEach(row => {
                        row.onclick = () => {
                            const chosen = items[parseInt(row.dataset.idx, 10)];
                            document.getElementById('podNewSubType').value = 'rss';
                            document.getElementById('podNewSubName').value = sanitizePodSafeName(chosen.name, 40);
                            document.getElementById('podNewSubUrl').value = chosen.feedUrl;
                            resBox.style.display = 'none';
                        };
                    });
                } catch (e) {
                    resBox.innerHTML = `<div style="color:#dc2626;padding:4px;">Erro na busca: ${e.message}</div>`;
                }
            };
        }

        document.getElementById('btnPodConfirmAddSub').onclick = () => {
            const type = document.getElementById('podNewSubType').value;
            const name = sanitizePodSafeName(document.getElementById('podNewSubName').value.trim(), 40);
            const url = document.getElementById('podNewSubUrl').value.trim();
            if (!url) {
                alert('Informe a URL do canal YouTube ou do Feed RSS.');
                return;
            }
            podSubscriptions.push({
                id: 'sub_' + Date.now(),
                name: name || 'Podcast',
                type,
                url,
                enabled: true
            });
            document.getElementById('podNewSubName').value = '';
            document.getElementById('podNewSubUrl').value = '';
            document.getElementById('podAddSubBox').style.display = 'none';
            renderPodSubscriptions();
            savePodSubscriptionsToSd(true);
        };

        document.getElementById('btnTestPodServer').onclick = async () => {
            const srv = getPodServerUrl();
            const statusEl = document.getElementById('podServerStatus');
            if (!srv) {
                statusEl.style.color = '#dc2626';
                statusEl.textContent = 'Informe a URL do servidor (ex: http://100.64.0.1:8088)';
                return;
            }
            localStorage.setItem('mps3_pod_srv', srv);
            statusEl.style.color = '#64748b';
            statusEl.textContent = 'Testando conexão com ' + srv + '...';

            try {
                const res = await fetchWithTimeout(srv + '/api/status', {}, 12000);
                if (res.ok) {
                    const data = await res.json();
                    statusEl.style.color = '#16a34a';
                    statusEl.textContent = `✓ Conectado ao ${data.server || 'servidor'} (${data.storage_free_mb || 0} MB livres)`;
                } else {
                    statusEl.style.color = '#dc2626';
                    statusEl.textContent = 'Servidor respondeu com erro: ' + res.status;
                }
            } catch (err) {
                statusEl.style.color = '#dc2626';
                if (err.name === 'AbortError') {
                    statusEl.textContent = 'Timeout após 12s. Verifique se o app do Tailscale está conectado no celular.';
                } else {
                    statusEl.textContent = `Falha (${err.message || err.name || 'Erro de rede'}). Verifique se o Tailscale está ativo.`;
                }
            }
        };

        function updatePodSelectionSummary() {
            const selected = podPendingList.filter(ep => ep.selected);
            const totalBytes = selected.reduce((acc, ep) => acc + (ep.size_bytes || 0), 0);
            const mbTotal = (totalBytes / (1024 * 1024)).toFixed(1);

            document.getElementById('podSelectedSummary').textContent =
                `${selected.length} de ${podPendingList.length} selecionados (~${mbTotal} MB)`;

            const startBtn = document.getElementById('btnStartPodSync');
            if (selected.length > 0 || podPlannedDeletes.length > 0) {
                startBtn.disabled = false;
                startBtn.textContent = selected.length > 0
                    ? `Sincronizar ${selected.length} (~${mbTotal} MB)`
                    : `Limpar ${podPlannedDeletes.length} ouvido(s)`;
            } else {
                startBtn.disabled = true;
                startBtn.textContent = 'Nenhum selecionado';
            }
        }

        function renderPodList() {
            const listEl = document.getElementById('podList');
            const toolbar = document.getElementById('podSelectToolbar');
            const cleanBanner = document.getElementById('podCleanupBanner');
            const cleanText = document.getElementById('podCleanupText');

            if (cleanBanner && cleanText) {
                if (podPlannedDeletes.length > 0) {
                    const freeMb = (podPlannedDeletes.reduce((a, d) => a + (d.size || 0), 0) / (1024 * 1024)).toFixed(1);
                    cleanBanner.style.display = 'flex';
                    cleanText.textContent = `🧹 Retenção Top-N: ${podPlannedDeletes.length} episódio(s) já ouvido(s) no SD serão removidos (+${freeMb} MB).`;
                } else {
                    cleanBanner.style.display = 'none';
                }
            }

            if (!podPendingList.length) {
                listEl.innerHTML = '<div style="color:#16a34a;text-align:center;padding:12px;font-weight:bold;">🎉 Todos os episódios recentes dos seus canais já estão sincronizados ou já foram ouvidos!</div>';
                toolbar.style.display = 'none';
                updatePodSelectionSummary();
                return;
            }

            toolbar.style.display = 'flex';
            let html = '';
            podPendingList.forEach((ep, i) => {
                const mb = ((ep.size_bytes || 0) / (1024 * 1024)).toFixed(1);
                let dateStr = '';
                if (ep.mtime) {
                    const d = new Date(ep.mtime * 1000);
                    dateStr = d.toLocaleDateString('pt-BR', { day: '2-digit', month: '2-digit', year: '2-digit' });
                }
                const isChecked = ep.selected ? 'checked' : '';
                const srcBadge = ep.source_type === 'youtube' ? '▶ YT' : (ep.source_type === 'rss' ? '📡 RSS' : '🏠 SRV');
                html += `<label id="podItemRow_${i}" style="display:flex;align-items:center;gap:10px;padding:8px 10px;border-bottom:1px solid var(--border-color);cursor:pointer;user-select:none;transition:background 0.15s;background:${ep.selected ? 'var(--bg-subtle)' : 'var(--bg-card)'};border-radius:6px;margin-bottom:4px;">
                    <input type="checkbox" class="pod-check" data-idx="${i}" ${isChecked} style="width:20px;height:20px;accent-color:var(--accent-color);cursor:pointer;flex-shrink:0;">
                    <div style="flex:1;min-width:0;overflow:hidden;">
                        <div style="display:flex;align-items:center;gap:6px;flex-wrap:wrap;margin-bottom:2px;">
                            <span style="background:#0284c7;color:#fff;font-size:0.7em;font-weight:bold;padding:1px 6px;border-radius:4px;white-space:nowrap;">${ep.program}</span>
                            <span style="font-size:0.68em;color:var(--text-muted);border:1px solid var(--border-color);padding:0 5px;border-radius:4px;">${srcBadge}</span>
                            ${dateStr ? `<span style="color:var(--text-muted);font-size:0.75em;">📅 ${dateStr}</span>` : ''}
                        </div>
                        <div style="overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font-size:0.9em;color:var(--text-primary);" title="${ep.title}">
                            ${ep.title || ep.filename}
                        </div>
                    </div>
                    <span style="color:var(--text-muted);font-size:0.82em;font-weight:600;flex-shrink:0;">~${mb} MB</span>
                </label>`;
            });
            listEl.innerHTML = html;

            listEl.querySelectorAll('.pod-check').forEach(cb => {
                cb.onchange = () => {
                    const idx = parseInt(cb.dataset.idx, 10);
                    podPendingList[idx].selected = cb.checked;
                    const row = document.getElementById('podItemRow_' + idx);
                    if (row) row.style.background = cb.checked ? 'var(--bg-subtle)' : 'var(--bg-card)';
                    updatePodSelectionSummary();
                };
            });

            updatePodSelectionSummary();
        }

        document.getElementById('btnSelectRecent').onclick = () => {
            const seen = new Set();
            podPendingList.forEach(ep => {
                if (!seen.has(ep.program)) {
                    ep.selected = true;
                    seen.add(ep.program);
                } else {
                    ep.selected = false;
                }
            });
            renderPodList();
        };

        document.getElementById('btnSelectAll').onclick = () => {
            podPendingList.forEach(ep => ep.selected = true);
            renderPodList();
        };

        document.getElementById('btnSelectNone').onclick = () => {
            podPendingList.forEach(ep => ep.selected = false);
            renderPodList();
        };

        async function fetchMps3SyncManifest() {
            try {
                const res = await fetchWithTimeout('/api/sync/manifest', {}, 8000);
                if (res.ok) return await res.json();
            } catch (e) {}
            // Fallback para /api/podcasts_local caso esteja rodando em versão antiga
            try {
                const r2 = await fetchWithTimeout('/api/podcasts_local', {}, 8000);
                if (r2.ok) {
                    const d2 = await r2.json();
                    return {
                        free_bytes: 8 * 1024 * 1024 * 1024,
                        podcasts: d2.podcasts || [],
                        played_history: []
                    };
                }
            } catch (e) {}
            return { free_bytes: 8 * 1024 * 1024 * 1024, podcasts: [], played_history: [] };
        }

        document.getElementById('btnCheckPodcasts').onclick = async () => {
            const listEl = document.getElementById('podList');
            const countEl = document.getElementById('podPendingCount');
            const startBtn = document.getElementById('btnStartPodSync');
            const statusMsg = document.getElementById('podStatusMsg');
            const toolbar = document.getElementById('podSelectToolbar');

            const apiInput = document.getElementById('podCustomApiInstances');
            if (apiInput && apiInput.value.trim()) {
                localStorage.setItem('mps3_pod_api_pool', apiInput.value.trim());
            }

            listEl.innerHTML = '<div style="color:var(--text-muted);text-align:center;padding:14px;">Consultando manifesto do SD e raspando canais...</div>';
            toolbar.style.display = 'none';
            startBtn.disabled = true;
            statusMsg.textContent = '';

            try {
                const manifest = await fetchMps3SyncManifest();
                const keepRecent = parseInt((document.getElementById('podKeepRecentSelect') || {}).value || '3', 10);
                const autoClean = !document.getElementById('podAutoCleanPlayed') || document.getElementById('podAutoCleanPlayed').checked;

                let scrapedCandidates = [];

                if (podSourceMode === 'browser') {
                    const activeSubs = podSubscriptions.filter(s => s.enabled !== false);
                    if (!activeSubs.length) {
                        throw new Error('Ative ou adicione pelo menos 1 canal YouTube ou Feed RSS.');
                    }
                    const errors = [];
                    listEl.innerHTML = `<div style="color:var(--text-muted);text-align:center;padding:14px;">Raspando ${activeSubs.length} canal(is) em paralelo...</div>`;
                    const scrapePromises = activeSubs.map(async (sub) => {
                        try {
                            if (sub.type === 'rss') {
                                let xml = '';
                                try {
                                    const res = await fetchWithTimeout(sub.url, {}, 12000);
                                    if (!res.ok) throw new Error('HTTP ' + res.status);
                                    xml = await res.text();
                                } catch (rssCorsErr) {
                                    const relRes = await mps3InnertubeRelay({ url: sub.url, method: 'GET' }, null, 15000);
                                    xml = await relRes.text();
                                }
                                return parsePodcastRssXml(xml, sub.name, keepRecent + 3);
                            } else {
                                return await scrapeYoutubeSubscription(sub, keepRecent + 3);
                            }
                        } catch (subErr) {
                            console.warn(`Aviso ao raspar ${sub.name}:`, subErr);
                            errors.push(`${sub.name}: ${subErr.message}`);
                            return [];
                        }
                    });
                    const scrapeResults = await Promise.allSettled(scrapePromises);
                    for (const r of scrapeResults) {
                        if (r.status === 'fulfilled' && Array.isArray(r.value)) {
                            scrapedCandidates.push(...r.value);
                        }
                    }
                    if (!scrapedCandidates.length && errors.length > 0) {
                        throw new Error(errors.join(' | '));
                    }
                } else {
                    const srv = getPodServerUrl();
                    localStorage.setItem('mps3_pod_srv', srv);
                    const srvRes = await fetchWithTimeout(srv + '/api/podcasts?limit=60', {}, 15000);
                    if (!srvRes.ok) throw new Error('Servidor retornou HTTP ' + srvRes.status);
                    const srvData = await srvRes.json();
                    scrapedCandidates = (srvData.podcasts || []).map(rp => ({
                        ...rp,
                        source_type: 'server',
                        direct_url: `${srv}/podcasts/${encodeURI(rp.rel_path)}`
                    }));
                }

                // Executa o BrowserSyncPlanner (mesma lógica do planner.py)
                const plan = BrowserSyncPlanner.computeSyncPlan(scrapedCandidates, manifest, {
                    keepRecent,
                    autoCleanPlayed: autoClean
                });

                podPendingList = plan.downloads;
                podPlannedDeletes = plan.deletes;

                countEl.textContent = podPendingList.length;
                renderPodList();
                statusMsg.textContent = `${podPendingList.length} novo(s), ${plan.skippedPlayed.length} já ouvido(s) ignorado(s).`;

            } catch (err) {
                listEl.innerHTML = `<div style="color:#dc2626;padding:12px;text-align:center;">Erro ao planejar sincronia: ${err.message}</div>`;
            }
        };

        document.getElementById('btnStartPodSync').onclick = async () => {
            const toDownload = podPendingList.filter(ep => ep.selected);
            if ((!toDownload.length && !podPlannedDeletes.length) || isPodSyncing) return;

            const totalCount = toDownload.length;
            const batchTotal = toDownload.reduce((acc, ep) => acc + (ep.size_bytes || 0), 0);
            const forceReencode = (document.getElementById('podAudioFormatMode') || {}).value === 'mp3_wav';

            isPodSyncing = true;
            document.getElementById('btnStartPodSync').disabled = true;
            document.getElementById('btnCheckPodcasts').disabled = true;
            document.getElementById('podProgressBox').style.display = 'block';

            let successCount = 0;
            let deletedCount = 0;

            try {
                // 1. Inicia sessão transacional no ESP32-S3 (pausa áudio, limpa /Podcasts/.staging e exibe no OLED)
                document.getElementById('podCurrentItem').textContent = 'Preparando cartão SD (/api/sync/begin)...';
                await fetch('/api/sync/begin', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ total_files: totalCount, total_bytes: batchTotal })
                }).catch(() => {});

                // 2. Remove episódios antigos já ouvidos fora do Top N
                for (const delItem of podPlannedDeletes) {
                    if (!isPodSyncing) break;
                    document.getElementById('podCurrentItem').textContent = `🧹 Removendo ouvido: ${delItem.filename}`;
                    try {
                        const rDel = await fetch('/api/delete', {
                            method: 'POST',
                            headers: { 'Content-Type': 'application/json' },
                            body: JSON.stringify({ path: delItem.path })
                        });
                        if (rDel.ok) deletedCount++;
                    } catch (e) {}
                }

                // 3. Processa cada episódio selecionado: Stream -> (Transcode opcional) -> SHA-256 -> .staging -> Verify -> Move
                for (let i = 0; i < totalCount; i++) {
                    if (!isPodSyncing) break;

                    const ep = toDownload[i];
                    const origIdx = podPendingList.indexOf(ep);
                    const rowEl = document.getElementById('podItemRow_' + origIdx);
                    if (rowEl) rowEl.style.background = 'rgba(2, 132, 199, 0.15)';

                    document.getElementById('podItemBar').style.width = '0%';
                    document.getElementById('podItemPct').textContent = '0%';
                    document.getElementById('podOverallInfo').textContent = `Episódio ${i + 1} de ${totalCount}`;

                    try {
                        // Passo 3.1: Resolve URL de stream de áudio (YouTube ou RSS/Servidor direto)
                        let mediaUrl = ep.direct_url;
                        let ext = ep.ext || 'm4a';
                        let resolvedContentLength = ep.size_bytes || 0;
                        let resolvedUserAgent = '';
                        if (ep.source_type === 'youtube' && !mediaUrl) {
                            document.getElementById('podCurrentItem').textContent = `[${i + 1}/${totalCount}] Extraindo stream YT: ${ep.title}`;
                            const resolved = await resolveYoutubeAudioStream(ep);
                            mediaUrl = resolved.url;
                            ext = resolved.ext || ext;
                            if (resolved.contentLength) resolvedContentLength = resolved.contentLength;
                            if (resolved.userAgent) resolvedUserAgent = resolved.userAgent;
                        }

                        const safeProgram = sanitizePodSafeName(ep.program || 'Podcasts', 48);
                        let finalFilename = buildPodEpisodeFilename(ep, ext);
                        let stagingPath = `/Podcasts/.staging/${finalFilename}`;
                        let finalDestPath = `/Podcasts/${safeProgram}/${finalFilename}`;

                        const isNoCorsCdn = /googlevideo\.com|cloudfront\.net|anchor\.fm/i.test(String(mediaUrl));
                        let pulledDirectlyOnMps3 = false;

                        if (!forceReencode && isNoCorsCdn && !(window.__MPS3_YT_BRIDGE__ && typeof window.__MPS3_YT_BRIDGE__.downloadAudioBytes === 'function')) {
                            document.getElementById('podCurrentItem').textContent = `[${i + 1}/${totalCount}] Baixando direto p/ SD: ${ep.title}`;
                            try {
                                await pullEpisodeDirectOnMps3(mediaUrl, stagingPath, resolvedContentLength, resolvedUserAgent, i + 1, totalCount, batchTotal, ep.title);
                            } catch (pullErr) {
                                if (ep.source_type === 'youtube' && String(pullErr && pullErr.message).includes('403')) {
                                    window.__mps3YtVisitorData = '';
                                    document.getElementById('podCurrentItem').textContent = `[${i + 1}/${totalCount}] Renovando token YT (403)...`;
                                    const fresh = await resolveYoutubeAudioStream({ ...ep, direct_url: null });
                                    mediaUrl = fresh.url;
                                    if (fresh.contentLength) resolvedContentLength = fresh.contentLength;
                                    if (fresh.userAgent) resolvedUserAgent = fresh.userAgent;
                                    await pullEpisodeDirectOnMps3(mediaUrl, stagingPath, resolvedContentLength, resolvedUserAgent, i + 1, totalCount, batchTotal, ep.title);
                                } else {
                                    throw pullErr;
                                }
                            }
                            pulledDirectlyOnMps3 = true;
                        }

                        if (!pulledDirectlyOnMps3) {
                            // Passo 3.2: Baixa o stream de áudio para a memória do navegador (ou fallback Pull Direto no MPS3 se bloquear CORS)
                            document.getElementById('podCurrentItem').textContent = `[${i + 1}/${totalCount}] Baixando áudio: ${ep.title}`;
                            let audioBytes;
                            if (window.__MPS3_YT_BRIDGE__ && typeof window.__MPS3_YT_BRIDGE__.downloadAudioBytes === 'function' && String(mediaUrl).includes('googlevideo.com')) {
                                audioBytes = await window.__MPS3_YT_BRIDGE__.downloadAudioBytes(mediaUrl, resolvedContentLength);
                            } else {
                                let resp;
                                try {
                                    resp = await fetch(mediaUrl);
                                } catch (corsErr) {
                                    if (window.__MPS3_YT_BRIDGE__ && typeof window.__MPS3_YT_BRIDGE__.downloadAudioBytes === 'function') {
                                        audioBytes = await window.__MPS3_YT_BRIDGE__.downloadAudioBytes(mediaUrl, 0);
                                    } else if (!forceReencode) {
                                        document.getElementById('podCurrentItem').textContent = `[${i + 1}/${totalCount}] Baixando direto p/ SD: ${ep.title}`;
                                        await pullEpisodeDirectOnMps3(mediaUrl, stagingPath, resolvedContentLength, resolvedUserAgent, i + 1, totalCount, batchTotal, ep.title);
                                        pulledDirectlyOnMps3 = true;
                                    } else {
                                        resp = await fetch('/api/dev/rss_proxy?url=' + encodeURIComponent(mediaUrl));
                                        if (resp.status === 404 || resp.status === 405) {
                                            throw new Error('CDN bloqueou CORS direto no navegador');
                                        }
                                    }
                                }
                                if (!pulledDirectlyOnMps3 && !audioBytes) {
                                    if (!resp.ok) throw new Error(`Falha no download (HTTP ${resp.status})`);
                                    const clenHeader = parseInt((resp.headers && resp.headers.get && resp.headers.get('Content-Length')) || '0', 10) || resolvedContentLength;
                                    if (resp.body && typeof resp.body.getReader === 'function') {
                                        const reader = resp.body.getReader();
                                        const chunks = [];
                                        let rxBytes = 0;
                                        const tStart = performance.now();
                                        while (true) {
                                            const { done, value } = await reader.read();
                                            if (done) break;
                                            if (value) {
                                                chunks.push(value);
                                                rxBytes += value.byteLength;
                                                if (clenHeader > 0) {
                                                    const dlPct = Math.min(99, Math.round((rxBytes * 100) / clenHeader));
                                                    document.getElementById('podItemBar').style.width = dlPct + '%';
                                                    document.getElementById('podItemPct').textContent = dlPct + '%';
                                                }
                                                const dtSec = Math.max(0.1, (performance.now() - tStart) / 1000);
                                                const mbps = (rxBytes / (1024 * 1024)) / dtSec;
                                                document.getElementById('podSpeedInfo').textContent = `${(rxBytes / (1024 * 1024)).toFixed(1)} MB (${mbps.toFixed(1)} MB/s)`;
                                            }
                                        }
                                        audioBytes = new Uint8Array(rxBytes);
                                        let off = 0;
                                        for (const c of chunks) {
                                            audioBytes.set(c, off);
                                            off += c.byteLength;
                                        }
                                    } else {
                                        const rawBuf = await resp.arrayBuffer();
                                        audioBytes = new Uint8Array(rawBuf);
                                    }
                                }
                            }

                            if (!pulledDirectlyOnMps3) {
                                // Passo 3.3: Conversão de áudio opcional no navegador
                                const processed = await processEpisodeAudioBytes(audioBytes, ext, forceReencode);
                                audioBytes = processed.bytes;
                                ext = processed.ext;

                                finalFilename = buildPodEpisodeFilename(ep, ext);
                                stagingPath = `/Podcasts/.staging/${finalFilename}`;
                                finalDestPath = `/Podcasts/${safeProgram}/${finalFilename}`;

                                // Passo 3.4: Calcula SHA-256 nativo (WebCrypto Subtle) com fallback para pure-JS
                                document.getElementById('podCurrentItem').textContent = `[${i + 1}/${totalCount}] Calculando SHA-256...`;
                                const expectedSha256 = await computeSha256(audioBytes);
                                const uploadBlob = new Blob([audioBytes], { type: 'application/octet-stream' });

                                // Passo 3.5: Upload para /Podcasts/.staging/<arquivo> no MPS3 (validação SHA-256 on-the-fly via header)
                                document.getElementById('podCurrentItem').textContent = `[${i + 1}/${totalCount}] Gravando no SD: ${finalFilename}`;
                                await new Promise((resolve, reject) => {
                                    podSyncXhr = new XMLHttpRequest();
                                    const uploadUrl = `/api/upload?path=${encodeURIComponent(stagingPath)}&idx=${i + 1}&count=${totalCount}&batchTotal=${batchTotal}&title=${encodeURIComponent(ep.title || '')}`;
                                    podSyncXhr.open('PUT', uploadUrl, true);
                                    podSyncXhr.setRequestHeader('X-Expected-SHA256', expectedSha256);
                                    podSyncXhr.timeout = 600000;

                                    let lastT = performance.now(), lastB = 0;
                                    podSyncXhr.upload.onprogress = (ev) => {
                                        if (!ev.lengthComputable) return;
                                        const now = performance.now();
                                        const pct = Math.round((ev.loaded * 100) / ev.total);
                                        const dt = (now - lastT) / 1000;
                                        const db = ev.loaded - lastB;
                                        const kbps = dt > 0.05 ? (db / 1024) / dt : 0;
                                        lastB = ev.loaded; lastT = now;

                                        document.getElementById('podItemBar').style.width = pct + '%';
                                        document.getElementById('podItemPct').textContent = pct + '%';
                                        document.getElementById('podSpeedInfo').textContent = kbps >= 1024 ? (kbps / 1024).toFixed(1) + ' MB/s' : Math.round(kbps) + ' KB/s';
                                    };

                                    podSyncXhr.onload = () => {
                                        if (podSyncXhr.status >= 200 && podSyncXhr.status < 300) {
                                            const retSha = (typeof podSyncXhr.getResponseHeader === 'function' ? (podSyncXhr.getResponseHeader('X-Content-SHA256') || '') : '').trim().toLowerCase();
                                            if (expectedSha256 && retSha && retSha !== expectedSha256.toLowerCase()) {
                                                reject(new Error(`SHA-256 mismatch no chip: retornado=${retSha}, esperado=${expectedSha256}`));
                                                return;
                                            }
                                            resolve();
                                        } else {
                                            let detail = 'HTTP ' + podSyncXhr.status;
                                            try {
                                                const ej = JSON.parse(podSyncXhr.responseText || '{}');
                                                if (ej.error) detail += ' (' + ej.error + ')';
                                            } catch (e) {
                                                if (podSyncXhr.responseText) detail += ': ' + String(podSyncXhr.responseText).slice(0, 60);
                                            }
                                            reject(new Error('Upload ' + detail));
                                        }
                                    };
                                    podSyncXhr.onerror = () => reject(new Error('Falha de rede no upload para o MPS3'));
                                    podSyncXhr.ontimeout = () => reject(new Error('Timeout de envio para o SD'));
                                    podSyncXhr.send(uploadBlob);
                                });

                                // Passo 3.6: Integridade garantida on-the-fly pelo firmware C no upload via X-Expected-SHA256
                                if (!expectedSha256) {
                                    document.getElementById('podCurrentItem').textContent = `[${i + 1}/${totalCount}] Verificando integridade no SD...`;
                                    const verRes = await fetchWithTimeout('/api/sync/verify', {
                                        method: 'POST',
                                        headers: { 'Content-Type': 'application/json' },
                                        body: JSON.stringify({
                                            path: stagingPath,
                                            expected_size: audioBytes.byteLength
                                        })
                                    }, 180000);
                                    if (verRes.status !== 404) {
                                        let verJson = {};
                                        try { verJson = await verRes.json(); } catch (e) {}
                                        if (!verRes.ok || verJson.verified === false) {
                                            throw new Error('Integridade SD falhou (' + (verJson.error || ('HTTP ' + verRes.status)) + ')');
                                        }
                                    }
                                }
                            }
                        }

                        // Passo 3.7: Promoção Atômica de /Podcasts/.staging para /Podcasts/<Canal>/<Arquivo> via JSON (contrato firmware C + simulador)
                        document.getElementById('podCurrentItem').textContent = `[${i + 1}/${totalCount}] Movendo para /Podcasts/${safeProgram}/...`;
                        const mvRes = await fetchWithTimeout('/api/move', {
                            method: 'POST',
                            headers: { 'Content-Type': 'application/json' },
                            body: JSON.stringify({
                                old_path: stagingPath,
                                new_path: finalDestPath,
                                src: stagingPath,
                                dst: finalDestPath
                            })
                        }, 30000);
                        if (!mvRes.ok) {
                            let mvDetail = 'HTTP ' + mvRes.status;
                            try {
                                const mj = await mvRes.json();
                                if (mj && mj.error) mvDetail = mj.error;
                            } catch (e) {}
                            throw new Error('Falha ao mover de .staging (' + mvDetail + ')');
                        }

                        successCount++;
                        ep.selected = false;
                        if (rowEl) {
                            const cb = rowEl.querySelector('.pod-check');
                            if (cb) cb.checked = false;
                            rowEl.style.background = 'rgba(22, 163, 74, 0.15)';
                            const badge = document.createElement('span');
                            badge.style.color = '#16a34a';
                            badge.style.fontWeight = 'bold';
                            badge.style.marginLeft = '6px';
                            badge.textContent = '✓ SHA-256 OK';
                            rowEl.appendChild(badge);
                        }

                    } catch (err) {
                        console.error('Erro na transferência do podcast:', err);
                        const reason = (err && err.message) ? err.message : 'erro desconhecido';
                        const isNetErr = reason.includes('Falha de rede') || reason.includes('Timeout') || reason.includes('fetch');
                        if (isNetErr && !ep._retried) {
                            ep._retried = true;
                            i--; // Repete o mesmo episódio
                            const waitMsg = `[Reconectando Wi-Fi...] Aguardando 4s antes de tentar novamente...`;
                            const currEl = document.getElementById('podCurrentItem');
                            if (currEl) currEl.textContent = waitMsg;
                            await new Promise(r => setTimeout(r, 4000));
                            continue;
                        }
                        if (rowEl) {
                            rowEl.style.background = 'rgba(220, 38, 38, 0.15)';
                            const badge = document.createElement('span');
                            badge.style.color = '#dc2626';
                            badge.style.fontWeight = 'bold';
                            badge.style.fontSize = '0.8em';
                            badge.style.marginLeft = '6px';
                            badge.title = reason;
                            badge.textContent = '✗ ' + (reason.length > 42 ? reason.slice(0, 42) + '…' : reason);
                            rowEl.appendChild(badge);
                        }
                    }
                }
            } finally {
                // 4. Finaliza sessão de sincronia no ESP32-S3 (/api/sync/done)
                await fetch('/api/sync/done', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ downloaded: successCount, deleted: deletedCount })
                }).catch(() => {});

                isPodSyncing = false;
                podSyncXhr = null;
                podPlannedDeletes = [];
                document.getElementById('podProgressBox').style.display = 'none';
                document.getElementById('btnCheckPodcasts').disabled = false;
                updatePodSelectionSummary();
                document.getElementById('podStatusMsg').textContent =
                    `Concluído: ${successCount}/${totalCount} baixados` + (deletedCount ? `, ${deletedCount} limpos.` : '.');
                fetchStatus();
                load();
            }
        };

        fetchStatus(); setInterval(fetchStatus, 8000); load();
