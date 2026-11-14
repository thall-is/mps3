// ==UserScript==
// @name         MPS3 YouTube Direct Scraper Bridge (Zero-CORS v3.1)
// @namespace    https://mps3.local/
// @version      3.1.0
// @description  Ponte para a Central Web do MPS3: raspa feeds Atom do YouTube, resolve VISITOR_DATA + InnerTube ANDROID (20.03.35) e baixa streams M4A/Opus diretamente pelo navegador sem restrições de CORS.
// @author       MPS3 Engineering
// @match        http://mps3.local/*
// @match        http://192.168.*.*/*
// @match        http://10.*.*.*/*
// @match        http://127.0.0.1:*/*
// @match        http://localhost:*/*
// @match        https://thall-is.github.io/*
// @grant        GM_xmlhttpRequest
// @grant        unsafeWindow
// @connect      www.youtube.com
// @connect      youtube.com
// @connect      googlevideo.com
// @connect      *
// @run-at       document-start
// ==/UserScript==

(function () {
    'use strict';

    const ANDROID_UA = 'com.google.android.youtube/20.03.35 (Linux; U; Android 14) gzip';

    function gmFetchText(url, options = {}) {
        return new Promise((resolve, reject) => {
            GM_xmlhttpRequest({
                method: options.method || 'GET',
                url: url,
                headers: options.headers || {},
                data: options.body || null,
                timeout: options.timeout || 15000,
                onload: (res) => {
                    if (res.status >= 200 && res.status < 300) {
                        resolve(res.responseText);
                    } else {
                        reject(new Error('HTTP ' + res.status));
                    }
                },
                onerror: () => reject(new Error('Erro de rede GM_xmlhttpRequest')),
                ontimeout: () => reject(new Error('Timeout GM_xmlhttpRequest'))
            });
        });
    }

    function gmFetchArrayBuffer(url, options = {}) {
        return new Promise((resolve, reject) => {
            GM_xmlhttpRequest({
                method: 'GET',
                url: url,
                headers: options.headers || { 'User-Agent': ANDROID_UA },
                responseType: 'arraybuffer',
                timeout: options.timeout || 180000,
                onload: (res) => {
                    if (res.status >= 200 && res.status < 300 && res.response) {
                        resolve(new Uint8Array(res.response));
                    } else {
                        reject(new Error('HTTP ' + res.status));
                    }
                },
                onerror: () => reject(new Error('Erro no download do stream via GM_xmlhttpRequest')),
                ontimeout: () => reject(new Error('Timeout no download do stream'))
            });
        });
    }

    const bridge = {
        version: '3.1.0',

        async resolveChannelId(urlOrHandle) {
            if (/^UC[A-Za-z0-9_-]{20,}$/.test(urlOrHandle)) return urlOrHandle;
            const ucMatch = String(urlOrHandle).match(/channel\/(UC[A-Za-z0-9_-]{20,})/);
            if (ucMatch) return ucMatch[1];

            const targetUrl = String(urlOrHandle).startsWith('http')
                ? urlOrHandle
                : `https://www.youtube.com/${String(urlOrHandle).replace(/^\/+/, '')}`;
            const pageHtml = await gmFetchText(targetUrl);
            const canonicalMatch =
                pageHtml.match(/<link rel="canonical" href="https:\/\/www\.youtube\.com\/channel\/(UC[A-Za-z0-9_-]{20,})"/) ||
                pageHtml.match(/"channelId"\s*:\s*"(UC[A-Za-z0-9_-]{20,})"/) ||
                pageHtml.match(/channel_id=(UC[A-Za-z0-9_-]{20,})/);
            if (canonicalMatch) return canonicalMatch[1];
            throw new Error('Não foi possível resolver o channel_id para: ' + urlOrHandle);
        },

        async fetchChannelVideos(sub, maxItems = 6) {
            const targetWin = typeof unsafeWindow !== 'undefined' ? unsafeWindow : window;
            const extractor = targetWin.MPS3_YT_EXTRACTOR;
            const target = extractor
                ? extractor.parseSubscriptionTarget(sub)
                : { channelRef: sub.channel_id || '', tabParamsOrder: ['EgdzdHJlYW1z8gYECgJ6AA==', 'EgZ2aWRlb3PyBgQKAjoA'] };

            const channelId = /^UC[A-Za-z0-9_-]{20,}$/.test(target.channelRef)
                ? target.channelRef
                : await this.resolveChannelId(sub.url || target.channelRef);

            for (const tabParams of target.tabParamsOrder) {
                try {
                    const body = JSON.stringify({
                        browseId: channelId,
                        params: tabParams,
                        context: {
                            client: extractor ? extractor.CONFIG.browseClient : {
                                clientName: 'WEB',
                                clientVersion: '2.20250101.00.00',
                                hl: 'pt',
                                gl: 'BR'
                            }
                        }
                    });
                    const respTxt = await gmFetchText('https://www.youtube.com/youtubei/v1/browse?prettyPrint=false', {
                        method: 'POST',
                        headers: { 'Content-Type': 'application/json' },
                        body: body
                    });
                    const bdata = JSON.parse(respTxt);
                    if (extractor && typeof extractor.extractVideosFromBrowseTree === 'function') {
                        const items = extractor.extractVideosFromBrowseTree(bdata, sub.name, maxItems);
                        if (items.length > 0) return items;
                    }
                } catch (e) {}
            }
            return [];
        },

        async resolveAudioStream(videoId) {
            const targetWin = typeof unsafeWindow !== 'undefined' ? unsafeWindow : window;
            const extractor = targetWin.MPS3_YT_EXTRACTOR;

            // 1. Obtém VISITOR_DATA atualizado da página de watch
            const watchHtml = await gmFetchText(`https://www.youtube.com/watch?v=${encodeURIComponent(videoId)}`);
            const visMatch = watchHtml.match(/"VISITOR_DATA"\s*:\s*"([^"]+)"/);
            const visitorData = visMatch ? visMatch[1] : '';

            // 2. Chama InnerTube /youtubei/v1/player com perfil VISIONOS 1.02 (sem bloqueio GVS PoToken)
            const visionClient = {
                clientName: 'VISIONOS',
                clientVersion: '1.02',
                deviceMake: 'Apple',
                deviceModel: 'RealityDevice17,1',
                userAgent: 'Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15',
                osName: 'visionOS',
                osVersion: '26.5.23O471',
                hl: 'pt',
                gl: 'BR',
                visitorData: visitorData
            };
            const body = JSON.stringify({
                videoId: videoId,
                contentCheckOk: true,
                racyCheckOk: true,
                context: { client: visionClient }
            });
            const respTxt = await gmFetchText('https://www.youtube.com/youtubei/v1/player?prettyPrint=false', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json',
                    'User-Agent': visionClient.userAgent,
                    'X-YouTube-Client-Name': '101',
                    'X-YouTube-Client-Version': '1.02',
                    'X-Goog-Visitor-Id': visitorData,
                    'Origin': 'https://www.youtube.com'
                },
                body: body
            });
            const data = JSON.parse(respTxt);
            if (extractor && typeof extractor.selectBestAudioFormat === 'function') {
                const chosen = extractor.selectBestAudioFormat(data);
                if (chosen) return chosen;
            }
            const formats = (data.streamingData && data.streamingData.adaptiveFormats) || [];
            const audioFormats = formats.filter(f => String(f.mimeType || '').startsWith('audio/') && f.url && !f.isDrc);
            if (!audioFormats.length) throw new Error('InnerTube não retornou stream direto.');
            const m4a = audioFormats.find(f => f.itag === 139 || f.itag === 140 || String(f.mimeType).includes('mp4')) || audioFormats[0];
            return {
                url: m4a.url,
                ext: String(m4a.mimeType).includes('mp4') ? 'm4a' : 'webm',
                mime: m4a.mimeType,
                contentLength: parseInt(m4a.contentLength || '0', 10) || 0
            };
        },

        async downloadAudioBytes(url, contentLength = 0) {
            const targetWin = typeof unsafeWindow !== 'undefined' ? unsafeWindow : window;
            const extractor = targetWin.MPS3_YT_EXTRACTOR;
            const ua = 'Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15';

            if (contentLength > 0 && extractor && typeof extractor.buildFragmentUrls === 'function') {
                const fragUrls = extractor.buildFragmentUrls(url, contentLength);
                const parts = [];
                let totalLen = 0;
                for (const fUrl of fragUrls) {
                    const part = await gmFetchArrayBuffer(fUrl, { headers: { 'User-Agent': ua } });
                    parts.push(part);
                    totalLen += part.byteLength;
                }
                const merged = new Uint8Array(totalLen);
                let offset = 0;
                for (const p of parts) {
                    merged.set(p, offset);
                    offset += p.byteLength;
                }
                return merged;
            }
            return await gmFetchArrayBuffer(url, { headers: { 'User-Agent': ua } });
        }
    };

    const targetWin = typeof unsafeWindow !== 'undefined' ? unsafeWindow : window;
    targetWin.__MPS3_YT_BRIDGE__ = bridge;
})();
