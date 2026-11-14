/* ==========================================================================
 * MÓDULO JS 10: MPS3 Wrapped, Motor Analítico, Canvas Social HD & Ponte Mobile
 * ========================================================================== */

        const WRAPPED_STATE = {
            rawEvents: [],
            rawSummary: null,
            hasRealData: false,
            useDemo: false,
            period: 'all',          // 'all' | 'year' | '30d' | '7d'
            category: 'all',        // 'all' | 'music' | 'podcast'
            format: 'story',        // 'story' (1080x1920) | 'feed' (1080x1350) | 'square' (1080x1080)
            cardTheme: 'oled_blue', // 'oled_white' | 'oled_blue' | 'oled_yellow' | 'oled_green' | 'oled_dual' | 'cyber' | 'gold'
            template: 'festival', // 'festival' | 'oled_matrix' | 'albums_era' | 'summary' | 'top10'
            stats: null,
            deviceIp: '',
            wifiMode: 'STA',
            lastBlob: null
        };

        const WRP_WDAYS_SHORT = ['Dom', 'Seg', 'Ter', 'Qua', 'Qui', 'Sex', 'Sáb'];
        const WRP_WDAYS_FULL  = ['Domingo', 'Segunda-feira', 'Terça-feira', 'Quarta-feira', 'Quinta-feira', 'Sexta-feira', 'Sábado'];

        // --- 1. GERADOR QR CODE ISO/IEC 18004 PURO EM JS (V1..V9, ECC-L, 100% OFFLINE) ---
        function encodeQrMatrix(text) {
            const bytes = Array.from(new TextEncoder().encode(String(text || '')));
            // Tabela ECC-L para versões 1..9: [totalCodewords, ecCodewordsPerBlock, numBlocks, alignCenter]
            const VTAB = [
                null,
                [26,  7, 1, 0],
                [44, 10, 1, 18],
                [70, 15, 1, 22],
                [100, 20, 1, 26],
                [134, 26, 1, 30],
                [172, 18, 2, 34],
                [196, 20, 2, 22], // V7 tem [6, 22, 38]
                [242, 24, 2, 24], // V8 tem [6, 24, 42]
                [292, 30, 2, 26]  // V9 tem [6, 26, 46]
            ];
            let ver = 0;
            for (let v = 1; v <= 9; v++) {
                const dataCap = VTAB[v][0] - VTAB[v][1] * VTAB[v][2];
                if (bytes.length + 2 <= dataCap) { ver = v; break; }
            }
            if (!ver) return null;

            const [totalCw, ecLen, numBlocks] = VTAB[ver];
            const dataCwLen = totalCw - ecLen * numBlocks;

            // Bitstream Modo Byte (0100) + 8-bit length + bytes + terminador + pad 0xEC/0x11
            const bits = [];
            const pushBits = (val, len) => {
                for (let i = len - 1; i >= 0; i--) bits.push((val >>> i) & 1);
            };
            pushBits(0b0100, 4);
            pushBits(bytes.length, 8);
            for (const b of bytes) pushBits(b, 8);
            const maxDataBits = dataCwLen * 8;
            for (let i = 0; i < 4 && bits.length < maxDataBits; i++) bits.push(0);
            while (bits.length % 8 !== 0) bits.push(0);

            const dataCw = [];
            for (let i = 0; i < bits.length; i += 8) {
                let b = 0;
                for (let j = 0; j < 8; j++) b = (b << 1) | bits[i + j];
                dataCw.push(b);
            }
            let padToggle = true;
            while (dataCw.length < dataCwLen) {
                dataCw.push(padToggle ? 0xEC : 0x11);
                padToggle = !padToggle;
            }

            // Reed-Solomon GF(256) idêntico a qrcode.c (polinômio 0x11D)
            const rsMul = (a, b) => {
                let z = 0;
                for (let i = 7; i >= 0; i--) {
                    z = (z << 1) ^ ((z >>> 7) * 0x11D);
                    z ^= ((b >>> i) & 1) * a;
                }
                return z & 0xFF;
            };

            const coeff = new Uint8Array(ecLen);
            coeff[ecLen - 1] = 1;
            let root = 1;
            for (let i = 0; i < ecLen; i++) {
                for (let j = 0; j < ecLen; j++) {
                    coeff[j] = rsMul(coeff[j], root);
                    if (j + 1 < ecLen) coeff[j] ^= coeff[j + 1];
                }
                root = (root << 1) ^ ((root >>> 7) * 0x11D);
            }

            const rsEncode = (msg) => {
                const rem = new Uint8Array(ecLen);
                for (const d of msg) {
                    const factor = d ^ rem[0];
                    for (let j = 1; j < ecLen; j++) rem[j - 1] = rem[j];
                    rem[ecLen - 1] = 0;
                    for (let j = 0; j < ecLen; j++) rem[j] ^= rsMul(coeff[j], factor);
                }
                return Array.from(rem);
            };

            const dataBlocks = [];
            const ecBlocks = [];
            const blockLen = Math.floor(dataCwLen / numBlocks);
            let offset = 0;
            for (let b = 0; b < numBlocks; b++) {
                const len = (b === numBlocks - 1) ? (dataCwLen - offset) : blockLen;
                const blk = dataCw.slice(offset, offset + len);
                offset += len;
                dataBlocks.push(blk);
                ecBlocks.push(rsEncode(blk));
            }

            const finalCw = [];
            const maxBlkLen = Math.max(...dataBlocks.map(b => b.length));
            for (let i = 0; i < maxBlkLen; i++) {
                for (let b = 0; b < numBlocks; b++) {
                    if (i < dataBlocks[b].length) finalCw.push(dataBlocks[b][i]);
                }
            }
            for (let i = 0; i < ecLen; i++) {
                for (let b = 0; b < numBlocks; b++) finalCw.push(ecBlocks[b][i]);
            }

            // Construção da grade N x N
            const size = ver * 4 + 17;
            const mod  = Array.from({ length: size }, () => new Uint8Array(size));
            const isFn = Array.from({ length: size }, () => new Uint8Array(size));

            const setFn = (cx, cy, dark) => {
                if (cx >= 0 && cy >= 0 && cx < size && cy < size) {
                    mod[cy][cx] = dark ? 1 : 0;
                    isFn[cy][cx] = 1;
                }
            };

            const drawFinder = (ox, oy) => {
                for (let dy = -1; dy <= 7; dy++) {
                    for (let dx = -1; dx <= 7; dx++) {
                        const dist = Math.max(Math.abs(dx - 3), Math.abs(dy - 3));
                        const dark = (dx >= 0 && dx <= 6 && dy >= 0 && dy <= 6) && (dist !== 2);
                        setFn(ox + dx, oy + dy, dark);
                    }
                }
            };
            drawFinder(0, 0);
            drawFinder(size - 7, 0);
            drawFinder(0, size - 7);

            for (let i = 8; i < size - 8; i++) {
                setFn(i, 6, i % 2 === 0);
                setFn(6, i, i % 2 === 0);
            }

            if (ver >= 2) {
                const coords = (ver <= 6)
                    ? [6, size - 7]
                    : [6, VTAB[ver][3], size - 7];
                for (const ay of coords) {
                    for (const ax of coords) {
                        if ((ax === 6 && ay === 6) || (ax === size - 7 && ay === 6) || (ax === 6 && ay === size - 7)) continue;
                        for (let dy = -2; dy <= 2; dy++) {
                            for (let dx = -2; dx <= 2; dx++) {
                                setFn(ax + dx, ay + dy, Math.max(Math.abs(dx), Math.abs(dy)) !== 1);
                            }
                        }
                    }
                }
            }

            // Reserva bits de formato + dark module
            for (let i = 0; i <= 8; i++) {
                if (i !== 6) { isFn[8][i] = 1; isFn[i][8] = 1; }
            }
            for (let i = 0; i < 8; i++) {
                isFn[8][size - 1 - i] = 1;
                isFn[size - 1 - i][8] = 1;
            }
            setFn(8, size - 8, true);

            if (ver >= 7) {
                let rem = ver;
                for (let i = 0; i < 12; i++) rem = (rem << 1) ^ ((rem >>> 11) * 0x1F25);
                const vbits = (ver << 12) | rem;
                for (let i = 0; i < 18; i++) {
                    const bit = ((vbits >>> i) & 1) !== 0;
                    const a = size - 11 + (i % 3);
                    const b = Math.floor(i / 3);
                    setFn(a, b, bit);
                    setFn(b, a, bit);
                }
            }

            // Ziguezague dos bits de dados
            let bitIdx = 0;
            const totalBits = finalCw.length * 8;
            for (let right = size - 1; right >= 1; right -= 2) {
                if (right === 6) right = 5;
                for (let vert = 0; vert < size; vert++) {
                    for (let j = 0; j < 2; j++) {
                        const cx = right - j;
                        const up = ((right + 1) & 2) === 0;
                        const cy = up ? (size - 1 - vert) : vert;
                        if (!isFn[cy][cx]) {
                            let dark = false;
                            if (bitIdx < totalBits) {
                                dark = ((finalCw[bitIdx >>> 3] >>> (7 - (bitIdx & 7))) & 1) !== 0;
                                bitIdx++;
                            }
                            mod[cy][cx] = dark ? 1 : 0;
                        }
                    }
                }
            }

            // Aplica máscara de menor penalidade ou máscara 0 e grava Format Bits (ECC-L = 0b01)
            const mask = 0;
            for (let y = 0; y < size; y++) {
                for (let x = 0; x < size; x++) {
                    if (!isFn[y][x] && ((x + y) % 2 === 0)) mod[y][x] ^= 1;
                }
            }
            const dataFmt = (0x01 << 3) | mask;
            let fRem = dataFmt;
            for (let i = 0; i < 10; i++) fRem = (fRem << 1) ^ ((fRem >>> 9) * 0x537);
            const fmtBits = ((dataFmt << 10) | fRem) ^ 0x5412;

            // Primeira cópia (canto superior esquerdo: coluna x=8 e linha y=8)
            for (let i = 0; i <= 5; i++) mod[i][8] = (fmtBits >>> i) & 1;
            mod[7][8] = (fmtBits >>> 6) & 1;
            mod[8][8] = (fmtBits >>> 7) & 1;
            mod[8][7] = (fmtBits >>> 8) & 1;
            for (let i = 9; i < 15; i++) mod[8][14 - i] = (fmtBits >>> i) & 1;

            // Segunda cópia (canto superior direito y=8 e inferior esquerdo x=8)
            for (let i = 0; i <= 7; i++) mod[8][size - 1 - i] = (fmtBits >>> i) & 1;
            for (let i = 8; i < 15; i++) mod[size - 15 + i][8] = (fmtBits >>> i) & 1;
            mod[size - 8][8] = 1;

            return { size, mod };
        }

        function generateQrSvg(text) {
            const qr = encodeQrMatrix(text);
            if (!qr) return '';
            const margin = 2;
            const vs = qr.size + margin * 2;
            let d = '';
            for (let y = 0; y < qr.size; y++) {
                for (let x = 0; x < qr.size; x++) {
                    if (qr.mod[y][x]) d += `M${x + margin},${y + margin}h1v1h-1z`;
                }
            }
            return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${vs} ${vs}" shape-rendering="crispEdges"><rect width="${vs}" height="${vs}" fill="#ffffff"/><path fill="#000000" d="${d}"/></svg>`;
        }

        // --- 2. DATASET DE DEMONSTRAÇÃO REALISTA (QUANDO CARTÃO SD AINDA ESTÁ VAZIO) ---
        const WRP_MONTHS_SHORT = ['Jan', 'Fev', 'Mar', 'Abr', 'Mai', 'Jun', 'Jul', 'Ago', 'Set', 'Out', 'Nov', 'Dez'];

        function generateDemoEvents() {
            const nowSec = Math.floor(Date.now() / 1000);
            const catalog = [
                { title: 'Time', artist: 'Pink Floyd', album: 'The Dark Side of the Moon', genre: 'Prog Rock', year: 1973, codec: 'FLAC', sr: 192000, bits: 24, dur: 413, cat: 'music', preset: 'Studio Hi-Res' },
                { title: 'Money', artist: 'Pink Floyd', album: 'The Dark Side of the Moon', genre: 'Prog Rock', year: 1973, codec: 'FLAC', sr: 192000, bits: 24, dur: 382, cat: 'music', preset: 'Studio Hi-Res' },
                { title: 'Us and Them', artist: 'Pink Floyd', album: 'The Dark Side of the Moon', genre: 'Prog Rock', year: 1973, codec: 'FLAC', sr: 192000, bits: 24, dur: 469, cat: 'music', preset: 'Studio Hi-Res' },
                { title: 'Giorgio by Moroder', artist: 'Daft Punk', album: 'Random Access Memories', genre: 'Electronic', year: 2013, codec: 'FLAC', sr: 88200, bits: 24, dur: 544, cat: 'music', preset: 'Bass Punch' },
                { title: 'Instant Crush', artist: 'Daft Punk', album: 'Random Access Memories', genre: 'Electronic', year: 2013, codec: 'FLAC', sr: 88200, bits: 24, dur: 337, cat: 'music', preset: 'Bass Punch' },
                { title: 'Construção', artist: 'Chico Buarque', album: 'Construção', genre: 'MPB', year: 1971, codec: 'FLAC', sr: 96000, bits: 24, dur: 384, cat: 'music', preset: 'Vocal Warmth' },
                { title: 'Dreams', artist: 'Fleetwood Mac', album: 'Rumours', genre: 'Classic Rock', year: 1977, codec: 'FLAC', sr: 96000, bits: 24, dur: 257, cat: 'music', preset: 'Studio Hi-Res' },
                { title: 'Clube da Esquina Nº 2', artist: 'Milton Nascimento & Lô Borges', album: 'Clube da Esquina', genre: 'MPB', year: 1972, codec: 'FLAC', sr: 44100, bits: 16, dur: 218, cat: 'music', preset: 'Acoustic' },
                { title: 'Everything In Its Right Place', artist: 'Radiohead', album: 'Kid A', genre: 'Alternative', year: 2000, codec: 'FLAC', sr: 48000, bits: 24, dur: 251, cat: 'music', preset: 'Studio Hi-Res' },
                { title: 'So What', artist: 'Miles Davis', album: 'Kind of Blue', genre: 'Jazz', year: 1959, codec: 'WAV', sr: 192000, bits: 24, dur: 562, cat: 'music', preset: 'Jazz Lounge' },
                { title: 'Redbone', artist: 'Childish Gambino', album: 'Awaken, My Love!', genre: 'Soul & Funk', year: 2016, codec: 'FLAC', sr: 44100, bits: 24, dur: 326, cat: 'music', preset: 'Bass Punch' },
                { title: 'Sultans of Swing', artist: 'Dire Straits', album: 'Dire Straits', genre: 'Classic Rock', year: 1978, codec: 'FLAC', sr: 96000, bits: 24, dur: 348, cat: 'music', preset: 'Studio Hi-Res' },
                { title: 'NerdCast 1053 - Ficção Científica Hard', artist: 'Jovem Nerd', album: 'NerdCast', genre: 'Podcast', year: 2026, codec: 'MP3', sr: 44100, bits: 16, dur: 4600, cat: 'podcast', preset: 'Podcast Voz' },
                { title: 'DLSHOW #664 - Bastidores da Semana', artist: 'Desce a Letra Show', album: 'DLSHOW', genre: 'Podcast', year: 2026, codec: 'M4A', sr: 48000, bits: 16, dur: 3800, cat: 'podcast', preset: 'Podcast Voz' },
                { title: 'Intro (Faixa Pulada)', artist: 'Unknown Promo', album: 'Single', genre: 'Pop', year: 2024, codec: 'MP3', sr: 44100, bits: 16, dur: 190, cat: 'music', preset: 'Flat' }
            ];
            const out = [];
            let cursorEpoch = nowSec - 42 * 86400;

            const pushEv = (ep, item, isSkip = false) => {
                const dt = new Date(ep * 1000);
                const ls = isSkip ? 9 : (item.cat === 'podcast' ? Math.round(item.dur * 0.78) : item.dur);
                out.push({
                    ts: dt.toISOString().slice(0, 19),
                    epoch: ep,
                    wday: dt.getDay(),
                    hour: dt.getHours(),
                    id: 'demo_' + item.artist + '_' + item.title,
                    path: (item.cat === 'podcast' ? 'Podcasts/' : 'Musicas/') + item.artist + '/' + item.title + '.' + item.codec.toLowerCase(),
                    cat: item.cat,
                    title: item.title,
                    artist: item.artist,
                    album: item.album,
                    genre: item.genre,
                    year: item.year,
                    codec: item.codec,
                    sr: item.sr,
                    bits: item.bits,
                    dur_s: item.dur,
                    listen_s: ls,
                    pct: Math.min(100, Math.round((ls * 100) / item.dur)),
                    scrobble: !isSkip,
                    completed: !isSkip && ls >= item.dur * 0.9,
                    end: isSkip ? 'skip_next' : 'eof',
                    vol: 72,
                    eq_on: true,
                    eq_preset: item.preset
                });
            };

            for (let day = 0; day < 43; day++) {
                // Pausa de 2 dias apenas no início para gerar sequência longa de 22+ dias ativos no final
                if (day === 14 || day === 15) { cursorEpoch += 86400; continue; }

                // Manhãs com Podcasts em alguns dias úteis
                if (day % 4 === 1) {
                    pushEv(cursorEpoch + 8 * 3600 + 600, catalog[12], false);
                } else if (day % 6 === 2) {
                    pushEv(cursorEpoch + 9 * 3600 + 300, catalog[13], false);
                }

                // Sessão de Álbum Completo (3 faixas seguidas do Dark Side of the Moon à noite)
                if (day % 5 === 0) {
                    const baseNight = cursorEpoch + 22 * 3600;
                    pushEv(baseNight, catalog[0], false);
                    pushEv(baseNight + 420, catalog[1], false);
                    pushEv(baseNight + 810, catalog[2], false);
                }

                // Dia Recorde & Obsessão no Repeat (Dia 31: 'Time' repetida 6 vezes!)
                if (day === 31) {
                    for (let r = 0; r < 6; r++) {
                        pushEv(cursorEpoch + 20 * 3600 + r * 430, catalog[0], false);
                    }
                    pushEv(cursorEpoch + 23 * 3600, catalog[3], false);
                    pushEv(cursorEpoch + 23 * 3600 + 560, catalog[4], false);
                } else {
                    const sessionsToday = 3 + ((day * 5) % 5);
                    const musicPool = [0, 3, 5, 6, 7, 8, 9, 10, 11, 4, 1];
                    for (let s = 0; s < sessionsToday; s++) {
                        // Evolução de fases: primeiras semanas mais Chico/Miles, semanas recentes mais Pink Floyd/Daft Punk
                        const shift = day < 18 ? 2 : 0;
                        const pick = musicPool[(day * 2 + s * 3 + shift) % musicPool.length];
                        const hrPool = [12, 18, 20, 21, 21, 22, 22, 23];
                        const hr = hrPool[(day + s) % hrPool.length];
                        pushEv(cursorEpoch + hr * 3600 + s * 390, catalog[pick], false);
                    }
                }

                // Algumas tentativas puladas na faixa promocional
                if (day % 9 === 3) {
                    pushEv(cursorEpoch + 19 * 3600, catalog[14], true);
                }
                cursorEpoch += 86400;
            }
            return out;
        }

        // --- 3. PARSER DE JSONL / SUMMARY E MOTOR ESTATÍSTICO DE PADRÃO MUNDIAL ---
        function parseJsonlEvents(text) {
            const events = [];
            if (!text) return events;
            const lines = text.split(/\r?\n/);
            for (const ln of lines) {
                const trimmed = ln.trim();
                if (!trimmed || trimmed[0] !== '{') continue;
                try {
                    const obj = JSON.parse(trimmed);
                    if (typeof obj.listen_s === 'number') events.push(obj);
                } catch (e) {}
            }
            return events;
        }

        function convertSummaryToSyntheticEvents(summaryObj) {
            if (!summaryObj || !Array.isArray(summaryObj.tracks)) return [];
            const out = [];
            for (const t of summaryObj.tracks) {
                const plays = Math.max(1, t.plays || 1);
                const perPlaySec = Math.round((t.listen_s || 0) / plays);
                const ep = t.last_ts || Math.floor(Date.now() / 1000);
                const firstEp = t.first_ts || ep;
                const isPod = /^\/?(Podcasts?|Audiobooks?)\//i.test(t.path || '');
                for (let i = 0; i < plays; i++) {
                    const evEp = (i === plays - 1) ? firstEp : Math.max(firstEp, ep - i * 3600);
                    const dt = new Date(evEp * 1000);
                    out.push({
                        ts: dt.toISOString().slice(0, 19),
                        epoch: evEp,
                        wday: dt.getDay(),
                        hour: dt.getHours(),
                        id: t.id || t.path,
                        path: t.path || '',
                        cat: isPod ? 'podcast' : 'music',
                        title: t.title || '',
                        artist: t.artist || '',
                        album: t.album || '',
                        genre: t.genre || '',
                        year: t.year || 0,
                        codec: t.codec || 'PCM',
                        sr: t.sr || 44100,
                        bits: t.bits || 16,
                        dur_s: perPlaySec,
                        listen_s: perPlaySec,
                        scrobble: i < (t.scrobbles || 0),
                        completed: i < (t.completed || 0),
                        end: (i < (t.skips || 0)) ? 'skip_next' : 'eof',
                        vol: 70,
                        eq_on: false,
                        eq_preset: 'Flat'
                    });
                }
            }
            return out;
        }

        function inferDisplayTitle(ev) {
            if (ev.title && ev.title.trim()) return ev.title.trim();
            const p = (ev.path || '').split('/').pop() || 'Faixa Sem Título';
            return p.replace(/\.[a-z0-9]{2,4}$/i, '').replace(/_/g, ' ');
        }

        function inferDisplayArtist(ev) {
            if (ev.artist && ev.artist.trim()) return ev.artist.trim();
            const parts = (ev.path || '').replace(/^\/+/, '').split('/');
            if (parts.length >= 2) return parts[parts.length - 2];
            return ev.cat === 'podcast' ? 'Podcast' : 'Artista Local';
        }

        function inferDisplayAlbum(ev) {
            if (ev.album && ev.album.trim()) {
                return ev.album.trim().replace(/\s*\((19\d{2}|20\d{2})\)\s*$/, '').trim() || ev.album.trim();
            }
            const parts = (ev.path || '').replace(/^\/+/, '').split('/');
            if (parts.length >= 3) {
                const candidate = parts[parts.length - 2];
                if (!/^(musicas?|music|podcasts?|audiobooks?)$/i.test(candidate)) return candidate;
            }
            return '';
        }

        function inferReleaseYear(ev) {
            const rawY = Number(ev.year) || 0;
            if (rawY >= 1900 && rawY <= 2099) return rawY;
            const hay = `${ev.album || ''} ${ev.path || ''}`;
            const m = hay.match(/(?:^|[^0-9])(19[2-9]\d|20[0-3]\d)(?:$|[^0-9])/);
            if (m) return Number(m[1]);
            return 0;
        }

        function inferGenre(ev) {
            if (ev.cat === 'podcast') return 'Podcast';
            if (ev.cat === 'audiobook') return 'Audiolivro';
            if (ev.genre && ev.genre.trim() && !/^other$/i.test(ev.genre.trim())) {
                return ev.genre.trim();
            }
            const pathHint = (ev.path || '').toLowerCase();
            const hints = [
                [/prog/, 'Prog Rock'], [/jazz/, 'Jazz'], [/mpb|bossa/, 'MPB'], [/samba|pagode/, 'Samba'],
                [/blues/, 'Blues'], [/metal/, 'Metal'], [/indie/, 'Indie'], [/altern/, 'Alternative'],
                [/electr|synth|house|techno/, 'Electronic'], [/hip[- ]?hop|rap/, 'Hip-Hop'],
                [/soul|funk|r&b/, 'Soul & Funk'], [/classi|orquest|sinfon/, 'Clássica'],
                [/reggae/, 'Reggae'], [/rock/, 'Rock'], [/pop/, 'Pop'], [/sertanej/, 'Sertanejo'], [/forr[oó]/, 'Forró']
            ];
            for (const [rx, label] of hints) {
                if (rx.test(pathHint)) return label;
            }
            return '';
        }

        function resolveEventEpoch(ev) {
            if (typeof ev.epoch === 'number' && ev.epoch > 100000) return ev.epoch;
            if (ev.ts) {
                const parsed = Date.parse(ev.ts);
                if (!isNaN(parsed)) return Math.floor(parsed / 1000);
            }
            return 0;
        }

        function resolveDateKey(ev, ep) {
            if (ev.ts && /^\d{4}-\d{2}-\d{2}/.test(ev.ts)) return ev.ts.slice(0, 10);
            if (ep > 0) {
                const d = new Date(ep * 1000);
                const y = d.getFullYear();
                const m = String(d.getMonth() + 1).padStart(2, '0');
                const day = String(d.getDate()).padStart(2, '0');
                return `${y}-${m}-${day}`;
            }
            return '';
        }

        function formatDateKeyPt(dateKey, shortMode = false) {
            if (!dateKey || !/^\d{4}-\d{2}-\d{2}$/.test(dateKey)) return '--';
            const parts = dateKey.split('-');
            const mIdx = Math.max(0, Math.min(11, parseInt(parts[1], 10) - 1));
            const dayNum = parseInt(parts[2], 10);
            if (shortMode) return `${String(dayNum).padStart(2, '0')}/${WRP_MONTHS_SHORT[mIdx].toUpperCase()}`;
            return `${dayNum} de ${WRP_MONTHS_SHORT[mIdx]}`;
        }

        function computeWrappedAnalytics(events, period, category) {
            const nowEpoch = Math.floor(Date.now() / 1000);
            const curYear = new Date().getFullYear();

            // Ordena todos os eventos cronologicamente para rastrear 1ª descoberta (firstSeen)
            const chronologicalAll = events.slice().sort((a, b) => resolveEventEpoch(a) - resolveEventEpoch(b));
            const firstSeenTrack = new Map();
            const firstSeenArtist = new Map();
            let globalMinEp = Infinity;
            let globalMaxEp = 0;

            for (const ev of chronologicalAll) {
                if ((Number(ev.listen_s) || 0) < 15) continue;
                const ep = resolveEventEpoch(ev);
                if (ep > 0) {
                    if (ep < globalMinEp) globalMinEp = ep;
                    if (ep > globalMaxEp) globalMaxEp = ep;
                }
                const title = inferDisplayTitle(ev);
                const artist = inferDisplayArtist(ev);
                const tKey = (ev.id || (artist + '|' + title)).toLowerCase();
                const aKey = artist.toLowerCase();
                if (!firstSeenTrack.has(tKey)) firstSeenTrack.set(tKey, ep);
                if (!firstSeenArtist.has(aKey)) firstSeenArtist.set(aKey, ep);
            }

            let windowStartEp = 0;
            if (period === 'year') {
                windowStartEp = Math.floor(new Date(curYear, 0, 1).getTime() / 1000);
            } else if (period === '30d') {
                windowStartEp = nowEpoch - 30 * 86400;
            } else if (period === '7d') {
                windowStartEp = nowEpoch - 7 * 86400;
            }

            const filtered = chronologicalAll.filter(ev => {
                if (category !== 'all' && (ev.cat || 'music') !== category) return false;
                const ep = resolveEventEpoch(ev);
                if (period === 'year') {
                    if (ev.ts && !ev.ts.startsWith(String(curYear))) {
                        const y = ep ? new Date(ep * 1000).getFullYear() : curYear;
                        if (y !== curYear) return false;
                    }
                } else if (period === '30d' && ep) {
                    if (ep < windowStartEp) return false;
                } else if (period === '7d' && ep) {
                    if (ep < windowStartEp) return false;
                }
                return true;
            });

            // Para 'all', considera "Descobertas Recentes" o último terço da linha do tempo (ou últimos 14 dias)
            const discoveryCutoffEp = windowStartEp > 0
                ? windowStartEp
                : (globalMaxEp > globalMinEp + 7 * 86400 ? (globalMaxEp - Math.min(14 * 86400, Math.floor((globalMaxEp - globalMinEp) / 3))) : 0);

            let totalSec = 0;
            let totalSamples = 0;
            let totalScrobbles = 0;
            let totalCompleted = 0;
            let earlySkips = 0;
            let volWeighted = 0;
            let volWeightSec = 0;
            let eqActiveSec = 0;

            let hiResSec = 0;
            let cdLosslessSec = 0;
            let lossySec = 0;
            let maxSr = 0;
            let maxBits = 0;

            let yearWeightedSum = 0;
            let yearWeightSec = 0;

            const hoursSec = new Array(24).fill(0);
            const wdaysSec = new Array(7).fill(0);
            const trackMap = new Map();
            const artistMap = new Map();
            const albumMap = new Map();
            const genreMap = new Map();
            const decadeMap = new Map();
            const calDayMap = new Map();
            const dayTrackPlays = new Map();
            const dayArtistSec = new Map();
            const nightArtistSec = new Map();
            const presetMap = new Map();
            const daySet = new Set();

            // Variáveis de sequência cronológica (Loop consecutivo & Sessão de Álbum Completo)
            let lastTrackKey = '';
            let curConsecTrack = 0;
            let bestConsecTrack = { title: '', artist: '', count: 0 };

            let lastAlbumKey = '';
            let curAlbumRun = 0;
            let fullAlbumSessions = 0;

            for (const ev of filtered) {
                const ls = Math.max(0, Number(ev.listen_s) || 0);
                const sr = Number(ev.sr) || 44100;
                const bits = Number(ev.bits) || 16;
                const codec = String(ev.codec || 'PCM').toUpperCase();
                const ep = resolveEventEpoch(ev);
                const dKey = resolveDateKey(ev, ep);

                totalSec += ls;
                totalSamples += ls * sr;
                if (ev.scrobble) totalScrobbles++;
                if (ev.completed) totalCompleted++;
                const isEarlySkip = (!ev.scrobble && ls < 25 && String(ev.end || '').startsWith('skip'));
                if (isEarlySkip) earlySkips++;

                if (sr > maxSr) maxSr = sr;
                if (bits > maxBits) maxBits = bits;

                if (typeof ev.vol === 'number' && ls > 0) {
                    volWeighted += ev.vol * ls;
                    volWeightSec += ls;
                }
                if (ev.eq_on) {
                    eqActiveSec += ls;
                    const pName = (ev.eq_preset && ev.eq_preset.trim()) ? ev.eq_preset.trim() : 'Ativo';
                    presetMap.set(pName, (presetMap.get(pName) || 0) + ls);
                } else {
                    presetMap.set('Flat', (presetMap.get('Flat') || 0) + ls);
                }

                const isLosslessCodec = /^(FLAC|WAV|PCM|ALAC|AIFF)$/i.test(codec);
                if (isLosslessCodec && (bits >= 24 || sr > 48000)) hiResSec += ls;
                else if (isLosslessCodec) cdLosslessSec += ls;
                else lossySec += ls;

                const hr = (typeof ev.hour === 'number' && ev.hour >= 0 && ev.hour < 24) ? ev.hour : 12;
                const wd = (typeof ev.wday === 'number' && ev.wday >= 0 && ev.wday < 7) ? ev.wday : 0;
                hoursSec[hr] += ls;
                wdaysSec[wd] += ls;

                if (ep && ls >= 15) {
                    const dayIdx = Math.floor((ep - 3 * 3600) / 86400);
                    daySet.add(dayIdx);
                }

                const title = inferDisplayTitle(ev);
                const artist = inferDisplayArtist(ev);
                const album = inferDisplayAlbum(ev);
                const relYear = inferReleaseYear(ev);
                const genre = inferGenre(ev);
                const tKey = (ev.id || (artist + '|' + title)).toLowerCase();
                const aKey = artist.toLowerCase();

                // Dia vs Madrugada/Noite
                if (ls > 0) {
                    if (hr >= 5 && hr <= 17) {
                        dayArtistSec.set(artist, (dayArtistSec.get(artist) || 0) + ls);
                    } else if (hr >= 20 || hr <= 4) {
                        nightArtistSec.set(artist, (nightArtistSec.get(artist) || 0) + ls);
                    }
                }

                // Dia do Calendário (Peak Listening Day)
                if (dKey && ls > 0) {
                    let cd = calDayMap.get(dKey);
                    if (!cd) {
                        cd = { dateKey: dKey, sec: 0, plays: 0 };
                        calDayMap.set(dKey, cd);
                    }
                    cd.sec += ls;
                    if (ev.scrobble || ls >= 30) cd.plays++;

                    if (ev.scrobble || ls >= 30) {
                        const dtKey = dKey + '|' + tKey;
                        const prevCnt = dayTrackPlays.get(dtKey) || { count: 0, dateKey: dKey, title, artist, tKey };
                        prevCnt.count++;
                        dayTrackPlays.set(dtKey, prevCnt);
                    }
                }

                // Décadas & Idade Musical (focado em música quando anos de lançamento existem)
                if (relYear >= 1930 && relYear <= 2035 && ls > 0 && (ev.cat !== 'podcast' || category === 'podcast')) {
                    if (ev.cat !== 'podcast') {
                        yearWeightedSum += relYear * ls;
                        yearWeightSec += ls;
                        const dec = Math.floor(relYear / 10) * 10;
                        decadeMap.set(dec, (decadeMap.get(dec) || 0) + ls);
                    }
                }
                if (genre && ls > 0) {
                    genreMap.set(genre, (genreMap.get(genre) || 0) + ls);
                }

                // Faixas
                let tEntry = trackMap.get(tKey);
                if (!tEntry) {
                    tEntry = {
                        key: tKey, title, artist, album, genre, year: relYear,
                        codec, sr, bits, cat: ev.cat || 'music',
                        sec: 0, scrobbles: 0, plays: 0, skips: 0,
                        firstEp: firstSeenTrack.get(tKey) || ep
                    };
                    trackMap.set(tKey, tEntry);
                }
                tEntry.sec += ls;
                tEntry.plays++;
                if (ev.scrobble) tEntry.scrobbles++;
                if (isEarlySkip) tEntry.skips++;
                if (!tEntry.album && album) tEntry.album = album;
                if (!tEntry.year && relYear) tEntry.year = relYear;
                if (!tEntry.genre && genre) tEntry.genre = genre;

                // Artistas
                let aEntry = artistMap.get(aKey);
                if (!aEntry) {
                    aEntry = {
                        name: artist, sec: 0, scrobbles: 0, plays: 0,
                        tracks: new Set(), albums: new Set(),
                        firstEp: firstSeenArtist.get(aKey) || ep
                    };
                    artistMap.set(aKey, aEntry);
                }
                aEntry.sec += ls;
                aEntry.plays++;
                if (ev.scrobble) aEntry.scrobbles++;
                aEntry.tracks.add(tKey);
                if (album && !/^single$/i.test(album)) aEntry.albums.add(album.toLowerCase());

                // Álbuns
                if (album && !/^single$/i.test(album) && ls > 0) {
                    const albKey = (artist + '|' + album).toLowerCase();
                    let albEntry = albumMap.get(albKey);
                    if (!albEntry) {
                        albEntry = {
                            key: albKey, name: album, artist, year: relYear,
                            sec: 0, scrobbles: 0, plays: 0, tracks: new Set(), fullSessions: 0
                        };
                        albumMap.set(albKey, albEntry);
                    }
                    albEntry.sec += ls;
                    albEntry.plays++;
                    if (ev.scrobble) albEntry.scrobbles++;
                    albEntry.tracks.add(tKey);
                    if (!albEntry.year && relYear) albEntry.year = relYear;

                    // Detecção de Sessão de Álbum Completo (3+ faixas seguidas sem pular do mesmo álbum)
                    if (ev.scrobble && !isEarlySkip) {
                        if (albKey === lastAlbumKey) {
                            curAlbumRun++;
                            if (curAlbumRun === 3 || (curAlbumRun > 3 && (curAlbumRun - 3) % 4 === 0)) {
                                fullAlbumSessions++;
                                albEntry.fullSessions++;
                            }
                        } else {
                            lastAlbumKey = albKey;
                            curAlbumRun = 1;
                        }
                    } else if (isEarlySkip) {
                        curAlbumRun = 0;
                        lastAlbumKey = '';
                    }
                } else if (isEarlySkip) {
                    curAlbumRun = 0;
                    lastAlbumKey = '';
                }

                // Loop consecutivo da mesma faixa
                if (ev.scrobble || ls >= 30) {
                    if (tKey === lastTrackKey) {
                        curConsecTrack++;
                    } else {
                        lastTrackKey = tKey;
                        curConsecTrack = 1;
                    }
                    if (curConsecTrack > bestConsecTrack.count) {
                        bestConsecTrack = { title, artist, count: curConsecTrack };
                    }
                }
            }

            // Maior Streak diário consecutivo
            const sortedDays = Array.from(daySet).sort((a, b) => a - b);
            let longestStreak = sortedDays.length ? 1 : 0;
            let curStreak = sortedDays.length ? 1 : 0;
            for (let i = 1; i < sortedDays.length; i++) {
                if (sortedDays[i] === sortedDays[i - 1] + 1) {
                    curStreak++;
                    if (curStreak > longestStreak) longestStreak = curStreak;
                } else {
                    curStreak = 1;
                }
            }

            const safeTotal = Math.max(1, totalSec);

            const topTracks = Array.from(trackMap.values())
                .filter(t => t.sec > 0)
                .sort((a, b) => (b.sec - a.sec) || (b.scrobbles - a.scrobbles));

            const topSkipped = Array.from(trackMap.values())
                .filter(t => t.skips >= 2)
                .sort((a, b) => b.skips - a.skips);

            const topArtists = Array.from(artistMap.values())
                .filter(a => a.sec > 0)
                .sort((a, b) => b.sec - a.sec)
                .map(a => Object.assign(a, {
                    sharePct: Math.max(1, Math.round((a.sec * 100) / safeTotal))
                }));

            const topAlbums = Array.from(albumMap.values())
                .filter(alb => alb.sec > 0)
                .sort((a, b) => (b.sec - a.sec) || (b.fullSessions - a.fullSessions))
                .map(alb => Object.assign(alb, {
                    tracksCount: alb.tracks.size,
                    sharePct: Math.max(1, Math.round((alb.sec * 100) / safeTotal))
                }));

            const topGenres = Array.from(genreMap.entries())
                .filter(([, sec]) => sec > 0)
                .sort((a, b) => b[1] - a[1])
                .map(([name, sec]) => ({
                    name,
                    sec,
                    pct: Math.max(1, Math.round((sec * 100) / safeTotal))
                }));

            // Décadas & Idade Musical (Spotify Listening Age + Last.fm Decades)
            const decPalette = {
                1950: '#f59e0b', 1960: '#ef4444', 1970: '#00e5ff', 1980: '#ec4899',
                1990: '#8b5cf6', 2000: '#10b981', 2010: '#3b82f6', 2020: '#eab308'
            };
            const safeYearSec = Math.max(1, yearWeightSec);
            const decades = Array.from(decadeMap.entries())
                .sort((a, b) => b[1] - a[1])
                .map(([dec, sec]) => ({
                    decade: dec,
                    label: dec >= 2000 ? `Anos ${dec}` : `Anos ${String(dec).slice(2)}`,
                    short: `${String(dec).slice(2)}s`,
                    sec,
                    pct: Math.max(1, Math.round((sec * 100) / safeYearSec)),
                    color: decPalette[dec] || '#00b4d8'
                }));
            const musicalAge = yearWeightSec > 0 ? Math.round(yearWeightedSum / yearWeightSec) : 0;
            const topDecade = decades[0] || null;

            // Dia Recorde do Ano (Peak Calendar Day)
            let peakCalendarDay = { dateKey: '', label: '--', shortLabel: '--', sec: 0, plays: 0 };
            for (const cd of calDayMap.values()) {
                if (cd.sec > peakCalendarDay.sec) {
                    peakCalendarDay = {
                        dateKey: cd.dateKey,
                        label: formatDateKeyPt(cd.dateKey, false),
                        shortLabel: formatDateKeyPt(cd.dateKey, true),
                        sec: cd.sec,
                        plays: cd.plays
                    };
                }
            }

            // Maior Vício / Obsessão em Loop em 1 único dia
            let bestOneDayLoop = null;
            for (const item of dayTrackPlays.values()) {
                if (!bestOneDayLoop || item.count > bestOneDayLoop.count) {
                    bestOneDayLoop = item;
                }
            }
            const top1Track = topTracks[0] || null;
            const loopObsession = bestOneDayLoop ? {
                title: bestOneDayLoop.title,
                artist: bestOneDayLoop.artist,
                maxPlaysOneDay: bestOneDayLoop.count,
                dateLabel: formatDateKeyPt(bestOneDayLoop.dateKey, false),
                shortDate: formatDateKeyPt(bestOneDayLoop.dateKey, true),
                maxConsecutive: bestConsecTrack.count,
                totalPlays: (trackMap.get(bestOneDayLoop.tKey)?.scrobbles || trackMap.get(bestOneDayLoop.tKey)?.plays || bestOneDayLoop.count)
            } : (top1Track ? {
                title: top1Track.title,
                artist: top1Track.artist,
                maxPlaysOneDay: top1Track.plays,
                dateLabel: peakCalendarDay.label,
                shortDate: peakCalendarDay.shortLabel,
                maxConsecutive: bestConsecTrack.count,
                totalPlays: top1Track.scrobbles || top1Track.plays
            } : null);

            // Novas Descobertas no Período
            let newTracksCount = 0;
            let topNewTrack = null;
            if (discoveryCutoffEp > 0) {
                for (const t of topTracks) {
                    if (t.firstEp >= discoveryCutoffEp) {
                        newTracksCount++;
                        if (!topNewTrack) topNewTrack = t;
                    }
                }
            } else {
                newTracksCount = topTracks.length;
                topNewTrack = topTracks[0] || null;
            }
            let newArtistsCount = 0;
            if (discoveryCutoffEp > 0) {
                for (const a of topArtists) {
                    if (a.firstEp >= discoveryCutoffEp) newArtistsCount++;
                }
            } else {
                newArtistsCount = topArtists.length;
            }

            // Evolução / Fases Sonoras (Mensal quando >= 2 meses, ou Fases Semanais quando período curto)
            const monthGroups = new Map();
            let minFiltEp = Infinity, maxFiltEp = 0;
            for (const ev of filtered) {
                const ls = Math.max(0, Number(ev.listen_s) || 0);
                if (ls <= 0) continue;
                const ep = resolveEventEpoch(ev);
                if (ep > 0) {
                    if (ep < minFiltEp) minFiltEp = ep;
                    if (ep > maxFiltEp) maxFiltEp = ep;
                }
                const dKey = resolveDateKey(ev, ep);
                if (dKey) {
                    const ym = dKey.slice(0, 7);
                    if (!monthGroups.has(ym)) monthGroups.set(ym, []);
                    monthGroups.get(ym).push(ev);
                }
            }

            const summarizePhaseBucket = (label, evList) => {
                let secSum = 0;
                const aSec = new Map();
                const tSec = new Map();
                for (const e of evList) {
                    const ls = Math.max(0, Number(e.listen_s) || 0);
                    secSum += ls;
                    const art = inferDisplayArtist(e);
                    const tit = inferDisplayTitle(e);
                    aSec.set(art, (aSec.get(art) || 0) + ls);
                    tSec.set(tit, (tSec.get(tit) || 0) + ls);
                }
                let bestA = '--', bestASec = -1;
                for (const [k, v] of aSec.entries()) { if (v > bestASec) { bestASec = v; bestA = k; } }
                let bestT = '--', bestTSec = -1;
                for (const [k, v] of tSec.entries()) { if (v > bestTSec) { bestTSec = v; bestT = k; } }
                return { label, leader: bestA, topTrack: bestT, sec: secSum };
            };

            let phaseMode = 'Mensal';
            const phases = [];
            if (monthGroups.size >= 3) {
                const sortedYm = Array.from(monthGroups.keys()).sort().slice(-5);
                for (const ym of sortedYm) {
                    const [yy, mm] = ym.split('-');
                    const mLabel = `${WRP_MONTHS_SHORT[Math.max(0, parseInt(mm, 10) - 1)]}/${yy.slice(2)}`;
                    phases.push(summarizePhaseBucket(mLabel, monthGroups.get(ym)));
                }
            } else if (filtered.length > 0 && maxFiltEp > minFiltEp) {
                phaseMode = 'Fases do Período';
                const numBuckets = Math.min(4, Math.max(2, Math.ceil((maxFiltEp - minFiltEp) / (5 * 86400))));
                const span = Math.max(1, (maxFiltEp - minFiltEp) / numBuckets);
                for (let b = 0; b < numBuckets; b++) {
                    const bStart = minFiltEp + b * span;
                    const bEnd = (b === numBuckets - 1) ? (maxFiltEp + 1) : (minFiltEp + (b + 1) * span);
                    const sliceEvs = filtered.filter(e => {
                        const ep = resolveEventEpoch(e);
                        return ep >= bStart && ep < bEnd;
                    });
                    if (sliceEvs.length > 0) {
                        const dtStart = new Date(bStart * 1000);
                        const lbl = `${String(dtStart.getDate()).padStart(2, '0')}/${WRP_MONTHS_SHORT[dtStart.getMonth()]}`;
                        phases.push(summarizePhaseBucket(lbl, sliceEvs));
                    }
                }
            } else if (filtered.length > 0) {
                phaseMode = 'Sessão Atual';
                phases.push(summarizePhaseBucket('Agora', filtered));
            }

            // Dia vs Noite
            let dayLeader = '', dayBest = 0;
            for (const [k, v] of dayArtistSec.entries()) { if (v > dayBest) { dayBest = v; dayLeader = k; } }
            let nightLeader = '', nightBest = 0;
            for (const [k, v] of nightArtistSec.entries()) { if (v > nightBest) { nightBest = v; nightLeader = k; } }

            // Disputa pela Coroa (#1 vs #2)
            let crownRivalry = null;
            if (topArtists.length >= 2) {
                const diffSec = Math.max(0, topArtists[0].sec - topArtists[1].sec);
                crownRivalry = {
                    first: topArtists[0].name,
                    second: topArtists[1].name,
                    diffMin: Math.max(1, Math.round(diffSec / 60)),
                    closeRace: (topArtists[1].sec / Math.max(1, topArtists[0].sec)) >= 0.72
                };
            }

            let favPreset = 'Flat';
            let bestPresetSec = -1;
            for (const [k, v] of presetMap.entries()) {
                if (v > bestPresetSec) { bestPresetSec = v; favPreset = k; }
            }

            let peakHour = 0;
            for (let h = 1; h < 24; h++) {
                if (hoursSec[h] > hoursSec[peakHour]) peakHour = h;
            }
            let peakWday = 0;
            for (let d = 1; d < 7; d++) {
                if (wdaysSec[d] > wdaysSec[peakWday]) peakWday = d;
            }

            const hiResPct = Math.round((hiResSec * 100) / safeTotal);
            const cdPct = Math.round((cdLosslessSec * 100) / safeTotal);
            const losslessTotalPct = Math.min(100, hiResPct + cdPct);
            const lossyPct = Math.max(0, 100 - losslessTotalPct);
            const avgVol = volWeightSec > 0 ? Math.round(volWeighted / volWeightSec) : 70;
            const compRate = filtered.length > 0 ? Math.round((totalCompleted * 100) / filtered.length) : 0;
            const zeroSkipPct = filtered.length > 0 ? Math.max(0, 100 - Math.round((earlySkips * 100) / filtered.length)) : 100;

            // Determinação da Persona Comportamental (100% Humana, Zero Clichê de Ficha Técnica)
            const nightShare = (hoursSec[21] + hoursSec[22] + hoursSec[23] + hoursSec[0] + hoursSec[1] + hoursSec[2] + hoursSec[3]) / safeTotal;
            const morningShare = (hoursSec[5] + hoursSec[6] + hoursSec[7] + hoursSec[8] + hoursSec[9] + hoursSec[10]) / safeTotal;
            const podSec = filtered.reduce((acc, e) => acc + (e.cat === 'podcast' ? (e.listen_s || 0) : 0), 0);
            const topArtistShare = topArtists[0] ? topArtists[0].sharePct : 0;

            let persona = {
                badge: 'CURADORIA PRÓPRIA',
                title: 'Colecionador Intencional',
                desc: 'Sua trilha nasce de escolhas reais no MicroSD — sem algoritmo empurrando singles descartáveis.'
            };
            if (totalSec === 0) {
                persona = {
                    badge: 'PRONTO PARA O PLAY',
                    title: 'Explorador Sonoro',
                    desc: 'Escute suas pastas de músicas ou podcasts no MPS3 para revelar seus hábitos, eras e obsessões.'
                };
            } else if (podSec / safeTotal >= 0.55) {
                persona = {
                    badge: 'MARATONISTA DE VOZES',
                    title: 'Devorador de Podcasts',
                    desc: `${Math.round((podSec * 100) / safeTotal)}% da sua jornada foi imersa em episódios longos, liderados por ${topArtists[0] ? topArtists[0].name : 'seus criadores favoritos'}.`
                };
            } else if (fullAlbumSessions >= 3 && zeroSkipPct >= 88) {
                persona = {
                    badge: 'ESCUTA DE OBRA FECHADA',
                    title: 'Guardião do Álbum Completo',
                    desc: `${fullAlbumSessions} vezes você ouviu um álbum em sequência sem pular faixas (${zeroSkipPct}% de foco total) — como o artista concebeu.`
                };
            } else if (topArtistShare >= 34 && topArtists[0]) {
                persona = {
                    badge: 'FIDELIDADE ABSOLUTA',
                    title: `Superfã de ${topArtists[0].name}`,
                    desc: `${topArtists[0].name} dominou sozinho ${topArtistShare}% de todas as suas horas registradas neste período.`
                };
            } else if (loopObsession && loopObsession.maxPlaysOneDay >= 5 && nightShare >= 0.42) {
                persona = {
                    badge: 'IMERSÃO NOTURNA',
                    title: 'Coruja do Repeat',
                    desc: `Suas noites ganham vida às ${String(peakHour).padStart(2, '0')}h — com direito a repetir "${loopObsession.title}" ${loopObsession.maxPlaysOneDay}x num único dia!`
                };
            } else if (loopObsession && loopObsession.maxPlaysOneDay >= 5) {
                persona = {
                    badge: 'HIPNOSE SONORA',
                    title: 'Mestre do Repeat',
                    desc: `Quando uma música te fisga, você vai até o fundo: "${loopObsession.title}" tocou ${loopObsession.maxPlaysOneDay}x em ${loopObsession.dateLabel}.`
                };
            } else if (nightShare >= 0.44) {
                persona = {
                    badge: 'SESSÃO DA MADRUGADA',
                    title: 'Notívago Musical',
                    desc: `${Math.round(nightShare * 100)}% da sua escuta acontece no silêncio da noite (pico às ${String(peakHour).padStart(2, '0')}h), longe do mundo lá fora.`
                };
            } else if (musicalAge > 0 && musicalAge <= 1985 && topDecade) {
                persona = {
                    badge: `ALMA DE ${musicalAge}`,
                    title: 'Viajante da Era de Ouro',
                    desc: `Mesmo em pleno ${curYear}, seu coração vive nos ${topDecade.label} (${topDecade.pct}% das suas horas musicais).`
                };
            } else if (morningShare >= 0.45) {
                persona = {
                    badge: 'RITUAL MATINAL',
                    title: 'Energia das Primeiras Horas',
                    desc: `Suas manhãs têm trilha própria: pico às ${String(peakHour).padStart(2, '0')}h para começar o dia no ritmo certo.`
                };
            } else if (topArtists.length >= 8 && topArtistShare <= 22) {
                persona = {
                    badge: 'RADAR SEM FRONTEIRAS',
                    title: 'Garimpeiro Eclético',
                    desc: `Você transitou por ${artistMap.size} artistas e ${albumMap.size} álbuns diferentes sem ficar preso a uma única bolha.`
                };
            }

            return {
                eventCount: filtered.length,
                totalSec,
                totalSamples,
                totalScrobbles,
                totalCompleted,
                earlySkips,
                compRate,
                zeroSkipPct,
                uniqueTracks: trackMap.size,
                uniqueArtists: artistMap.size,
                uniqueAlbums: albumMap.size,
                fullAlbumSessions,
                longestStreak,
                activeDays: sortedDays.length,
                avgVol,
                favPreset,
                hiResPct,
                cdPct,
                losslessTotalPct,
                lossyPct,
                maxSr,
                maxBits,
                peakHour,
                peakWday,
                hoursSec,
                wdaysSec,
                peakCalendarDay,
                loopObsession,
                musicalAge,
                topDecade,
                decades,
                topGenres,
                topTracks,
                topArtists,
                topAlbums,
                topSkipped,
                discoveries: {
                    newTracks: newTracksCount,
                    newArtists: newArtistsCount,
                    topNewTrack
                },
                phases,
                phaseMode,
                dayVsNight: { dayLeader, nightLeader, daySec: dayBest, nightSec: nightBest },
                crownRivalry,
                persona
            };
        }

        // --- 4. FORMATAÇÃO & RENDERIZAÇÃO DO PAINEL HTML ---
        function fmtHoursMinutes(sec) {
            const totalMin = Math.round(sec / 60);
            const h = Math.floor(totalMin / 60);
            const m = totalMin % 60;
            if (h <= 0) return `${m} min`;
            return `${h}h ${String(m).padStart(2, '0')}m`;
        }

        function fmtLargeSamples(n) {
            if (n >= 1e9) return (n / 1e9).toFixed(2).replace('.', ',') + ' Bi';
            if (n >= 1e6) return (n / 1e6).toFixed(1).replace('.', ',') + ' Mi';
            if (n >= 1e3) return Math.round(n / 1e3) + ' mil';
            return String(Math.round(n));
        }

        function escWrpHtml(s) {
            return String(s || '').replace(/[&<>"']/g, c => ({
                '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
            }[c]));
        }

        function refreshWrappedDashboardUI() {
            const sourceEvents = WRAPPED_STATE.useDemo ? generateDemoEvents() : WRAPPED_STATE.rawEvents;
            const st = computeWrappedAnalytics(sourceEvents, WRAPPED_STATE.period, WRAPPED_STATE.category);
            WRAPPED_STATE.stats = st;

            const badge = document.getElementById('wrpDataSourceBadge');
            const banner = document.getElementById('wrpDemoBanner');
            const bannerTxt = document.getElementById('wrpDemoBannerText');
            if (WRAPPED_STATE.useDemo) {
                badge.textContent = 'Modo Demo';
                badge.style.color = '#f5a623';
                banner.style.display = 'flex';
                bannerTxt.textContent = WRAPPED_STATE.hasRealData
                    ? '🧪 Visualizando prévia de demonstração. Clique em "🧪 Demo" para voltar aos seus dados reais do SD.'
                    : '🧪 Cartão SD ainda sem histórico de faixas concluídas — exibindo prévia interativa para testar os gráficos e o envio ao Smartphone!';
            } else {
                badge.textContent = `Cartão SD (${st.eventCount} sessões)`;
                badge.style.color = 'var(--accent-color)';
                banner.style.display = 'none';
            }

            // Hero Persona + Idade Musical
            document.getElementById('wrpPersonaBadge').textContent = st.persona.badge;
            document.getElementById('wrpPersonaTitle').textContent = st.persona.title;
            document.getElementById('wrpPersonaDesc').textContent = st.persona.desc;
            const eraChip = document.getElementById('wrpEraChip');
            if (eraChip) {
                if (st.musicalAge > 0) {
                    eraChip.style.display = 'inline-block';
                    eraChip.textContent = `🕰️ Idade Musical: ${st.musicalAge}${st.topDecade ? ' • ' + st.topDecade.label : ''}`;
                } else {
                    eraChip.style.display = 'none';
                }
            }

            // 6 KPIs Humanos & Comportamentais
            document.getElementById('wrpKpiTime').textContent = fmtHoursMinutes(st.totalSec);
            const daysEq = st.totalSec / 86400;
            const vinyls = Math.max(1, Math.round(st.totalSec / (42 * 60)));
            document.getElementById('wrpKpiVinyl').textContent = daysEq >= 1
                ? `≈ ${daysEq.toFixed(1).replace('.', ',')} dias seguidos (${vinyls} LPs)`
                : `≈ ${vinyls} ${vinyls === 1 ? 'disco LP' : 'discos LP'} de 42m`;

            document.getElementById('wrpKpiPeakDate').textContent = st.peakCalendarDay.sec > 0 ? st.peakCalendarDay.label : '--';
            document.getElementById('wrpKpiPeakDateSub').textContent = st.peakCalendarDay.sec > 0
                ? `${fmtHoursMinutes(st.peakCalendarDay.sec)} num único dia`
                : 'Maior maratona em 1 dia';

            const loopEl = document.getElementById('wrpKpiLoopTrack');
            const loopSub = document.getElementById('wrpKpiLoopSub');
            if (st.loopObsession) {
                loopEl.textContent = st.loopObsession.title;
                loopEl.title = `${st.loopObsession.title} — ${st.loopObsession.artist}`;
                loopSub.textContent = st.loopObsession.maxPlaysOneDay >= 2
                    ? `${st.loopObsession.maxPlaysOneDay}x em ${st.loopObsession.dateLabel} (${st.loopObsession.totalPlays}x total)`
                    : `${st.loopObsession.totalPlays}x reproduzida no período`;
            } else {
                loopEl.textContent = '--';
                loopSub.textContent = 'Recorde no Repeat';
            }

            const topArtEl = document.getElementById('wrpKpiTopArtist');
            const topArtSub = document.getElementById('wrpKpiTopArtistShare');
            if (st.topArtists[0]) {
                topArtEl.textContent = st.topArtists[0].name;
                topArtEl.title = st.topArtists[0].name;
                topArtSub.textContent = `${st.topArtists[0].sharePct}% de todo o seu tempo`;
            } else {
                topArtEl.textContent = '--';
                topArtSub.textContent = '0% do seu tempo total';
            }

            document.getElementById('wrpKpiZeroSkip').textContent = st.totalSec > 0 ? `${st.zeroSkipPct}% foco` : '--';
            document.getElementById('wrpKpiAlbumSess').textContent = st.fullAlbumSessions > 0
                ? `${st.fullAlbumSessions} ${st.fullAlbumSessions === 1 ? 'sessão de álbum' : 'sessões de álbum'}`
                : `${st.compRate}% ouvidas até o fim`;

            document.getElementById('wrpKpiStreak').textContent = `${st.longestStreak} ${st.longestStreak === 1 ? 'dia' : 'dias'}`;
            document.getElementById('wrpKpiDiscoveries').textContent = st.totalSec > 0
                ? `${st.discoveries.newTracks} descobertas • Pico: ${WRP_WDAYS_SHORT[st.peakWday]}`
                : '0 descobertas no período';

            // Gráfico 1: Relógio Biológico 24h + Dia vs Noite + Dias da Semana
            document.getElementById('wrpPeakHourLabel').textContent = st.totalSec > 0
                ? `Pico às ${String(st.peakHour).padStart(2, '0')}h`
                : 'Sem atividade';
            const maxHr = Math.max(1, ...st.hoursSec);
            let hHtml = '';
            for (let h = 0; h < 24; h++) {
                const pct = Math.max(5, Math.round((st.hoursSec[h] * 100) / maxHr));
                const mins = Math.round(st.hoursSec[h] / 60);
                const isPeak = (st.totalSec > 0 && h === st.peakHour);
                const color = isPeak ? 'background:var(--accent-color);opacity:1;' : '';
                hHtml += `<div class="wrp-hour-col" title="${String(h).padStart(2, '0')}h: ${mins} min"><div class="wrp-hour-bar" style="height:${st.hoursSec[h] > 0 ? pct : 4}%;${color}"></div></div>`;
            }
            document.getElementById('wrpHoursBars').innerHTML = hHtml;

            const dnEl = document.getElementById('wrpDayNightSplit');
            if (dnEl) {
                const parts = [];
                if (st.dayVsNight.dayLeader) parts.push(`<span>☀️ <strong>Dia:</strong> ${escWrpHtml(st.dayVsNight.dayLeader)}</span>`);
                if (st.dayVsNight.nightLeader) parts.push(`<span>🌙 <strong>Noite:</strong> ${escWrpHtml(st.dayVsNight.nightLeader)}</span>`);
                dnEl.innerHTML = parts.join('') || '<span>Escute em horários variados para revelar seu contraste Dia vs. Noite.</span>';
            }

            const maxWd = Math.max(1, ...st.wdaysSec);
            document.getElementById('wrpWeekdaysMini').innerHTML = WRP_WDAYS_SHORT.map((dName, idx) => {
                const active = st.totalSec > 0 && idx === st.peakWday;
                const mins = Math.round(st.wdaysSec[idx] / 60);
                return `<div style="text-align:center;flex:1;${active ? 'color:var(--accent-color);font-weight:800;' : ''}" title="${mins} min"><div>${dName}</div><div style="font-size:0.85em;">${Math.round((st.wdaysSec[idx] * 100) / maxWd)}%</div></div>`;
            }).join('');

            // Gráfico 2: Décadas / Idade Musical & Gêneros (ou Programas em filtro Podcast)
            const eraTitleEl = document.getElementById('wrpEraBoxTitle');
            const eraLbl = document.getElementById('wrpMusicalAgeLabel');
            const decBar = document.getElementById('wrpDecadesBar');
            const decLeg = document.getElementById('wrpDecadesLegend');
            const genPills = document.getElementById('wrpTopGenresPills');

            if (st.decades.length > 0) {
                eraTitleEl.textContent = '🕰️ Décadas & Gêneros Musicais';
                eraLbl.textContent = st.musicalAge > 0 ? `Alma de ${st.musicalAge}` : st.decades[0].label;
                decBar.innerHTML = st.decades.map(d =>
                    `<div style="width:${d.pct}%;background:${d.color};" title="${escWrpHtml(d.label)}: ${d.pct}% (${fmtHoursMinutes(d.sec)})"></div>`
                ).join('');
                decLeg.innerHTML = st.decades.slice(0, 4).map(d =>
                    `<span><i class="wrp-dot" style="background:${d.color};"></i>${escWrpHtml(d.label)} (${d.pct}%)</span>`
                ).join('');
            } else if (st.topArtists.length > 0) {
                eraTitleEl.textContent = WRAPPED_STATE.category === 'podcast' ? '🎙️ Concentração por Programa' : '🧭 Concentração de Artistas & Gêneros';
                eraLbl.textContent = `${st.uniqueArtists} ${WRAPPED_STATE.category === 'podcast' ? 'canais' : 'artistas'}`;
                const barColors = ['#00e5ff', '#3b82f6', '#8b5cf6', '#f59e0b', '#10b981'];
                const sliceA = st.topArtists.slice(0, 4);
                decBar.innerHTML = sliceA.map((a, i) =>
                    `<div style="width:${a.sharePct}%;background:${barColors[i % barColors.length]};" title="${escWrpHtml(a.name)}: ${a.sharePct}%"></div>`
                ).join('');
                decLeg.innerHTML = sliceA.map((a, i) =>
                    `<span><i class="wrp-dot" style="background:${barColors[i % barColors.length]};"></i>${escWrpHtml(a.name)} (${a.sharePct}%)</span>`
                ).join('');
            } else {
                eraLbl.textContent = '--';
                decBar.innerHTML = '';
                decLeg.innerHTML = '<span style="color:var(--text-muted);">Sem dados suficientes neste filtro.</span>';
            }

            if (genPills) {
                if (st.topGenres.length > 0) {
                    genPills.innerHTML = st.topGenres.slice(0, 5).map(g =>
                        `<span class="wrp-genre-pill">${escWrpHtml(g.name)} <b>${g.pct}%</b></span>`
                    ).join('');
                } else {
                    genPills.innerHTML = `<span style="font-size:0.74em;color:var(--text-muted);">Adicione tags de gênero (ID3/FLAC) ou organize pastas por estilo para ver seus Top Gêneros.</span>`;
                }
            }

            // Ranking 1: Top 5 Faixas
            const tBox = document.getElementById('wrpTopTracksList');
            if (!st.topTracks.length) {
                tBox.innerHTML = `<div style="font-size:0.82em;color:var(--text-muted);padding:8px;">Nenhuma faixa registrada neste filtro.</div>`;
            } else {
                tBox.innerHTML = st.topTracks.slice(0, 5).map((t, i) => {
                    const subParts = [t.artist];
                    if (t.album) subParts.push(t.album);
                    if (t.year) subParts.push(String(t.year));
                    return `<div class="wrp-rank-row">` +
                        `<span class="wrp-rank-num">${i + 1}</span>` +
                        `<div class="wrp-rank-main"><div class="wrp-rank-name">${escWrpHtml(t.title)}</div><div class="wrp-rank-sub">${escWrpHtml(subParts.join(' • '))}</div></div>` +
                        `<div class="wrp-rank-meta">${t.scrobbles || t.plays}x<div style="font-size:0.78em;color:var(--text-muted);font-weight:500;">${fmtHoursMinutes(t.sec)}</div></div>` +
                    `</div>`;
                }).join('');
            }

            // Ranking 2: Top 5 Artistas & Criadores (com % de participação no tempo total!)
            const aBox = document.getElementById('wrpTopArtistsList');
            if (!st.topArtists.length) {
                aBox.innerHTML = `<div style="font-size:0.82em;color:var(--text-muted);padding:8px;">Nenhum artista registrado neste filtro.</div>`;
            } else {
                aBox.innerHTML = st.topArtists.slice(0, 5).map((a, i) => {
                    return `<div class="wrp-rank-row">` +
                        `<span class="wrp-rank-num">${i + 1}</span>` +
                        `<div class="wrp-rank-main"><div class="wrp-rank-name">${escWrpHtml(a.name)}</div><div class="wrp-rank-sub">${a.tracks.size} ${a.tracks.size === 1 ? 'faixa' : 'faixas'} • ${a.scrobbles || a.plays} plays</div></div>` +
                        `<div class="wrp-rank-meta">${fmtHoursMinutes(a.sec)}<div style="font-size:0.78em;color:var(--text-muted);font-weight:600;">${a.sharePct}% do total</div></div>` +
                    `</div>`;
                }).join('');
            }

            // Ranking 3: Top 5 Álbuns (Apple Music Replay / Roon Pillar)
            const albBox = document.getElementById('wrpTopAlbumsList');
            if (albBox) {
                if (!st.topAlbums.length) {
                    albBox.innerHTML = `<div style="font-size:0.82em;color:var(--text-muted);padding:8px;">Nenhum álbum identificado nas tags/pastas deste filtro.</div>`;
                } else {
                    albBox.innerHTML = st.topAlbums.slice(0, 5).map((alb, i) => {
                        const sub = `${alb.artist}${alb.year ? ' (' + alb.year + ')' : ''} • ${alb.tracksCount} ${alb.tracksCount === 1 ? 'faixa' : 'faixas'}`;
                        const sessTag = alb.fullSessions > 0 ? `${alb.fullSessions}x inteiro` : `${alb.scrobbles || alb.plays} plays`;
                        return `<div class="wrp-rank-row">` +
                            `<span class="wrp-rank-num">${i + 1}</span>` +
                            `<div class="wrp-rank-main"><div class="wrp-rank-name">${escWrpHtml(alb.name)}</div><div class="wrp-rank-sub">${escWrpHtml(sub)}</div></div>` +
                            `<div class="wrp-rank-meta">${fmtHoursMinutes(alb.sec)}<div style="font-size:0.78em;color:var(--text-muted);font-weight:500;">${escWrpHtml(sessTag)}</div></div>` +
                        `</div>`;
                    }).join('');
                }
            }

            // Ranking 4: Evolução & Fases Sonoras (Spotify Sound Evolution)
            const phBox = document.getElementById('wrpPhasesList');
            const phSub = document.getElementById('wrpPhasesSubLabel');
            if (phSub) phSub.textContent = st.phaseMode;
            if (phBox) {
                if (!st.phases.length) {
                    phBox.innerHTML = `<div style="font-size:0.82em;color:var(--text-muted);padding:8px;">Acumule sessões em dias diferentes para visualizar suas fases sonoras.</div>`;
                } else {
                    phBox.innerHTML = st.phases.map(ph => {
                        return `<div class="wrp-rank-row">` +
                            `<span class="wrp-rank-num" style="width:auto;padding:0 6px;font-family:monospace;">${escWrpHtml(ph.label)}</span>` +
                            `<div class="wrp-rank-main"><div class="wrp-rank-name">${escWrpHtml(ph.leader)}</div><div class="wrp-rank-sub">Destaque: "${escWrpHtml(ph.topTrack)}"</div></div>` +
                            `<div class="wrp-rank-meta">${fmtHoursMinutes(ph.sec)}</div>` +
                        `</div>`;
                    }).join('');
                }
            }

            // Insights & Curiosidades Comportamentais
            const facts = [];
            if (st.crownRivalry) {
                if (st.crownRivalry.closeRace) {
                    facts.push(`⚔️ <strong>Disputa acirrada pelo #1:</strong> <em>${escWrpHtml(st.crownRivalry.second)}</em> ficou a apenas <strong>${st.crownRivalry.diffMin} min</strong> de roubar a coroa de <em>${escWrpHtml(st.crownRivalry.first)}</em>.`);
                } else {
                    facts.push(`👑 <strong>Liderança folgada:</strong> <em>${escWrpHtml(st.crownRivalry.first)}</em> abriu <strong>${fmtHoursMinutes(st.topArtists[0].sec - st.topArtists[1].sec)}</strong> de vantagem sobre o 2º lugar (${escWrpHtml(st.crownRivalry.second)}).`);
                }
            }
            if (st.fullAlbumSessions > 0 && st.topAlbums[0]) {
                facts.push(`💿 <strong>Ritual do Álbum Completo:</strong> Você engatou <strong>${st.fullAlbumSessions} ${st.fullAlbumSessions === 1 ? 'sessão contínua' : 'sessões contínuas'}</strong> de álbum sem pular — destaque para <em>"${escWrpHtml(st.topAlbums[0].name)}"</em>.`);
            }
            if (st.discoveries.topNewTrack && st.discoveries.newTracks > 0) {
                facts.push(`🌱 <strong>Melhor descoberta recente:</strong> <em>"${escWrpHtml(st.discoveries.topNewTrack.title)}"</em> (${escWrpHtml(st.discoveries.topNewTrack.artist)}) já acumulou ${fmtHoursMinutes(st.discoveries.topNewTrack.sec)}.`);
            }
            if (st.topSkipped[0]) {
                facts.push(`⏭️ <strong>Paciência zero:</strong> <em>"${escWrpHtml(st.topSkipped[0].title)}"</em> levou <strong>${st.topSkipped[0].skips} skips rápidos</strong> antes dos 25 segundos.`);
            }
            document.getElementById('wrpFunFactsText').innerHTML = facts.join('<br>') || 'Dê o play no seu acervo para revelar curiosidades sobre suas fases, descobertas e álbuns favoritos.';

            renderWrappedCanvas();
            renderHeaderOled128x64();
        }

        // --- 5. CARREGAMENTO REMOTO DO CARTÃO SD (/api/analytics/manifest) ---
        async function refreshWrappedFromSd() {
            try {
                const stRes = await fetch('/api/status');
                if (stRes.ok) {
                    const stJson = await stRes.json();
                    if (stJson.ip) WRAPPED_STATE.deviceIp = stJson.ip;
                    if (stJson.wifi_mode) WRAPPED_STATE.wifiMode = stJson.wifi_mode;
                }
            } catch (e) {}

            try {
                const res = await fetch('/api/analytics/manifest');
                if (!res.ok) throw new Error('Manifest HTTP ' + res.status);
                const man = await res.json();
                const files = Array.isArray(man.files) ? man.files : [];
                const jsonlFiles = files.filter(f => /\.jsonl$/i.test(f.name)).sort((a, b) => a.name.localeCompare(b.name));
                const sumFile = files.find(f => f.name === 'tracks_summary.json');

                let loadedEvents = [];
                for (const jf of jsonlFiles) {
                    const r = await fetch('/api/download?path=' + encodeURIComponent(jf.download_path));
                    if (r.ok) {
                        const txt = await r.text();
                        loadedEvents = loadedEvents.concat(parseJsonlEvents(txt));
                    }
                }

                if (!loadedEvents.length && sumFile) {
                    const r = await fetch('/api/download?path=' + encodeURIComponent(sumFile.download_path));
                    if (r.ok) {
                        const sObj = await r.json();
                        loadedEvents = convertSummaryToSyntheticEvents(sObj);
                    }
                }

                WRAPPED_STATE.rawEvents = loadedEvents;
                WRAPPED_STATE.hasRealData = loadedEvents.length > 0;
                WRAPPED_STATE.useDemo = !WRAPPED_STATE.hasRealData;
            } catch (err) {
                WRAPPED_STATE.hasRealData = false;
                WRAPPED_STATE.useDemo = true;
            }
            refreshWrappedDashboardUI();
        }

        async function handleWrappedFileImport(fileList) {
            if (!fileList || !fileList.length) return;
            const f = fileList[0];
            const txt = await f.text();
            let evs = [];
            if (f.name.toLowerCase().endsWith('.json')) {
                try { evs = convertSummaryToSyntheticEvents(JSON.parse(txt)); } catch (e) {}
            } else {
                evs = parseJsonlEvents(txt);
            }
            if (evs.length > 0) {
                WRAPPED_STATE.rawEvents = evs;
                WRAPPED_STATE.hasRealData = true;
                WRAPPED_STATE.useDemo = false;
                refreshWrappedDashboardUI();
            } else {
                alert('Nenhum evento válido encontrado no arquivo selecionado.');
            }
        }

        // --- 6. GERADOR DE MÍDIA SOCIAL EM ALTA DEFINIÇÃO (CANVAS 2D 1080P & MOTOR OLED 128x64) ---
        const WRP_PALETTES = {
            oled_white: {
                bg0: '#04060a', bg1: '#0a0f17', bg2: '#04060a',
                accent: '#f4f9ff', accent2: '#94a3b8',
                cardBg: 'rgba(14, 19, 28, 0.88)', cardBorder: 'rgba(244, 249, 255, 0.36)',
                textMain: '#f8fafc', textSub: '#94a3b8', pillText: '#05080d',
                grid: 'rgba(244, 249, 255, 0.05)',
                oledOn: '#f4f9ff', oledTop: '#f4f9ff', oledOff: '#121822', oledGlow: 'rgba(244, 249, 255, 0.32)', oledTopGlow: 'rgba(244, 249, 255, 0.32)', dualZone: false
            },
            oled_blue: {
                bg0: '#03070e', bg1: '#071426', bg2: '#03070e',
                accent: '#00e5ff', accent2: '#0077ff',
                cardBg: 'rgba(8, 20, 36, 0.88)', cardBorder: 'rgba(0, 229, 255, 0.36)',
                textMain: '#f0f9ff', textSub: '#7dd3fc', pillText: '#030911',
                grid: 'rgba(0, 229, 255, 0.055)',
                oledOn: '#00e5ff', oledTop: '#00e5ff', oledOff: '#081828', oledGlow: 'rgba(0, 229, 255, 0.34)', oledTopGlow: 'rgba(0, 229, 255, 0.34)', dualZone: false
            },
            oled_yellow: {
                bg0: '#090702', bg1: '#171204', bg2: '#080601',
                accent: '#ffd600', accent2: '#ff9e00',
                cardBg: 'rgba(26, 20, 6, 0.88)', cardBorder: 'rgba(255, 214, 0, 0.36)',
                textMain: '#fffbeb', textSub: '#fde047', pillText: '#0f0c02',
                grid: 'rgba(255, 214, 0, 0.05)',
                oledOn: '#ffd600', oledTop: '#ffd600', oledOff: '#1f1905', oledGlow: 'rgba(255, 214, 0, 0.34)', oledTopGlow: 'rgba(255, 214, 0, 0.34)', dualZone: false
            },
            oled_green: {
                bg0: '#020905', bg1: '#051a0e', bg2: '#020804',
                accent: '#00ff66', accent2: '#10b981',
                cardBg: 'rgba(6, 26, 14, 0.88)', cardBorder: 'rgba(0, 255, 102, 0.36)',
                textMain: '#ecfdf5', textSub: '#6ee7b7', pillText: '#021007',
                grid: 'rgba(0, 255, 102, 0.05)',
                oledOn: '#00ff66', oledTop: '#00ff66', oledOff: '#072012', oledGlow: 'rgba(0, 255, 102, 0.34)', oledTopGlow: 'rgba(0, 255, 102, 0.34)', dualZone: false
            },
            oled_dual: {
                bg0: '#04070d', bg1: '#091526', bg2: '#04070d',
                accent: '#00e5ff', accent2: '#ffd600',
                cardBg: 'rgba(9, 20, 34, 0.88)', cardBorder: 'rgba(255, 214, 0, 0.40)',
                textMain: '#f8fafc', textSub: '#94a3b8', pillText: '#06090f',
                grid: 'rgba(0, 229, 255, 0.05)',
                oledOn: '#00e5ff', oledTop: '#ffd600', oledOff: '#081726', oledGlow: 'rgba(0, 229, 255, 0.34)', oledTopGlow: 'rgba(255, 214, 0, 0.36)', dualZone: true
            },
            cyber: {
                bg0: '#050811', bg1: '#0b192e', bg2: '#07101f',
                accent: '#00e5ff', accent2: '#0077ff',
                cardBg: 'rgba(13, 25, 45, 0.78)', cardBorder: 'rgba(0, 229, 255, 0.28)',
                textMain: '#f8fafc', textSub: '#94a3b8', pillText: '#041019',
                grid: 'rgba(0, 229, 255, 0.055)',
                oledOn: '#00e5ff', oledTop: '#00e5ff', oledOff: '#081828', oledGlow: 'rgba(0, 229, 255, 0.32)', oledTopGlow: 'rgba(0, 229, 255, 0.32)', dualZone: false
            },
            gold: {
                bg0: '#0e0b08', bg1: '#21180e', bg2: '#140f0a',
                accent: '#f5a623', accent2: '#ffd166',
                cardBg: 'rgba(36, 27, 18, 0.80)', cardBorder: 'rgba(245, 166, 35, 0.30)',
                textMain: '#fff8eb', textSub: '#cbb89d', pillText: '#1a1206',
                grid: 'rgba(245, 166, 35, 0.05)',
                oledOn: '#f5a623', oledTop: '#ffd166', oledOff: '#1f1608', oledGlow: 'rgba(245, 166, 35, 0.32)', oledTopGlow: 'rgba(245, 166, 35, 0.32)', dualZone: false
            },
            aurora: {
                bg0: '#140529', bg1: '#3a0ca3', bg2: '#10002b',
                accent: '#ff2e93', accent2: '#00f5d4',
                cardBg: 'rgba(28, 12, 54, 0.74)', cardBorder: 'rgba(255, 46, 147, 0.34)',
                textMain: '#ffffff', textSub: '#d8b4fe', pillText: '#ffffff',
                grid: 'rgba(255, 255, 255, 0.05)',
                oledOn: '#00f5d4', oledTop: '#ff2e93', oledOff: '#1b0938', oledGlow: 'rgba(0, 245, 212, 0.32)', oledTopGlow: 'rgba(255, 46, 147, 0.32)', dualZone: true
            },
            studio: {
                bg0: '#16181d', bg1: '#232730', bg2: '#121418',
                accent: '#ff5500', accent2: '#e2e8f0',
                cardBg: 'rgba(40, 44, 54, 0.82)', cardBorder: 'rgba(255, 255, 255, 0.16)',
                textMain: '#f1f5f9', textSub: '#94a3b8', pillText: '#ffffff',
                grid: 'rgba(255, 255, 255, 0.04)',
                oledOn: '#f1f5f9', oledTop: '#ff5500', oledOff: '#1c2029', oledGlow: 'rgba(241, 245, 249, 0.25)', oledTopGlow: 'rgba(255, 85, 0, 0.3)', dualZone: true
            }
        };

        // Fonte Bitmap 5x7 Autêntica SSD1306 / U8g2
        const OLED_FONT_5X7 = {
            ' ': [0x00,0x00,0x00,0x00,0x00], '!': [0x00,0x00,0x5F,0x00,0x00], '"': [0x00,0x07,0x00,0x07,0x00],
            '#': [0x14,0x7F,0x14,0x7F,0x14], '$': [0x24,0x2A,0x7F,0x2A,0x12], '%': [0x23,0x13,0x08,0x64,0x62],
            '&': [0x36,0x49,0x55,0x22,0x50], "'": [0x00,0x05,0x03,0x00,0x00], '(': [0x00,0x1C,0x22,0x41,0x00],
            ')': [0x00,0x41,0x22,0x1C,0x00], '*': [0x14,0x08,0x3E,0x08,0x14], '+': [0x08,0x08,0x3E,0x08,0x08],
            ',': [0x00,0x50,0x30,0x00,0x00], '-': [0x08,0x08,0x08,0x08,0x08], '.': [0x00,0x60,0x60,0x00,0x00],
            '/': [0x20,0x10,0x08,0x04,0x02], '0': [0x3E,0x51,0x49,0x45,0x3E], '1': [0x00,0x42,0x7F,0x40,0x00],
            '2': [0x42,0x61,0x51,0x49,0x46], '3': [0x21,0x41,0x45,0x4B,0x31], '4': [0x18,0x14,0x12,0x7F,0x10],
            '5': [0x27,0x45,0x45,0x45,0x39], '6': [0x3C,0x4A,0x49,0x49,0x30], '7': [0x01,0x71,0x09,0x05,0x03],
            '8': [0x36,0x49,0x49,0x49,0x36], '9': [0x06,0x49,0x49,0x29,0x1E], ':': [0x00,0x36,0x36,0x00,0x00],
            ';': [0x00,0x56,0x36,0x00,0x00], '<': [0x08,0x14,0x22,0x41,0x00], '=': [0x14,0x14,0x14,0x14,0x14],
            '>': [0x00,0x41,0x22,0x14,0x08], '?': [0x02,0x01,0x51,0x09,0x06], '@': [0x32,0x49,0x79,0x41,0x3E],
            'A': [0x7E,0x11,0x11,0x11,0x7E], 'B': [0x7F,0x49,0x49,0x49,0x36], 'C': [0x3E,0x41,0x41,0x41,0x22],
            'D': [0x7F,0x41,0x41,0x22,0x1C], 'E': [0x7F,0x49,0x49,0x49,0x41], 'F': [0x7F,0x09,0x09,0x09,0x01],
            'G': [0x3E,0x41,0x49,0x49,0x7A], 'H': [0x7F,0x08,0x08,0x08,0x7F], 'I': [0x00,0x41,0x7F,0x41,0x00],
            'J': [0x20,0x40,0x41,0x3F,0x01], 'K': [0x7F,0x08,0x14,0x22,0x41], 'L': [0x7F,0x40,0x40,0x40,0x40],
            'M': [0x7F,0x02,0x0C,0x02,0x7F], 'N': [0x7F,0x04,0x08,0x10,0x7F], 'O': [0x3E,0x41,0x41,0x41,0x3E],
            'P': [0x7F,0x09,0x09,0x09,0x06], 'Q': [0x3E,0x41,0x51,0x21,0x5E], 'R': [0x7F,0x09,0x19,0x29,0x46],
            'S': [0x46,0x49,0x49,0x49,0x31], 'T': [0x01,0x01,0x7F,0x01,0x01], 'U': [0x3F,0x40,0x40,0x40,0x3F],
            'V': [0x1F,0x20,0x40,0x20,0x1F], 'W': [0x3F,0x40,0x38,0x40,0x3F], 'X': [0x63,0x14,0x08,0x14,0x63],
            'Y': [0x07,0x08,0x70,0x08,0x07], 'Z': [0x61,0x51,0x49,0x45,0x43], '[': [0x00,0x7F,0x41,0x41,0x00],
            ']': [0x00,0x41,0x41,0x7F,0x00], '_': [0x40,0x40,0x40,0x40,0x40], '|': [0x00,0x00,0x7F,0x00,0x00]
        };

        function normAsciiOled(str) {
            return String(str || '')
                .normalize('NFD').replace(/[\u0300-\u036f]/g, '')
                .toUpperCase();
        }

        function createOled1BitBuffer(w, h) {
            const buf = Array.from({ length: h }, () => new Uint8Array(w));
            const setPx = (x, y, c = 1) => {
                const ix = x | 0, iy = y | 0;
                if (ix >= 0 && ix < w && iy >= 0 && iy < h) buf[iy][ix] = c ? 1 : 0;
            };
            const line = (x0, y0, x1, y1, c = 1) => {
                let ix0 = x0 | 0, iy0 = y0 | 0, ix1 = x1 | 0, iy1 = y1 | 0;
                const dx = Math.abs(ix1 - ix0), dy = -Math.abs(iy1 - iy0);
                const sx = ix0 < ix1 ? 1 : -1, sy = iy0 < iy1 ? 1 : -1;
                let err = dx + dy;
                while (true) {
                    setPx(ix0, iy0, c);
                    if (ix0 === ix1 && iy0 === iy1) break;
                    const e2 = 2 * err;
                    if (e2 >= dy) { err += dy; ix0 += sx; }
                    if (e2 <= dx) { err += dx; iy0 += sy; }
                }
            };
            const rect = (x, y, rw, rh, fill = false, c = 1) => {
                const ix = x | 0, iy = y | 0, iw = rw | 0, ih = rh | 0;
                if (fill) {
                    for (let yy = iy; yy < iy + ih; yy++) {
                        for (let xx = ix; xx < ix + iw; xx++) setPx(xx, yy, c);
                    }
                } else {
                    line(ix, iy, ix + iw - 1, iy, c);
                    line(ix, iy + ih - 1, ix + iw - 1, iy + ih - 1, c);
                    line(ix, iy, ix, iy + ih - 1, c);
                    line(ix + iw - 1, iy, ix + iw - 1, iy + ih - 1, c);
                }
            };
            const rrect = (x, y, rw, rh, fill = false, c = 1) => {
                const ix = x | 0, iy = y | 0, iw = rw | 0, ih = rh | 0;
                if (fill) {
                    rect(ix + 1, iy, iw - 2, ih, true, c);
                    rect(ix, iy + 1, iw, ih - 2, true, c);
                } else {
                    line(ix + 1, iy, ix + iw - 2, iy, c);
                    line(ix + 1, iy + ih - 1, ix + iw - 2, iy + ih - 1, c);
                    line(ix, iy + 1, ix, iy + ih - 2, c);
                    line(ix + iw - 1, iy + 1, ix + iw - 1, iy + ih - 2, c);
                }
            };
            const circle = (cx, cy, r, fill = false, c = 1) => {
                for (let dy = -r; dy <= r; dy++) {
                    for (let dx = -r; dx <= r; dx++) {
                        const d2 = dx * dx + dy * dy;
                        if (fill ? (d2 <= r * r) : (d2 >= (r - 0.8) * (r - 0.8) && d2 <= (r + 0.6) * (r + 0.6))) {
                            setPx(cx + dx, cy + dy, c);
                        }
                    }
                }
            };
            const text = (x, y, s, c = 1, scale = 1, spacing = 1, maxChars = 99) => {
                const clean = normAsciiOled(s).slice(0, maxChars);
                let cx = x | 0;
                const cy = y | 0;
                for (let i = 0; i < clean.length; i++) {
                    const cols = OLED_FONT_5X7[clean[i]] || OLED_FONT_5X7['?'];
                    for (let colIdx = 0; colIdx < 5; colIdx++) {
                        const bits = cols[colIdx];
                        for (let rowIdx = 0; rowIdx < 7; rowIdx++) {
                            if ((bits >>> rowIdx) & 1) {
                                for (let sy = 0; sy < scale; sy++) {
                                    for (let sx = 0; sx < scale; sx++) {
                                        setPx(cx + colIdx * scale + sx, cy + rowIdx * scale + sy, c);
                                    }
                                }
                            }
                        }
                    }
                    cx += 5 * scale + spacing;
                }
            };
            const textCenter = (y, s, c = 1, scale = 1, spacing = 1, maxChars = 99) => {
                const clean = normAsciiOled(s).slice(0, maxChars);
                const tw = clean.length * (5 * scale + spacing) - spacing;
                const x = Math.max(2, ((w - tw) / 2) | 0);
                text(x, y, clean, c, scale, spacing, maxChars);
            };
            const drawSdIcon = (x, y, c = 1) => {
                rect(x, y, 6, 7, false, c);
                setPx(x + 5, y, c ? 0 : 1);
                setPx(x + 4, y + 1, c);
                line(x + 2, y + 2, x + 2, y + 4, c);
                line(x + 4, y + 2, x + 4, y + 4, c);
            };
            const drawWifiIcon = (x, y, c = 1) => {
                line(x + 1, y, x + 5, y, c);
                setPx(x, y + 1, c); setPx(x + 6, y + 1, c);
                line(x + 2, y + 2, x + 4, y + 2, c);
                setPx(x + 1, y + 3, c); setPx(x + 5, y + 3, c);
                setPx(x + 3, y + 5, c);
            };
            const drawSpeakerIcon = (x, y, c = 1) => {
                rect(x, y + 2, 2, 3, true, c);
                line(x + 2, y + 1, x + 2, y + 5, c);
                line(x + 3, y, x + 3, y + 6, c);
                setPx(x + 5, y + 2, c); setPx(x + 5, y + 4, c);
                setPx(x + 6, y + 1, c); setPx(x + 6, y + 3, c); setPx(x + 6, y + 5, c);
            };
            const drawBatteryIcon = (x, y, c = 1) => {
                rect(x, y, 10, 6, false, c);
                line(x + 10, y + 1, x + 10, y + 4, c);
                rect(x + 2, y + 2, 6, 2, true, c);
            };
            return { w, h, buf, setPx, line, rect, rrect, circle, text, textCenter, drawSdIcon, drawWifiIcon, drawSpeakerIcon, drawBatteryIcon };
        }

        function blitOledToCanvas(ctx, oled, ox, oy, pitch, gap, pal) {
            const totalW = oled.w * pitch;
            const totalH = oled.h * pitch;
            const offColor = pal.oledOff || '#081828';
            const dualY = pal.dualZone ? 14 : -1;
            const topColor = pal.oledTop || pal.accent;
            const mainColor = pal.oledOn || pal.accent;

            // Fundo escuro externo (moldura) + vidro OLED contínuo sem grelha conurbada
            ctx.fillStyle = '#020408';
            ctx.fillRect(ox - 4, oy - 4, totalW + 8, totalH + 8);
            ctx.fillStyle = offColor;
            ctx.fillRect(ox, oy, totalW, totalH);

            const pxSize = Math.max(1, pitch - (gap || 0));

            for (let py = 0; py < oled.h; py++) {
                const isTop = (py < dualY);
                const onFill = isTop ? topColor : mainColor;
                const sy = oy + py * pitch;
                const row = oled.buf[py];
                for (let px = 0; px < oled.w; px++) {
                    if (row[px]) {
                        const sx = ox + px * pitch;
                        ctx.fillStyle = onFill;
                        ctx.fillRect(sx, sy, pxSize, pxSize);
                    }
                }
            }
        }

        function buildNowPlayingOled128x64(st) {
            const oled = createOled1BitBuffer(128, 64);
            oled.drawSdIcon(2, 2, 1);
            let npTitle = 'MPS3 WRAPPED';
            if (WRAPPED_STATE.period === '30d') npTitle = 'MPS3 30 DIAS';
            else if (WRAPPED_STATE.period === '7d') npTitle = 'MPS3 7 DIAS';
            else if (WRAPPED_STATE.period === 'year') npTitle = `MPS3 ${new Date().getFullYear()}`;
            oled.text(11, 2, npTitle, 1, 1, 1, 12);
            oled.drawWifiIcon(88, 2, 1);
            oled.drawSpeakerIcon(99, 2, 1);
            oled.drawBatteryIcon(114, 2, 1);
            oled.line(0, 11, 127, 11, 1);

            // Cassete / Reel icônico à esquerda (X=2..37, Y=15..40)
            oled.rrect(2, 15, 36, 26, false, 1);
            oled.circle(12, 28, 5, false, 1);
            oled.setPx(12, 28, 1);
            oled.circle(28, 28, 5, false, 1);
            oled.setPx(28, 28, 1);
            oled.line(12, 23, 28, 23, 1);
            oled.line(7, 40, 33, 40, 1);

            const tStr = normAsciiOled(fmtHoursMinutes(st.totalSec)).slice(0, 7);
            oled.text(42, 15, tStr, 1, 2, 2, 7);
            const topArt = st.topArtists[0]
                ? `${normAsciiOled(st.topArtists[0].name).slice(0, 9)} ${st.topArtists[0].sharePct}%`
                : `${st.uniqueArtists} ARTISTAS`;
            oled.text(42, 33, topArt, 1, 1, 1, 14);

            // Barra invertida estilo U8g2 com #1 Música e contagem de plays (Y=43..53)
            oled.rrect(2, 43, 124, 11, true, 1);
            const topT = st.topTracks[0];
            const playsTag = topT ? `${topT.scrobbles || topT.plays}X` : '';
            const rawTitle = topT ? String(topT.title || '').replace(/\[.*?\]|\(.*?\)/g, '').trim() : '';
            const maxTitleChars = Math.max(6, 14 - playsTag.length);
            const topName = rawTitle ? normAsciiOled(rawTitle).slice(0, maxTitleChars) : 'SEU SOM OFFLINE';
            oled.text(5, 45, `> #1 ${topName}`, 0, 1, 1, 15);
            if (playsTag) {
                oled.text(123 - playsTag.length * 6, 45, playsTag, 0, 1, 1, 5);
            }

            // Barra de Escuta Profunda Zero-Skip + Sequência (Y=56..62)
            oled.rect(2, 57, 70, 5, false, 1);
            const fillW = Math.max(6, Math.min(66, Math.round(((st.zeroSkipPct || 0) / 100) * 66)));
            oled.rect(4, 59, fillW, 1, true, 1);
            oled.text(76, 56, `FOCO ${st.zeroSkipPct}%`, 1, 1, 1, 9);
            return oled;
        }

        function buildRtaAndTopTracksOled128x64(st) {
            const oled = createOled1BitBuffer(128, 64);
            oled.rect(0, 0, 128, 11, true, 1);
            oled.text(4, 2, 'TOP FAIXAS * RTA 24H', 0, 1, 1, 21);

            const list = st.topTracks.slice(0, 4);
            for (let i = 0; i < 4; i++) {
                const yPos = 14 + i * 10;
                const item = list[i];
                const title = item ? normAsciiOled(item.title).slice(0, 14) : `FAIXA #${i + 1}`;
                const cnt = item ? `${item.scrobbles || item.plays}X`.slice(0, 4) : '--';
                if (i === 0) {
                    oled.rect(2, yPos - 1, 124, 9, true, 1);
                    oled.text(4, yPos, `${i + 1}. ${title}`, 0, 1, 1, 17);
                    oled.text(124 - cnt.length * 6, yPos, cnt, 0, 1, 1, 4);
                } else {
                    oled.text(4, yPos, `${i + 1}. ${title}`, 1, 1, 1, 17);
                    oled.text(124 - cnt.length * 6, yPos, cnt, 1, 1, 1, 4);
                }
            }

            oled.line(0, 54, 127, 54, 1);
            const leftFoot = st.musicalAge > 0
                ? `ALMA ${st.musicalAge}`
                : `PICO AS ${String(st.peakHour).padStart(2, '0')}H`;
            oled.text(3, 56, leftFoot, 1, 1, 1, 14);

            // Sparkline real das 24h (12 blocos de 2h cada)
            const buckets = [];
            for (let b = 0; b < 12; b++) {
                buckets.push((st.hoursSec[b * 2] || 0) + (st.hoursSec[b * 2 + 1] || 0));
            }
            const maxB = Math.max(1, ...buckets);
            for (let b = 0; b < 12; b++) {
                const h = buckets[b] > 0 ? Math.max(1, Math.round((buckets[b] / maxB) * 6)) : 1;
                oled.rect(90 + b * 3, 63 - h, 2, h, true, 1);
            }
            return oled;
        }

        // --- DESIGN SYSTEM OFICIAL MPS3 STORIES 1-BIT (PROPORÇÃO FÍSICA SSD1306 128x220) ---
        function mps3DrawPlayIcon(oled, x, y, c = 1) {
            oled.line(x, y, x, y + 6, c);
            oled.line(x + 1, y + 1, x + 1, y + 5, c);
            oled.line(x + 2, y + 2, x + 2, y + 4, c);
            oled.setPx(x + 3, y + 3, c);
        }

        function mps3DrawMiniText(oled, x, y, str, c = 1) {
            const miniNums = {
                '0': ["###", "# #", "# #", "# #", "###"],
                '1': [" # ", "## ", " # ", " # ", "###"],
                '2': ["###", "  #", "###", "#  ", "###"],
                '3': ["###", "  #", "###", "  #", "###"],
                '4': ["# #", "# #", "###", "  #", "  #"],
                '5': ["###", "#  ", "###", "  #", "###"],
                '6': ["###", "#  ", "###", "# #", "###"],
                '7': ["###", "  #", "  #", "  #", "  #"],
                '8': ["###", "# #", "###", "# #", "###"],
                '9': ["###", "# #", "###", "  #", "###"]
            };
            let curX = x;
            for (let i = 0; i < str.length; i++) {
                const glyph = miniNums[str[i]];
                if (glyph) {
                    for (let r = 0; r < 5; r++) {
                        for (let col = 0; col < 3; col++) {
                            if (glyph[r][col] === '#') {
                                oled.setPx(curX + col, y + r, c);
                            }
                        }
                    }
                    curX += 4;
                } else {
                    curX += 3;
                }
            }
        }

        function mps3DrawFirmwareBattery(oled, x, y, percent = 98) {
            const p = Math.max(0, Math.min(100, percent));
            const buf = `${p}`;
            const w = 15;
            const h = 9;
            const bx = x !== undefined ? x : (oled.w - w - 4);

            // 1. Moldura da bateria (u8g2_DrawFrame)
            oled.rect(bx, y, w, h, false, 1);
            // 2. Terminal de contato à esquerda (u8g2_DrawVLine bx - 1, y + 2, 5)
            oled.line(bx - 1, y + 2, bx - 1, y + 6, 1);

            // 3. Mini texto da porcentagem (oled_draw_mini_text) centralizado com folga livre de 4px nas laterais e 2px vertical
            const textW = buf.length * 4 - 1;
            const textX = bx + Math.floor((w - textW) / 2);
            const textY = y + 2;
            mps3DrawMiniText(oled, textX, textY, buf, 1);
        }

        function mps3DrawUserMoonCrater(oled, x, y, flipH = false) {
            // Lua 1 (Gibosa com Crateras, 6x7) desenhada pelo usuário
            const moon = [
                "..##..",
                ".#.##.",
                "###..#",
                "####.#",
                "###..#",
                ".#.##.",
                "..##.."
            ];
            for (let r = 0; r < 7; r++) {
                for (let c = 0; c < 6; c++) {
                    const char = flipH ? moon[r][5 - c] : moon[r][c];
                    if (char === '#') oled.setPx(x + c, y + r, 1);
                }
            }
        }

        function mps3DrawUserMoonCrescent(oled, x, y, flipH = false) {
            // Lua 2 (Crescente Estilizada, 5x7) desenhada pelo usuário
            const moon = [
                "..###",
                ".###.",
                "###..",
                "###..",
                "###..",
                ".###.",
                "..###"
            ];
            for (let r = 0; r < 7; r++) {
                for (let c = 0; c < 5; c++) {
                    const char = flipH ? moon[r][4 - c] : moon[r][c];
                    if (char === '#') oled.setPx(x + c, y + r, 1);
                }
            }
        }

        function mps3DrawSunIcon(oled, x, y) {
            // Sol radiante com raios 1-bit (7x7)
            const sun = [
                "   #   ",
                " # # # ",
                "  ###  ",
                "#######",
                "  ###  ",
                " # # # ",
                "   #   "
            ];
            for (let r = 0; r < 7; r++) {
                for (let c = 0; c < 7; c++) {
                    if (sun[r][c] === '#') oled.setPx(x + c, y + r, 1);
                }
            }
        }

        function mps3DrawCelestialStars(oled, leftMoonX, rightMoonX, sunX, sunY, skyTopY, rtaBase, st, starsProgress = 1.0) {
            const pStars = (typeof starsProgress === 'number') ? Math.max(0.0, Math.min(1.0, starsProgress)) : 1.0;
            if (pStars <= 0.0) return;

            // Semente LCG determinística baseada na assinatura acústica do usuário
            let seed = (st.totalSec || 12345) ^ ((st.totalScrobbles || 100) * 31) ^ ((st.uniqueArtists || 7) * 127);
            function lcg() {
                seed = (seed * 1664525 + 1013904223) >>> 0;
                return (seed >>> 8) / 16777216;
            }

            const hours = st.hoursSec || new Array(24).fill(0);
            const startX = 17;
            const nightHours = [0, 1, 2, 3, 4, 5, 18, 19, 20, 21, 22, 23];

            nightHours.forEach(h => {
                const val = hours[h] || 0;
                const bx = startX + h * 4 + 1;
                const starCount = val > 0 ? (val > 1800 ? 3 : (val > 600 ? 2 : 1)) : (lcg() > 0.45 ? 1 : 0);

                for (let s = 0; s < starCount; s++) {
                    const rY = skyTopY + 2 + Math.floor(lcg() * (rtaBase - skyTopY - 14));
                    const rX = bx + Math.floor((lcg() - 0.5) * 5);
                    const distSun = Math.hypot(rX - sunX, rY - sunY);
                    const starRevealOrder = (((rX * 47 + rY * 89 + s * 101) & 0xFF) / 255.0);

                    if (starRevealOrder <= pStars && distSun > 20 && rX >= 6 && rX <= oled.w - 7 && rY >= skyTopY && rY < rtaBase - 4) {
                        if (val > 1200 && s === 0 && pStars > 0.75) {
                            oled.setPx(rX, rY, 1);
                            oled.setPx(rX - 1, rY, 1);
                            oled.setPx(rX + 1, rY, 1);
                            oled.setPx(rX, rY - 1, 1);
                            oled.setPx(rX, rY + 1, 1);
                        } else {
                            oled.setPx(rX, rY, 1);
                        }
                    }
                }
            });

            const starGuidingCount = Math.min(4, (st.uniqueArtists || 5));
            for (let g = 0; g < starGuidingCount; g++) {
                const isLeft = (g % 2 === 0);
                const gx = isLeft ? (leftMoonX + 5 + Math.floor(lcg() * 14)) : (rightMoonX - 5 - Math.floor(lcg() * 14));
                const gy = skyTopY + 3 + Math.floor(lcg() * 8);
                const gRevealOrder = (((gx * 31 + gy * 97 + g * 53) & 0xFF) / 255.0);
                if (gRevealOrder <= pStars && Math.hypot(gx - sunX, gy - sunY) > 22) {
                    if (pStars > 0.7) {
                        oled.setPx(gx, gy, 1);
                        oled.setPx(gx - 1, gy, 1);
                        oled.setPx(gx + 1, gy, 1);
                        oled.setPx(gx, gy - 1, 1);
                        oled.setPx(gx, gy + 1, 1);
                    } else {
                        oled.setPx(gx, gy, 1);
                    }
                }
            }
        }

        function mps3DrawArtistBrackets(oled, y, text) {
            const clean = normAsciiOled(text);
            const tw = clean.length * 6 - 1;
            const textX = Math.floor((oled.w - tw) / 2);
            const brLeft = Math.max(4, textX - 6);
            const brRight = Math.min(oled.w - 5, textX + tw + 5);

            oled.line(brLeft, y + 9, brRight, y + 9, 1);
            oled.line(brLeft, y, brLeft, y + 9, 1);
            oled.line(brRight, y, brRight, y + 9, 1);
            oled.text(textX, y + 1, clean, 1, 1, 1);
        }

        function mps3DrawHeadliner(oled, y, name, sharePct) {
            const clean = normAsciiOled(name);
            const tag = sharePct > 0 ? `* HEADLINER (${sharePct}%) *` : '* HEADLINER *';
            mps3DrawArtistBrackets(oled, y, tag);

            const nameY = y + 12; // 1px para cima: respiro de 1px em relacao ao bloco PALCO
            if (clean.length <= 8) {
                const tw = clean.length * (5 * 2 + 2) - 2;
                const x = Math.max(4, Math.floor((oled.w - tw) / 2));
                oled.text(x, nameY, clean, 1, 2, 2);
                return 29;
            } else {
                const tw = clean.length * 7 - 1;
                const boxW = Math.min(oled.w - 12, tw + 10);
                const boxX = Math.floor((oled.w - boxW) / 2);
                const textX = Math.floor((oled.w - tw) / 2);

                oled.rrect(boxX, nameY, boxW, 12, false, 1);
                oled.text(textX, nameY + 2, clean, 1, 1, 2);
                oled.text(textX + 1, nameY + 2, clean, 1, 1, 2);
                return 26;
            }
        }

        function mps3DrawProgressBarWithTime(oled, x, y, w, h, curSec, totalSec, labelLeft, labelRight) {
            oled.rrect(x, y, w, h, false, 1);
            const ratio = totalSec > 0 ? Math.min(1.0, Math.max(0.0, curSec / totalSec)) : 0;
            const innerW = w - 2;
            const innerH = h - 2;
            const fillW = Math.round(ratio * innerW);

            const leftTxt = labelLeft || 'PALCO';
            const rightTxt = labelRight || fmtHoursMinutes(totalSec);
            const textY = y + Math.floor((h - 7) / 2);

            oled.text(x + 4, textY, leftTxt, 1, 1, 1);
            const rW = rightTxt.length * 6 - 1;
            oled.text(x + w - 4 - rW, textY, rightTxt, 1, 1, 1);

            for (let py = y + 1; py < y + 1 + innerH; py++) {
                for (let px = x + 1; px < x + 1 + fillW; px++) {
                    oled.buf[py][px] = oled.buf[py][px] ? 0 : 1;
                }
            }
        }

        function mps3DrawListRow(oled, x, y, w, h, num, title, meta, isSelected) {
            const textY = y + Math.floor((h - 7) / 2);
            const maxChars = Math.max(6, Math.floor((w - 28 - (meta ? meta.length * 6 : 0)) / 6));
            const cleanTitle = normAsciiOled(title).slice(0, maxChars);

            if (isSelected) {
                oled.rect(x, y, w, h, true, 1);
                oled.text(x + 3, textY, `${num}.`, 0, 1, 1);
                oled.text(x + 16, textY, cleanTitle, 0, 1, 1);
                if (meta) {
                    const mw = meta.length * 6 - 1;
                    oled.text(x + w - 4 - mw, textY, meta, 0, 1, 1);
                }
            } else {
                oled.text(x + 3, textY, `${num}.`, 1, 1, 1);
                oled.text(x + 16, textY, cleanTitle, 1, 1, 1);
                if (meta) {
                    const mw = meta.length * 6 - 1;
                    oled.text(x + w - 4 - mw, textY, meta, 1, 1, 1);
                }
            }
        }

        function mps3DrawMetricCard(oled, x, y, w, bottomY, val, label) {
            // Card nativo do firmware MPS3: emerge da borda inferior (sem linha de baixo própria!)
            oled.line(x, y, x + w - 1, y, 1);
            oled.line(x, y, x, bottomY, 1);
            oled.line(x + w - 1, y, x + w - 1, bottomY, 1);

            // Valor numérico (ex: 152, 9, 27D)
            const twVal = val.length * 6 - 1;
            const txVal = x + Math.max(1, Math.floor((w - twVal) / 2));
            oled.text(txVal, y + 2, val, 1, 1, 1);

            // Linha divisória interna
            oled.line(x + 2, y + 10, x + w - 3, y + 10, 1);

            // Rótulo descritivo (ex: PLAYS, ART, DIAS) centralizado e descido 1px
            const twLbl = label.length * 6 - 1;
            const txLbl = x + Math.max(1, Math.floor((w - twLbl) / 2));
            oled.text(txLbl, y + 12, label, 1, 1, 1);
        }

        function renderStoriesNativeOledV95(oled, st, coHeadCount = 5, periodName = 'MENSAL', moonMode = 'dual_phases') {
            const gridW = oled.w;
            const gridH = oled.h;

            // 1. Moldura do display SSD1306 (pixel perfect)
            oled.rect(0, 0, gridW, gridH, false, 1);
            oled.line(2, 2, gridW - 3, 2, 1);
            oled.line(2, 2, 2, gridH - 3, 1);
            oled.line(gridW - 3, 2, gridW - 3, gridH - 3, 1);
            oled.line(2, gridH - 3, gridW - 3, gridH - 3, 1);

            // 2. Primeira Linha: Ícone Play + Badge MPS3 (mesma fonte do Headliner) + Periodicidade + Bateria Real MPS3
            mps3DrawPlayIcon(oled, 6, 4);

            const badgeX = 14;
            const badgeY = 2;
            const badgeW = 34;
            const badgeH = 11;
            oled.rrect(badgeX, badgeY, badgeW, badgeH, false, 1);

            const mps3X = badgeX + 4;
            oled.text(mps3X, badgeY + 2, 'MPS3', 1, 1, 2);
            oled.text(mps3X + 1, badgeY + 2, 'MPS3', 1, 1, 2);

            const periodStr = normAsciiOled(periodName).toUpperCase();
            oled.text(badgeX + badgeW + 5, badgeY + 2, periodStr, 1, 1, 1);

            mps3DrawFirmwareBattery(oled, gridW - 21, 4, 98);
            oled.line(2, 14, gridW - 3, 14, 1);

            let curY = 17;

            // Rodapé fixo na base: 3 Cards emergindo da moldura (sem linha inferior)
            const byCards = gridH - 20 - 3;
            const bottomY = gridH - 3;
            const contentMaxY = byCards - 3;

            const arts = (st && st.topArtists) ? st.topArtists.map(a => normAsciiOled((a && a.name) || '')) : [];
            const headliner = (arts[0] && arts[0].trim()) || 'ARTISTA #1';
            const headShare = st.topArtists && st.topArtists[0] ? (st.topArtists[0].sharePct || 0) : 0;

            // A. Headliner com Bracket
            mps3DrawHeadliner(oled, curY, headliner, headShare);
            curY = 44;

            // B. Barra de Palco Integrada (desgrudada do headliner)
            mps3DrawProgressBarWithTime(
                oled, 6, curY, gridW - 12, 10,
                st.totalSec, st.totalSec,
                'PALCO',
                fmtHoursMinutes(st.totalSec)
            );
            curY += 10;

            // C. Destaque Especial para CO-HEADLINERS (Simetria perfeita: 3px acima e 3px abaixo!)
            curY += 3;
            const tagCo = '- CO-HEADLINERS -';
            const tagCoW = tagCo.length * 6 - 1;
            const tagCoX = Math.floor((gridW - tagCoW) / 2);

            oled.line(6, curY + 3, tagCoX - 4, curY + 3, 1);
            oled.text(tagCoX, curY, tagCo, 1, 1, 1);
            oled.line(tagCoX + tagCoW + 3, curY + 3, gridW - 7, curY + 3, 1);
            curY += 7 + 3;

            // Lista de 5 Co-Headliners
            const listRows = coHeadCount;
            const rowH = 10;
            for (let r = 0; r < listRows; r++) {
                const artName = arts[r + 1] || `ARTISTA #${r + 2}`;
                const pct = st.topArtists && st.topArtists[r + 1] && typeof st.topArtists[r + 1].sharePct === 'number' ? `${st.topArtists[r + 1].sharePct}%` : '';
                mps3DrawListRow(oled, 6, curY + r * (rowH + 1), gridW - 12, rowH, r + 2, artName, pct, r === 0);
            }
            curY += listRows * (rowH + 1);

            // D. Álbum em badge direto do firmware
            const topAlb = st.topAlbums && st.topAlbums[0];
            const albName = (topAlb && topAlb.name) ? normAsciiOled(topAlb.name) : 'DISCOTECA MPS3';
            oled.rrect(6, curY, gridW - 12, 11, false, 1);
            oled.text(9, curY + 2, 'ALBUM:', 1, 1, 1);
            oled.text(48, curY + 2, albName, 1, 1, 1, 11);
            curY += 13;

            // Divisória sutil
            oled.line(6, curY, gridW - 7, curY, 1);
            curY += 3;

            // E. Ciclo 24h com Luas Híbridas Desenhadas pelo Usuário!
            const rtaBase = contentMaxY - 1;
            const peakH = (typeof st.peakHour === 'number') ? st.peakHour : 12;
            oled.textCenter(curY, `DIA 24H : PICO ${String(peakH).padStart(2, '0')}H`, 1, 1, 1);
            curY += 8;

            const rtaBarMaxH = Math.min(28, rtaBase - curY - 10);
            const sunY = rtaBase - rtaBarMaxH - 9;

            const hours = st.hoursSec || new Array(24).fill(0);
            const maxVal = Math.max(1, ...hours);
            const startX = 17;

            // Lua da Esquerda (00h) - Lua 1 (Crateras 6x7) na madrugada
            const leftMoonX = 6;
            const moonY = rtaBase - 13;
            if (moonMode === 'crater_both' || moonMode === 'dual_phases') {
                mps3DrawUserMoonCrater(oled, leftMoonX, moonY, false);
            } else {
                mps3DrawUserMoonCrescent(oled, leftMoonX, moonY, false);
            }

            for (let h = 0; h < 24; h++) {
                const val = hours[h] || 0;
                const ratio = maxVal > 0 ? (val / maxVal) : 0;
                const barH = ratio > 0 ? Math.max(4, Math.round(ratio * rtaBarMaxH)) : 2;
                const bx = startX + h * 4;

                oled.rect(bx, rtaBase - barH, 3, barH, true, 1);
                if (barH >= 8) {
                    oled.line(bx, rtaBase - barH - 2, bx + 2, rtaBase - barH - 2, 1);
                }
            }

            const sunX = startX + 12 * 4 - 2;
            mps3DrawSunIcon(oled, sunX, sunY);

            // Lua da Direita (23h) - Alinhada perfeitamente na borda direita do gráfico (x = gridW - 7)
            const rightMoonW = (moonMode === 'crater_both') ? 6 : 5;
            const rightMoonX = (gridW - 7) - rightMoonW + 1; // 121 - 5 + 1 = 117 (+3px para a direita)
            if (moonMode === 'crater_both') {
                mps3DrawUserMoonCrater(oled, rightMoonX, moonY, true);
            } else {
                mps3DrawUserMoonCrescent(oled, rightMoonX, moonY, true);
            }

            // Círculos radiais de estrelas procedurais vinculadas às estatísticas acústicas reais!
            mps3DrawCelestialStars(oled, leftMoonX, rightMoonX, sunX, sunY, curY, rtaBase, st);

            oled.line(6, rtaBase + 1, gridW - 7, rtaBase + 1, 1);

            // F. Rodapé: 3 Cards emergindo diretamente da moldura
            const playsVal = st.totalScrobbles >= 1000 ? `${(st.totalScrobbles / 1000).toFixed(1)}k` : `${st.totalScrobbles || 152}`;
            const artsVal = `${st.uniqueArtists || 9}`;
            const streakVal = `${st.longestStreak || 27}`;

            const bW = 36;
            const gap = 4;
            mps3DrawMetricCard(oled, 6, byCards, bW, bottomY, playsVal, 'PLAYS');
            mps3DrawMetricCard(oled, 6 + (bW + gap), byCards, bW, bottomY, artsVal, 'ART');
            mps3DrawMetricCard(oled, 6 + (bW + gap) * 2, byCards, bW, bottomY, streakVal, 'DIAS');
        }

        function buildFestivalLineupOled(st, gridH) {
            const gridW = 128;
            const oled = createOled1BitBuffer(gridW, gridH);

            // Pôster Vertical Stories 9:16 (Design System Oficial Firmware MPS3)
            if (gridH >= 210) {
                let periodName = 'MENSAL';
                if (WRAPPED_STATE.period === '7d') periodName = 'SEMANAL';
                else if (WRAPPED_STATE.period === '30d') periodName = 'MENSAL';
                else if (WRAPPED_STATE.period === '24h' || WRAPPED_STATE.period === '1d') periodName = 'DIARIO';
                else if (WRAPPED_STATE.period === 'year') {
                    const curMonth = new Date().getMonth();
                    periodName = (curMonth === 11) ? 'WRAPPED' : `${new Date().getFullYear()}`;
                } else if (WRAPPED_STATE.period === 'all') {
                    periodName = 'MENSAL';
                }
                renderStoriesNativeOledV95(oled, st, 5, periodName, 'dual_phases');
                return oled;
            }

            oled.rect(0, 0, gridW, gridH, false, 1);
            oled.rect(2, 2, gridW - 4, gridH - 4, false, 1);

            // Vínculo temporal dinâmico e reativo ao filtro ativo
            const curY = new Date().getFullYear();
            let festHeader = `MPS3 FEST ${curY}`;
            let stageHeader = 'LINEUP DO MEU ANO';
            if (WRAPPED_STATE.period === '30d') {
                festHeader = 'MPS3 FEST 30D';
                stageHeader = 'LINEUP DESTE MES';
            } else if (WRAPPED_STATE.period === '7d') {
                festHeader = 'MPS3 FEST 7D';
                stageHeader = 'LINEUP DA SEMANA';
            } else if (WRAPPED_STATE.period === 'all') {
                festHeader = 'MPS3 SOUNDFEST';
                stageHeader = 'LINEUP HISTORICO';
            }

            oled.drawSdIcon(6, 5, 1);
            oled.text(15, 5, festHeader, 1, 1, 1, 14);
            oled.drawSpeakerIcon(102, 5, 1);
            oled.drawBatteryIcon(112, 5, 1);
            oled.line(2, 14, gridW - 3, 14, 1);

            // Marquee de Palco Principal (invertido)
            oled.rrect(5, 17, gridW - 10, 18, true, 1);
            oled.textCenter(19, stageHeader, 0, 1, 1, 19);
            oled.textCenter(27, `${fmtHoursMinutes(st.totalSec)} DE PALCO`, 0, 1, 1, 19);

            const arts = st.topArtists.map(a => normAsciiOled(a.name));
            const headliner = arts[0] || 'SEU ARTISTA #1';
            const headShare = st.topArtists[0] ? `${st.topArtists[0].sharePct}% DO MEU SOM` : 'HEADLINER';

            // Palco Principal - Headliner em destaque gigante sem corte de caracteres
            oled.textCenter(39, `* ${headShare} *`, 1, 1, 1, 19);
            if (headliner.length <= 11) {
                // Até 11 caracteres (ex: $UICIDEBOY$): escala 2 com espaçamento 1px nítido (largura 120px com margem 4px)
                oled.textCenter(49, headliner, 1, 2, 1, 11);
            } else if (headliner.length <= 19) {
                // 12 a 19 caracteres: escala 1 com negrito double-strike e moldura fina
                oled.rrect(6, 47, gridW - 12, 14, false, 1);
                oled.textCenter(50, headliner, 1, 1, 1, 19);
                oled.textCenter(51, headliner, 1, 1, 1, 19);
            } else {
                // Nome muito longo: quebra em 2 linhas sem truncar palavras
                const words = headliner.split(/\s+/).filter(Boolean);
                let l1 = '', l2 = '';
                for (const w of words) {
                    if (!l1 || (l1 + ' ' + w).length <= 19) l1 = l1 ? (l1 + ' ' + w) : w;
                    else if (!l2 || (l2 + ' ' + w).length <= 19) l2 = l2 ? (l2 + ' ' + w) : w;
                }
                oled.textCenter(47, l1 || headliner.slice(0, 19), 1, 1, 1, 19);
                if (l2) oled.textCenter(56, l2, 1, 1, 1, 19);
            }
            oled.line(6, 66, gridW - 7, 66, 1);

            // Co-Headliners (#2 e #3) com respiro vertical e horizontal de 1px
            const a2 = (arts[1] || 'CONVIDADO #2').slice(0, 18);
            const a3 = (arts[2] || 'CONVIDADO #3').slice(0, 18);
            oled.textCenter(70, '- CO-HEADLINERS -', 1, 1, 1, 18);
            oled.rrect(6, 79, gridW - 12, 11, true, 1);
            oled.textCenter(81, a2, 0, 1, 1, 18);
            oled.rrect(6, 92, gridW - 12, 11, false, 1);
            oled.textCenter(94, a3, 1, 1, 1, 18);

            if (gridH >= 155) {
                oled.line(6, 106, gridW - 7, 106, 1);
                oled.textCenter(109, 'SPECIAL GUESTS', 1, 1, 1, 18);
                const line45 = [arts[3], arts[4]].filter(Boolean).map(s => s.slice(0, 8)).join(' * ') || 'ARTISTA #4 * #5';
                const line67 = [arts[5], arts[6]].filter(Boolean).map(s => s.slice(0, 8)).join(' * ') || 'ARTISTA #6 * #7';
                oled.textCenter(119, line45, 1, 1, 1, 19);
                oled.textCenter(129, line67, 1, 1, 1, 19);
            }

            if (gridH >= 210) {
                const line89 = [arts[7], arts[8], arts[9]].filter(Boolean).map(s => s.slice(0, 5)).join(' . ');
                if (line89) oled.textCenter(139, line89, 1, 1, 1, 19);

                // Destaque do Álbum Completo no Festival (título com cabeçalho limpo sem corte de caracteres)
                oled.line(6, 148, gridW - 7, 148, 1);
                oled.textCenter(151, 'ALBUM NO PALCO:', 1, 1, 1, 18);
                const topAlb = st.topAlbums[0] ? normAsciiOled(st.topAlbums[0].name).slice(0, 18) : 'DISCOTECA PROPRIA';
                oled.rrect(6, 160, gridW - 12, 11, true, 1);
                oled.textCenter(162, topAlb, 0, 1, 1, 18);

                // Canhoto VIP picotado + Código de barras 1-bit no rodapé!
                for (let dx = 4; dx < gridW - 4; dx += 4) {
                    oled.line(dx, 175, dx + 1, 175, 1);
                }
                // Formatação precisa para nunca estourar o limite de 19 colunas do SSD1306
                let ticketBadge = '';
                if (st.peakCalendarDay.sec > 0) {
                    ticketBadge = `PICO ${st.peakCalendarDay.shortLabel}`;
                } else {
                    ticketBadge = `STREAK ${st.longestStreak}`;
                }
                if (st.musicalAge > 0 && ticketBadge.length + 8 <= 19) {
                    ticketBadge += ` * A${st.musicalAge}`;
                } else if (st.zeroSkipPct > 0 && ticketBadge.length + 8 <= 19) {
                    ticketBadge += ` * ${st.zeroSkipPct}%`;
                }
                oled.textCenter(179, ticketBadge, 1, 1, 1, 19);

                // Código de barras estilizado 1-bit do ingresso (y=188..197)
                for (let bx = 10; bx < gridW - 10; bx += 2) {
                    const barW = ((bx * 7 + (st.totalScrobbles || 5)) % 3 === 0) ? 2 : 1;
                    oled.rect(bx, 188, barW, 10, true, 1);
                    if (barW === 2) bx += 1;
                }
                // Persona title reposicionado em y=201 (vai até y=207, deixando 8px livres antes da borda em 215)
                oled.textCenter(201, normAsciiOled(st.persona.title).slice(0, 19), 1, 1, 1, 19);
            } else {
                // Rodapé compacto para Square (116) ou Feed (150)
                const footLbl = st.musicalAge > 0 ? `VIP * ALMA ${st.musicalAge}` : `VIP * FOCO ${st.zeroSkipPct}%`;
                oled.rrect(6, gridH - 16, gridW - 12, 11, true, 1);
                oled.textCenter(gridH - 14, footLbl, 0, 1, 1, 18);
            }
            return oled;
        }

        // Renderiza o Mini-OLED 128x64 ao vivo no cabeçalho da Central Web
        function renderHeaderOled128x64() {
            const cvs = document.getElementById('headerOledCanvas');
            if (!cvs || !cvs.getContext) return;
            const oledKey = 'oled_' + (localStorage.getItem('mps3_oled_color') || 'blue');
            const pal = WRP_PALETTES[oledKey] || WRP_PALETTES.oled_blue;
            const st = WRAPPED_STATE.stats || computeWrappedAnalytics(
                WRAPPED_STATE.rawEvents.length ? WRAPPED_STATE.rawEvents : generateDemoEvents(),
                WRAPPED_STATE.period,
                WRAPPED_STATE.category
            );
            const oled = buildNowPlayingOled128x64(st);
            const ctx = cvs.getContext('2d');
            blitOledToCanvas(ctx, oled, 0, 0, 3, 0, pal);
        }
        window.renderHeaderOled128x64 = renderHeaderOled128x64;

        function syncWrappedOledColorFromSite(colorCode) {
            const map = {
                white: 'oled_white',
                blue: 'oled_blue',
                yellow: 'oled_yellow',
                green: 'oled_green',
                dual: 'oled_dual'
            };
            const targetTheme = map[colorCode];
            if (targetTheme && WRP_PALETTES[targetTheme]) {
                WRAPPED_STATE.cardTheme = targetTheme;
                document.querySelectorAll('#wrpThemeGroup .wrp-pill').forEach(b => {
                    b.classList.toggle('active', b.getAttribute('data-wtheme') === targetTheme);
                });
                renderWrappedCanvas();
            }
        }
        window.syncWrappedOledColorFromSite = syncWrappedOledColorFromSite;

        function drawRoundRect(ctx, x, y, w, h, r) {
            ctx.beginPath();
            ctx.moveTo(x + r, y);
            ctx.arcTo(x + w, y, x + w, y + h, r);
            ctx.arcTo(x + w, y + h, x, y + h, r);
            ctx.arcTo(x, y + h, x, y, r);
            ctx.arcTo(x, y, x + w, y, r);
            ctx.closePath();
        }

        function fitText(ctx, text, maxWidth) {
            let s = String(text || '');
            if (ctx.measureText(s).width <= maxWidth) return s;
            while (s.length > 1 && ctx.measureText(s + '…').width > maxWidth) {
                s = s.slice(0, -1);
            }
            return s + '…';
        }

        const MPS3_SPIRAL_SVG_PATH = "M 500.00,60.00 C 671.49,131.35 782.38,275.30 816.25,417.34 C 850.12,559.38 809.80,696.43 735.11,783.61 C 660.42,870.78 554.63,908.47 464.67,901.25 C 374.72,894.03 301.86,844.16 264.89,783.61 C 227.91,723.06 225.96,653.68 247.39,602.18 C 268.81,550.68 312.04,517.38 354.69,507.21 C 397.34,497.04 438.50,509.15 464.67,531.58 C 490.85,554.01 502.17,585.77 500.00,612.79 C 497.83,639.80 482.86,661.68 464.67,672.78 C 446.49,683.88 425.66,684.47 410.19,678.03 C 394.73,671.60 384.73,658.62 381.68,645.81 C 378.62,633.00 382.26,620.65 388.99,612.79 C 395.73,604.93 405.27,601.53 413.38,602.18 C 421.49,602.83 428.06,607.33 431.39,612.79 C 434.73,618.25 434.90,624.50 432.97,629.15 C 431.04,633.79 427.14,636.79 423.30,637.71 C 419.45,638.63 415.74,637.53 413.38,635.51 C 411.02,633.49 410.00,630.63 410.19,628.19 C 410.39,625.75 411.74,623.78 413.38,622.78 C 415.02,621.78 416.90,621.73 418.29,622.31 C 419.69,622.89 420.59,624.06 420.86,625.21 C 421.14,626.37 420.81,627.48 420.20,628.19 C 419.60,628.90 418.74,629.20 418.00,629.15 C 417.27,629.09 416.68,628.68 416.38,628.19 C 416.08,627.70 416.06,627.13 416.24,626.71 C 416.41,626.30 416.76,626.02 417.11,625.94 C 417.46,625.86 417.79,625.96 418.00,626.14 C 418.22,626.32 418.31,626.58 418.29,626.80 C 418.27,627.02 418.15,627.20 418.00,627.29 C 417.86,627.38 417.69,627.38 417.56,627.33 C 417.44,627.28 417.35,627.17 417.33,627.07 C 417.31,626.96 417.33,626.86 417.39,626.80 C 417.44,626.74 417.52,626.71 417.59,626.71 C 417.65,626.72 417.71,626.76 417.73,626.80 C 417.76,626.84 417.76,626.90 417.75,626.93 C 417.73,626.97 417.70,627.00 417.67,627.00 C 417.64,627.01 417.61,627.00 417.59,626.99 C 417.57,626.97 417.56,626.95 417.56,626.93 C 417.56,626.91 417.57,626.89 417.59,626.88 C 417.60,626.87 417.62,626.87 417.63,626.88 C 417.64,626.88 417.65,626.89 417.65,626.90 C 417.65,626.91 417.65,626.92 417.64,626.93 C 417.64,626.93 417.63,626.93 417.63,626.93 C 417.62,626.93 417.61,626.93 417.61,626.93";
        let _mps3SpiralPath2D = null;

        function drawMps3Emblem(ctx, cx, cy, radius, strokeColor) {
            ctx.save();
            ctx.translate(cx, cy);
            const scale = (radius * 2) / 1000;
            ctx.scale(scale, scale);
            ctx.translate(-500, -500);
            ctx.strokeStyle = strokeColor;
            ctx.lineWidth = 44;
            ctx.lineJoin = 'round';
            ctx.lineCap = 'round';

            // 1. Polígono estrela oficial da marca MPS3 (viewBox 0 0 1000 1000)
            ctx.beginPath();
            ctx.moveTo(500.00, 60.00);
            ctx.lineTo(735.11, 783.61);
            ctx.lineTo(119.58, 336.39);
            ctx.lineTo(880.42, 336.39);
            ctx.lineTo(264.89, 783.61);
            ctx.closePath();
            ctx.stroke();

            // 2. Espiral logarítmica oficial da marca MPS3 (idêntica ao favicon/ícone da página)
            if (typeof Path2D !== 'undefined') {
                if (!_mps3SpiralPath2D) {
                    _mps3SpiralPath2D = new Path2D(MPS3_SPIRAL_SVG_PATH);
                }
                ctx.stroke(_mps3SpiralPath2D);
            }
            ctx.restore();
        }
        if (typeof window !== 'undefined') {
            window.drawMps3Emblem = drawMps3Emblem;
        }

        function renderWrappedCanvas() {
            const canvas = document.getElementById('wrappedCanvas');
            if (!canvas) return;
            const st = WRAPPED_STATE.stats || computeWrappedAnalytics(generateDemoEvents(), 'all', 'all');
            const pal = WRP_PALETTES[WRAPPED_STATE.cardTheme] || WRP_PALETTES.oled_blue;

            let W = 1080, H = 1920, fmtLabel = 'Stories (9:16)';
            if (WRAPPED_STATE.format === 'feed') { W = 1080; H = 1350; fmtLabel = 'Feed (4:5)'; }
            else if (WRAPPED_STATE.format === 'square') { W = 1080; H = 1080; fmtLabel = 'Quadrado (1:1)'; }

            canvas.width = W;
            canvas.height = H;
            const dimEl = document.getElementById('wrpCanvasDimInfo');
            if (dimEl) dimEl.textContent = `${W} × ${H} px • ${fmtLabel}`;

            const ctx = canvas.getContext('2d');
            const pad = 68;
            const innerW = W - pad * 2;

            // 1. Fundo Gradiente + Orbes de Luz + Grade Sutil
            const bgGrad = ctx.createLinearGradient(0, 0, W, H);
            bgGrad.addColorStop(0, pal.bg0);
            bgGrad.addColorStop(0.52, pal.bg1);
            bgGrad.addColorStop(1, pal.bg2);
            ctx.fillStyle = bgGrad;
            ctx.fillRect(0, 0, W, H);

            const glow1 = ctx.createRadialGradient(W * 0.85, H * 0.14, 20, W * 0.85, H * 0.14, W * 0.62);
            glow1.addColorStop(0, pal.accent + '38');
            glow1.addColorStop(1, 'transparent');
            ctx.fillStyle = glow1;
            ctx.fillRect(0, 0, W, H);

            const glow2 = ctx.createRadialGradient(W * 0.12, H * 0.86, 20, W * 0.12, H * 0.86, W * 0.55);
            glow2.addColorStop(0, pal.accent2 + '2E');
            glow2.addColorStop(1, 'transparent');
            ctx.fillStyle = glow2;
            ctx.fillRect(0, 0, W, H);

            ctx.strokeStyle = pal.grid;
            ctx.lineWidth = 1;
            for (let gx = 0; gx < W; gx += 54) {
                ctx.beginPath(); ctx.moveTo(gx, 0); ctx.lineTo(gx, H); ctx.stroke();
            }
            for (let gy = 0; gy < H; gy += 54) {
                ctx.beginPath(); ctx.moveTo(0, gy); ctx.lineTo(W, gy); ctx.stroke();
            }

            // 2. Cabeçalho de Marca MPS3
            let y = 74;
            drawMps3Emblem(ctx, pad + 26, y + 22, 24, pal.accent);
            ctx.fillStyle = pal.textMain;
            ctx.font = '800 34px system-ui, -apple-system, Segoe UI, sans-serif';
            ctx.fillText('MPS3', pad + 68, y + 33);
            ctx.fillStyle = pal.accent;
            ctx.font = '700 22px system-ui, -apple-system, Segoe UI, sans-serif';
            ctx.fillText('WRAPPED', pad + 174, y + 31);

            const periodLabels = { all: 'HISTÓRICO COMPLETO', year: String(new Date().getFullYear()), '30d': 'ÚLTIMOS 30 DIAS', '7d': 'ÚLTIMOS 7 DIAS' };
            const pTag = periodLabels[WRAPPED_STATE.period] || String(new Date().getFullYear());
            ctx.font = '700 21px ui-monospace, SFMono-Regular, Consolas, monospace';
            const pTagW = ctx.measureText(pTag).width + 34;
            drawRoundRect(ctx, W - pad - pTagW, y + 2, pTagW, 42, 21);
            ctx.fillStyle = pal.cardBg;
            ctx.fill();
            ctx.strokeStyle = pal.cardBorder;
            ctx.lineWidth = 2;
            ctx.stroke();
            ctx.fillStyle = pal.accent;
            ctx.fillText(pTag, W - pad - pTagW + 17, y + 30);

            y += 78;

            const isCompact = (H <= 1120);
            const isFeed = (H === 1350);
            const handleVal = (document.getElementById('wrpUserHandleInput')?.value || '').trim();

            // --- TEMPLATE A: PÔSTER MEU FESTIVAL / STORIES EM 1-BIT OLED (Design System MPS3) ---
            if (WRAPPED_STATE.template === 'festival') {
                const gridW = 128;
                const gridH = isCompact ? 116 : (isFeed ? 150 : 216);
                const pitch = 8;
                const ox = Math.floor((W - gridW * pitch) / 2);
                const oy = isCompact ? y : (isFeed ? y : 146);
                const oled = buildFestivalLineupOled(st, gridH);
                blitOledToCanvas(ctx, oled, ox, oy, pitch, 0, pal);

                if (handleVal) {
                    let subTag = 'MPS3 MENSAL';
                    if (WRAPPED_STATE.period === '7d') subTag = 'MPS3 SEMANAL';
                    else if (WRAPPED_STATE.period === '24h' || WRAPPED_STATE.period === '1d') subTag = 'MPS3 DIARIO';
                    else if (WRAPPED_STATE.period === 'year' && new Date().getMonth() === 11) subTag = 'MPS3 WRAPPED';
                    ctx.fillStyle = pal.textSub;
                    ctx.font = '700 21px ui-monospace, monospace';
                    ctx.fillText(fitText(ctx, `${handleVal} • ${subTag}`, innerW), pad, H - 24);
                }
                return;
            }

            // --- TEMPLATE B: PÔSTER FULL 1-BIT OLED (COM MÉTRICAS PESSOAIS & REAIS) ---
            if (WRAPPED_STATE.template === 'oled_matrix') {
                const gridW = 128;
                const gridH = isCompact ? 116 : (isFeed ? 150 : 216);
                const pitch = 8;
                const ox = Math.floor((W - gridW * pitch) / 2);
                const oy = isCompact ? y : (isFeed ? y : 146);

                const oled = createOled1BitBuffer(gridW, gridH);
                oled.rect(0, 0, gridW, gridH, false, 1);
                oled.rect(2, 2, gridW - 4, gridH - 4, false, 1);

                oled.drawSdIcon(6, 5, 1);
                oled.text(16, 5, 'MPS3 WRAPPED', 1, 1, 1, 13);
                oled.drawWifiIcon(96, 5, 1);
                oled.drawBatteryIcon(110, 5, 1);
                oled.line(2, 14, gridW - 3, 14, 1);

                oled.rrect(6, 18, gridW - 12, 19, true, 1);
                oled.textCenter(20, 'MINHA JORNADA 1-BIT', 0, 1, 1, 19);
                const subBanner = st.musicalAge > 0 ? `IDADE MUSICAL ${st.musicalAge}` : normAsciiOled(st.persona.badge).slice(0, 18);
                oled.textCenter(28, subBanner, 0, 1, 1, 19);

                oled.textCenter(41, fmtHoursMinutes(st.totalSec), 1, 2, 2, 8);
                oled.textCenter(58, `${st.totalScrobbles} PLAYS * ${st.uniqueArtists} ARTISTAS`, 1, 1, 1, 19);
                oled.line(6, 68, gridW - 7, 68, 1);

                // 2 KPI Boxes com 2px de respiro livre no chão (y=72..100, número scale=2 em y=84..97)
                oled.rrect(6, 72, 56, 29, false, 1);
                oled.text(11, 75, 'SEQUENCIA', 1, 1, 1, 8);
                oled.text(11, 84, `${st.longestStreak || 0}`.slice(0, 4), 1, 2, 1, 4);

                oled.rrect(66, 72, 56, 29, false, 1);
                oled.text(71, 75, 'ZERO-SKIP', 1, 1, 1, 8);
                oled.text(71, 84, `${st.zeroSkipPct}%`.slice(0, 4), 1, 2, 1, 4);

                if (!isCompact) {
                    const maxItems = isFeed ? 2 : 5;
                    const listTop = 105;
                    oled.text(6, listTop, `TOP ${maxItems} MAIS OUVIDAS:`, 1, 1, 1, 19);
                    for (let i = 0; i < maxItems; i++) {
                        const iy = listTop + 11 + i * 10;
                        const tItem = st.topTracks[i];
                        const cnt = tItem ? `${tItem.scrobbles || tItem.plays}X`.slice(0, 4) : '';
                        const tName = tItem ? normAsciiOled(tItem.title).slice(0, 12) : `FAIXA #${i + 1}`;
                        const rowStr = `${i + 1}.${tName}`;
                        if (i === 0) {
                            oled.rrect(6, iy - 2, gridW - 12, 10, true, 1);
                            oled.text(8, iy, rowStr, 0, 1, 1, 15);
                            if (cnt) oled.text(120 - cnt.length * 6, iy, cnt, 0, 1, 1, 4);
                        } else {
                            oled.text(8, iy, rowStr, 1, 1, 1, 15);
                            if (cnt) oled.text(120 - cnt.length * 6, iy, cnt, 1, 1, 1, 4);
                        }
                    }

                    if (!isFeed) {
                        const specTop = listTop + 14 + maxItems * 10 + 1;
                        oled.line(6, specTop, gridW - 7, specTop, 1);
                        const recTag = st.peakCalendarDay.sec > 0 ? `PICO ${st.peakCalendarDay.shortLabel}` : `PICO ${String(st.peakHour).padStart(2, '0')}H`;
                        oled.text(6, specTop + 4, `HABITO 24H (${recTag}):`, 1, 1, 1, 19);

                        const rtaBase = gridH - 22;
                        const buckets16 = [];
                        for (let b = 0; b < 16; b++) {
                            const hIdx = Math.floor((b * 24) / 16);
                            buckets16.push((st.hoursSec[hIdx] || 0) + (st.hoursSec[(hIdx + 1) % 24] || 0));
                        }
                        const maxB16 = Math.max(1, ...buckets16);
                        for (let b = 0; b < 16; b++) {
                            const bh = buckets16[b] > 0 ? Math.max(1, Math.round((buckets16[b] / maxB16) * 4)) : 1;
                            const bx = 8 + b * 7;
                            for (let s = 0; s < bh; s++) {
                                oled.rect(bx, rtaBase - s * 3, 5, 2, true, 1);
                            }
                        }
                    }
                }

                oled.rrect(6, gridH - 17, gridW - 12, 11, true, 1);
                oled.textCenter(gridH - 15, normAsciiOled(st.persona.title).slice(0, 18), 0, 1, 1, 18);

                blitOledToCanvas(ctx, oled, ox, oy, pitch, 0, pal);
                return;
            }

            // --- TEMPLATE C: MÓDULO FÍSICO OLED 128x64 I2C + INSIGHTS COMPORTAMENTAIS ---
            if (WRAPPED_STATE.template === 'oled_bezel') {
                const pcbX = pad;
                const pcbW = innerW;
                const pcbH = isCompact ? 540 : 576;
                drawRoundRect(ctx, pcbX, y, pcbW, pcbH, 22);
                ctx.fillStyle = '#0c1c34';
                ctx.fill();
                ctx.strokeStyle = pal.oledOn || pal.accent;
                ctx.lineWidth = 3;
                ctx.stroke();

                const holes = [
                    [pcbX + 36, y + 36], [pcbX + pcbW - 36, y + 36],
                    [pcbX + 36, y + pcbH - 36], [pcbX + pcbW - 36, y + pcbH - 36]
                ];
                holes.forEach(([hx, hy]) => {
                    ctx.fillStyle = '#eab308';
                    ctx.fillRect(hx - 18, hy - 18, 36, 36);
                    ctx.fillStyle = '#05080e';
                    ctx.fillRect(hx - 9, hy - 9, 18, 18);
                });

                const pins = ['GND', 'VCC', 'SCL', 'SDA'];
                pins.forEach((pName, idx) => {
                    const px = Math.floor(W / 2 - 135 + idx * 90);
                    const py = y + 26;
                    ctx.fillStyle = '#eab308';
                    ctx.fillRect(px - 15, py - 11, 30, 22);
                    ctx.fillStyle = '#090d14';
                    ctx.fillRect(px - 6, py - 4, 12, 8);
                    ctx.fillStyle = '#cbd5e1';
                    ctx.font = '700 15px ui-monospace, monospace';
                    ctx.fillText(pName, px - 14, py + 26);
                });

                const oled1 = buildNowPlayingOled128x64(st);
                const ox1 = Math.floor((W - 128 * 7) / 2);
                const oy1 = y + 72;
                ctx.strokeStyle = pal.oledTop || pal.accent;
                ctx.lineWidth = 2;
                ctx.strokeRect(ox1 - 6, oy1 - 6, 128 * 7 + 12, 64 * 7 + 12);
                blitOledToCanvas(ctx, oled1, ox1, oy1, 7, 0, pal);

                y += pcbH + (isCompact ? 20 : 28);

                if (!isCompact) {
                    const oled2 = buildRtaAndTopTracksOled128x64(st);
                    const ox2 = Math.floor((W - 128 * 7) / 2);
                    const oy2 = y + 10;
                    ctx.strokeStyle = pal.oledOn || pal.accent;
                    ctx.lineWidth = 2.5;
                    ctx.strokeRect(ox2 - 6, oy2 - 6, 128 * 7 + 12, 64 * 7 + 12);
                    blitOledToCanvas(ctx, oled2, ox2, oy2, 7, 0, pal);
                    y += 64 * 7 + 34;
                }

                // Dois Cards U8g2 inferiores ricos & balanceados (Persona & Hábitos + Top Acervo & Discoteca)
                const footMargin = 64;
                const remH = Math.max(160, H - y - footMargin - 16);
                const colGap = 20;
                const colW = (innerW - colGap) / 2;

                // --- CARD ESQUERDO: IDENTIDADE SONORA & HÁBITOS DE FOCO ---
                drawRoundRect(ctx, pad, y, colW, remH, 20);
                ctx.fillStyle = pal.cardBg;
                ctx.fill();
                ctx.strokeStyle = pal.cardBorder;
                ctx.lineWidth = 2;
                ctx.stroke();

                // Header Pill Persona
                const bTxt = st.persona.badge || 'PERFIL DE ESCUTA';
                ctx.font = '800 18px ui-monospace, monospace';
                const bW = ctx.measureText(bTxt).width + 24;
                drawRoundRect(ctx, pad + 20, y + 20, bW, 30, 15);
                ctx.fillStyle = pal.accent;
                ctx.fill();
                ctx.fillStyle = pal.pillText || '#030812';
                ctx.fillText(bTxt, pad + 32, y + 41);

                // Título Persona
                ctx.fillStyle = pal.textMain;
                ctx.font = '900 28px system-ui, -apple-system, sans-serif';
                ctx.fillText(fitText(ctx, st.persona.title, colW - 40), pad + 20, y + 84);

                // Descrição da Persona
                ctx.fillStyle = pal.textSub;
                ctx.font = '500 18px system-ui, -apple-system, sans-serif';
                const descP = fitText(ctx, st.persona.desc || 'Escuta intencional sem algoritmos.', colW - 40);
                ctx.fillText(descP, pad + 20, y + 114);

                let curLeftY = y + 134;

                // Se houver espaço generoso (Stories ou espaço vertical suficiente)
                if (remH >= 280) {
                    ctx.strokeStyle = pal.cardBorder;
                    ctx.lineWidth = 1;
                    ctx.beginPath();
                    ctx.moveTo(pad + 20, curLeftY);
                    ctx.lineTo(pad + colW - 20, curLeftY);
                    ctx.stroke();
                    curLeftY += 24;

                    const metrics = [
                        {
                            label: 'TAXA ZERO-SKIP (FOCO)',
                            val: `${st.zeroSkipPct}% das faixas ouvidas até o fim`,
                            icon: '🎯'
                        },
                        {
                            label: 'RECORDISTA NO REPEAT',
                            val: st.loopObsession ? `"${st.loopObsession.title.slice(0, 20)}" (${st.loopObsession.maxPlaysOneDay}x/dia)` : 'Nenhuma faixa repetida em excesso',
                            icon: '🔄'
                        },
                        {
                            label: 'DIAS ATIVOS EM SEQUÊNCIA',
                            val: `${st.longestStreak} dias consecutivos no MicroSD`,
                            icon: '📅'
                        }
                    ];

                    if (remH >= 420) {
                        metrics.push({
                            label: 'BIORRITMO DIA VS NOITE',
                            val: `☀️ ${st.dayVsNight?.dayLeader ? st.dayVsNight.dayLeader.slice(0, 14) : 'Dia'} • 🌙 ${st.dayVsNight?.nightLeader ? st.dayVsNight.nightLeader.slice(0, 14) : 'Noite'}`,
                            icon: '🌓'
                        });
                    }

                    metrics.forEach((m) => {
                        if (curLeftY + 44 <= y + remH - 16) {
                            ctx.fillStyle = pal.oledTop || pal.accent;
                            ctx.font = '800 15px ui-monospace, monospace';
                            ctx.fillText(`${m.icon} ${m.label}`, pad + 20, curLeftY);
                            ctx.fillStyle = pal.textMain;
                            ctx.font = '600 19px system-ui, sans-serif';
                            ctx.fillText(fitText(ctx, m.val, colW - 40), pad + 20, curLeftY + 24);
                            curLeftY += 54;
                        }
                    });
                }

                // --- CARD DIREITO: TOP ACERVO, DISCOTECA & RECORDES ---
                const rx = pad + colW + colGap;
                drawRoundRect(ctx, rx, y, colW, remH, 20);
                ctx.fillStyle = pal.cardBg;
                ctx.fill();
                ctx.strokeStyle = pal.cardBorder;
                ctx.lineWidth = 2;
                ctx.stroke();

                // Header Pill Recorde
                const rTag = st.peakCalendarDay.sec > 0 ? 'MARATONA RECORDE' : 'DISCOTECA DE OURO';
                ctx.font = '800 18px ui-monospace, monospace';
                const rW = ctx.measureText(rTag).width + 24;
                drawRoundRect(ctx, rx + 20, y + 20, rW, 30, 15);
                ctx.fillStyle = pal.oledTop || pal.accent;
                ctx.fill();
                ctx.fillStyle = pal.pillText || '#030812';
                ctx.fillText(rTag, rx + 32, y + 41);

                // Hero Recorde
                ctx.fillStyle = pal.textMain;
                ctx.font = '900 28px system-ui, -apple-system, sans-serif';
                const rHero = st.peakCalendarDay.sec > 0 ? st.peakCalendarDay.label : 'Acervo Próprio';
                ctx.fillText(fitText(ctx, rHero, colW - 40), rx + 20, y + 84);

                // Sub-hero Recorde
                ctx.fillStyle = pal.textSub;
                ctx.font = '500 18px system-ui, -apple-system, sans-serif';
                const rSub = st.peakCalendarDay.sec > 0
                    ? `${fmtHoursMinutes(st.peakCalendarDay.sec)} num só dia${st.musicalAge > 0 ? ' • Alma de ' + st.musicalAge : ''}`
                    : `${st.totalScrobbles} plays • ${st.uniqueArtists} artistas`;
                ctx.fillText(fitText(ctx, rSub, colW - 40), rx + 20, y + 114);

                let curRightY = y + 134;

                if (remH >= 280) {
                    ctx.strokeStyle = pal.cardBorder;
                    ctx.lineWidth = 1;
                    ctx.beginPath();
                    ctx.moveTo(rx + 20, curRightY);
                    ctx.lineTo(rx + colW - 20, curRightY);
                    ctx.stroke();
                    curRightY += 24;

                    // Top 3 Artistas com Barras de Participação %
                    ctx.fillStyle = pal.oledTop || pal.accent;
                    ctx.font = '800 15px ui-monospace, monospace';
                    ctx.fillText('👑 TOP ARTISTAS & % DO TEMPO', rx + 20, curRightY);
                    curRightY += 22;

                    const top3 = st.topArtists.slice(0, 3);
                    top3.forEach((art, idx) => {
                        if (curRightY + 36 <= y + remH - 90) {
                            ctx.fillStyle = pal.textMain;
                            ctx.font = '700 18px system-ui, sans-serif';
                            const artLine = `${idx + 1}. ${art.name}`;
                            ctx.fillText(fitText(ctx, artLine, colW - 120), rx + 20, curRightY + 14);

                            ctx.fillStyle = pal.accent;
                            ctx.font = '800 16px ui-monospace, monospace';
                            const shareTxt = `${art.sharePct}%`;
                            ctx.fillText(shareTxt, rx + colW - 20 - ctx.measureText(shareTxt).width, curRightY + 14);

                            // Mini barra horizontal
                            curRightY += 22;
                            ctx.fillStyle = 'rgba(255, 255, 255, 0.08)';
                            ctx.fillRect(rx + 20, curRightY, colW - 40, 5);
                            ctx.fillStyle = pal.accent;
                            const barFillW = Math.max(8, Math.round(((art.sharePct || 5) / 100) * (colW - 40)));
                            ctx.fillRect(rx + 20, curRightY, barFillW, 5);
                            curRightY += 16;
                        }
                    });

                    // Álbum #1 de Cabeceira no rodapé do card direito
                    if (st.topAlbums[0] && curRightY + 54 <= y + remH - 12) {
                        ctx.strokeStyle = pal.cardBorder;
                        ctx.lineWidth = 1;
                        ctx.beginPath();
                        ctx.moveTo(rx + 20, curRightY);
                        ctx.lineTo(rx + colW - 20, curRightY);
                        ctx.stroke();
                        curRightY += 20;

                        ctx.fillStyle = pal.oledTop || pal.accent;
                        ctx.font = '800 15px ui-monospace, monospace';
                        ctx.fillText('💿 DISCO DE CABECEIRA', rx + 20, curRightY);
                        curRightY += 20;

                        ctx.fillStyle = pal.textMain;
                        ctx.font = '700 18px system-ui, sans-serif';
                        ctx.fillText(fitText(ctx, `"${st.topAlbums[0].name}"`, colW - 40), rx + 20, curRightY);

                        ctx.fillStyle = pal.textSub;
                        ctx.font = '500 16px system-ui, sans-serif';
                        const albRitual = st.topAlbums[0].fullSessions > 0
                            ? `${st.topAlbums[0].artist} • ${st.topAlbums[0].fullSessions}x ouvido do início ao fim`
                            : `${st.topAlbums[0].artist} • ${st.topAlbums[0].scrobbles} reproduções`;
                        ctx.fillText(fitText(ctx, albRitual, colW - 40), rx + 20, curRightY + 20);
                    }
                }

                const footY = H - 56;
                ctx.fillStyle = pal.textSub;
                ctx.font = '700 22px ui-monospace, monospace';
                const leftFooter = handleVal
                    ? `${handleVal} • MPS3 OLED 128X64 PLAYER`
                    : 'MPS3 • RETROSPECTIVA PRIVADA NO MICROSD';
                ctx.fillText(fitText(ctx, leftFooter, innerW - 250), pad, footY);

                ctx.fillStyle = pal.oledTop || pal.accent;
                ctx.font = '800 22px ui-monospace, monospace';
                const rightBadge = `PICO ${String(st.peakHour).padStart(2, '0')}H • ${st.longestStreak}D STREAK`;
                ctx.fillText(rightBadge, W - pad - ctx.measureText(rightBadge).width, footY);
                return;
            }

            // 3. Bloco Hero para Templates HD (albums_era, summary, top10)
            const heroH = isCompact ? 205 : (isFeed ? 245 : 300);
            drawRoundRect(ctx, pad, y, innerW, heroH, 28);
            ctx.fillStyle = pal.cardBg;
            ctx.fill();
            ctx.strokeStyle = pal.cardBorder;
            ctx.lineWidth = 2.5;
            ctx.stroke();

            if (WRAPPED_STATE.template === 'albums_era') {
                ctx.fillStyle = pal.accent;
                ctx.font = '800 22px system-ui, sans-serif';
                ctx.fillText('IDADE MUSICAL & ÁLBUNS DE CABECEIRA', pad + 36, y + 48);

                ctx.fillStyle = pal.textMain;
                ctx.font = `900 ${isCompact ? 72 : 90}px system-ui, sans-serif`;
                const eraBig = st.musicalAge > 0
                    ? `Alma de ${st.musicalAge}`
                    : (st.topGenres[0]?.name || fmtHoursMinutes(st.totalSec));
                ctx.fillText(fitText(ctx, eraBig, innerW - 72), pad + 36, y + (isCompact ? 132 : 154));

                ctx.fillStyle = pal.textSub;
                ctx.font = '600 25px system-ui, sans-serif';
                const decSummary = st.topDecade
                    ? `Domínio dos ${st.topDecade.label} (${st.topDecade.pct}%) • ${st.uniqueAlbums} álbuns ouvidos`
                    : `${st.uniqueAlbums} álbuns • ${st.fullAlbumSessions} sessões contínuas sem pular`;
                ctx.fillText(fitText(ctx, decSummary, innerW - 72), pad + 36, y + (isCompact ? 176 : 204));

                if (!isCompact) {
                    const topGenStr = st.topGenres.slice(0, 3).map(g => `${g.name} (${g.pct}%)`).join('  •  ');
                    ctx.fillStyle = pal.accent2;
                    ctx.font = '700 23px system-ui, sans-serif';
                    ctx.fillText(fitText(ctx, topGenStr || `${st.zeroSkipPct}% Escuta Intencional Zero-Skip`, innerW - 72), pad + 36, y + 258);
                }
            } else {
                ctx.font = '800 20px system-ui, sans-serif';
                const bTxt = st.persona.badge;
                const bW = ctx.measureText(bTxt).width + 32;
                drawRoundRect(ctx, pad + 34, y + 28, bW, 36, 18);
                ctx.fillStyle = pal.accent;
                ctx.fill();
                ctx.fillStyle = pal.pillText;
                ctx.fillText(bTxt, pad + 50, y + 53);

                ctx.fillStyle = pal.textMain;
                ctx.font = `900 ${isCompact ? 78 : 102}px system-ui, sans-serif`;
                ctx.fillText(fmtHoursMinutes(st.totalSec), pad + 34, y + (isCompact ? 142 : 168));

                ctx.fillStyle = pal.accent;
                ctx.font = `800 ${isCompact ? 28 : 34}px system-ui, sans-serif`;
                ctx.fillText(fitText(ctx, st.persona.title, innerW - 68), pad + 34, y + (isCompact ? 182 : 218));

                if (!isCompact && H >= 1500) {
                    ctx.fillStyle = pal.textSub;
                    ctx.font = '500 24px system-ui, sans-serif';
                    ctx.fillText(fitText(ctx, `${st.totalScrobbles} plays • ${st.uniqueArtists} artistas • ${st.uniqueAlbums} álbuns • ${st.zeroSkipPct}% sem pular`, innerW - 68), pad + 34, y + 264);
                }
            }

            y += heroH + (isCompact ? 22 : 30);

            // 4. Corpo Principal por Template HD
            if (WRAPPED_STATE.template === 'top10') {
                const maxRows = isCompact ? 6 : (isFeed ? 8 : 10);
                const boxH = H - y - 135;
                drawRoundRect(ctx, pad, y, innerW, boxH, 26);
                ctx.fillStyle = pal.cardBg;
                ctx.fill();
                ctx.strokeStyle = pal.cardBorder;
                ctx.stroke();

                ctx.fillStyle = pal.accent;
                ctx.font = '800 25px system-ui, sans-serif';
                ctx.fillText(`TOP ${maxRows} FAIXAS MAIS OUVIDAS`, pad + 34, y + 48);

                const rowH = Math.floor((boxH - 72) / maxRows);
                const list = st.topTracks.slice(0, maxRows);
                list.forEach((t, idx) => {
                    const ry = y + 66 + idx * rowH;
                    drawRoundRect(ctx, pad + 34, ry + 6, 44, 44, 12);
                    ctx.fillStyle = idx === 0 ? pal.accent : 'rgba(255,255,255,0.1)';
                    ctx.fill();
                    ctx.fillStyle = idx === 0 ? pal.pillText : pal.textMain;
                    ctx.font = '800 24px system-ui, sans-serif';
                    ctx.fillText(String(idx + 1), pad + (idx + 1 >= 10 ? 42 : 49), ry + 36);

                    ctx.fillStyle = pal.textMain;
                    ctx.font = '700 27px system-ui, sans-serif';
                    ctx.fillText(fitText(ctx, t.title, innerW - 280), pad + 96, ry + 30);

                    ctx.fillStyle = pal.textSub;
                    ctx.font = '500 21px system-ui, sans-serif';
                    const metaSub = [t.artist, t.album, t.year ? String(t.year) : ''].filter(Boolean).join(' • ');
                    ctx.fillText(fitText(ctx, metaSub, innerW - 280), pad + 96, ry + 56);

                    const playsTxt = `${t.scrobbles || t.plays}x  •  ${fmtHoursMinutes(t.sec)}`;
                    ctx.fillStyle = pal.accent;
                    ctx.font = '800 22px ui-monospace, monospace';
                    const dw = ctx.measureText(playsTxt).width;
                    ctx.fillText(playsTxt, pad + innerW - 34 - dw, ry + 38);
                });
            } else if (WRAPPED_STATE.template === 'albums_era') {
                // Barra visual de Décadas + Top Álbuns Completos
                if (st.decades.length > 0 && !isCompact) {
                    const decH = 148;
                    drawRoundRect(ctx, pad, y, innerW, decH, 24);
                    ctx.fillStyle = pal.cardBg;
                    ctx.fill();
                    ctx.strokeStyle = pal.cardBorder;
                    ctx.stroke();

                    ctx.fillStyle = pal.accent;
                    ctx.font = '800 21px system-ui, sans-serif';
                    ctx.fillText('LINHA DO TEMPO DAS DÉCADAS OUVIDAS', pad + 32, y + 40);

                    const barW = innerW - 64;
                    let curBx = pad + 32;
                    st.decades.slice(0, 5).forEach((d, idx) => {
                        const segW = Math.max(24, Math.round((d.pct / 100) * barW));
                        const drawW = Math.min(segW, pad + 32 + barW - curBx);
                        if (drawW > 4) {
                            ctx.fillStyle = idx === 0 ? pal.accent : d.color;
                            ctx.fillRect(curBx, y + 58, drawW - 3, 28);
                            curBx += drawW;
                        }
                    });

                    ctx.font = '700 20px system-ui, sans-serif';
                    let lx = pad + 32;
                    st.decades.slice(0, 4).forEach((d, idx) => {
                        ctx.fillStyle = idx === 0 ? pal.accent : pal.textSub;
                        const lbl = `${d.label}: ${d.pct}%`;
                        ctx.fillText(lbl, lx, y + 122);
                        lx += ctx.measureText(lbl).width + 36;
                    });
                    y += decH + 24;
                }

                const remH = H - y - 135;
                drawRoundRect(ctx, pad, y, innerW, remH, 24);
                ctx.fillStyle = pal.cardBg;
                ctx.fill();
                ctx.strokeStyle = pal.cardBorder;
                ctx.stroke();

                ctx.fillStyle = pal.accent;
                ctx.font = '800 24px system-ui, sans-serif';
                ctx.fillText('TOP ÁLBUNS MAIS OUVIDOS', pad + 32, y + 48);

                const nAlb = isCompact ? 4 : (isFeed ? 4 : 6);
                const albList = st.topAlbums.length ? st.topAlbums.slice(0, nAlb) : st.topTracks.slice(0, nAlb).map(t => ({
                    name: t.title, artist: t.artist, year: t.year, sec: t.sec, fullSessions: 0, scrobbles: t.scrobbles || t.plays
                }));
                const rStep = Math.floor((remH - 72) / Math.max(1, nAlb));
                albList.forEach((alb, idx) => {
                    const ry = y + 66 + idx * rStep;
                    ctx.fillStyle = pal.accent;
                    ctx.font = '900 28px ui-monospace, monospace';
                    ctx.fillText(`0${idx + 1}`, pad + 32, ry + 34);

                    ctx.fillStyle = pal.textMain;
                    ctx.font = '700 28px system-ui, sans-serif';
                    ctx.fillText(fitText(ctx, alb.name, innerW - 300), pad + 92, ry + 32);

                    ctx.fillStyle = pal.textSub;
                    ctx.font = '500 21px system-ui, sans-serif';
                    const subA = `${alb.artist}${alb.year ? ' (' + alb.year + ')' : ''}${alb.fullSessions > 0 ? ' • ' + alb.fullSessions + 'x completo' : ''}`;
                    ctx.fillText(fitText(ctx, subA, innerW - 300), pad + 92, ry + 60);

                    const tm = fmtHoursMinutes(alb.sec);
                    ctx.fillStyle = pal.accent;
                    ctx.font = '800 24px ui-monospace, monospace';
                    ctx.fillText(tm, pad + innerW - 32 - ctx.measureText(tm).width, ry + 38);
                });
            } else {
                // RESUMO GERAL: Top Artistas (% Share) + Top Músicas (Plays) + 4 Destaques Pessoais + Onda 24h
                const listRows = isCompact ? 4 : 5;
                const colGap = 24;
                const colW = (innerW - colGap) / 2;
                const listsH = isCompact ? 380 : (isFeed ? 470 : 590);

                drawRoundRect(ctx, pad, y, colW, listsH, 24);
                ctx.fillStyle = pal.cardBg;
                ctx.fill();
                ctx.strokeStyle = pal.cardBorder;
                ctx.stroke();

                ctx.fillStyle = pal.accent;
                ctx.font = '800 23px system-ui, sans-serif';
                ctx.fillText('TOP ARTISTAS', pad + 28, y + 46);

                const aStep = Math.floor((listsH - 68) / listRows);
                st.topArtists.slice(0, listRows).forEach((a, idx) => {
                    const ry = y + 66 + idx * aStep;
                    ctx.fillStyle = pal.accent;
                    ctx.font = '900 28px system-ui, sans-serif';
                    ctx.fillText(`${idx + 1}`, pad + 28, ry + 32);

                    ctx.fillStyle = pal.textMain;
                    ctx.font = '700 25px system-ui, sans-serif';
                    ctx.fillText(fitText(ctx, a.name, colW - 92), pad + 66, ry + 30);

                    ctx.fillStyle = pal.textSub;
                    ctx.font = '600 20px system-ui, sans-serif';
                    ctx.fillText(`${a.sharePct}% do tempo • ${fmtHoursMinutes(a.sec)}`, pad + 66, ry + 57);
                });

                const rx = pad + colW + colGap;
                drawRoundRect(ctx, rx, y, colW, listsH, 24);
                ctx.fillStyle = pal.cardBg;
                ctx.fill();
                ctx.strokeStyle = pal.cardBorder;
                ctx.stroke();

                ctx.fillStyle = pal.accent;
                ctx.font = '800 23px system-ui, sans-serif';
                ctx.fillText('TOP MÚSICAS', rx + 28, y + 46);

                st.topTracks.slice(0, listRows).forEach((t, idx) => {
                    const ry = y + 66 + idx * aStep;
                    ctx.fillStyle = pal.accent;
                    ctx.font = '900 28px system-ui, sans-serif';
                    ctx.fillText(`${idx + 1}`, rx + 28, ry + 32);

                    ctx.fillStyle = pal.textMain;
                    ctx.font = '700 25px system-ui, sans-serif';
                    ctx.fillText(fitText(ctx, t.title, colW - 92), rx + 66, ry + 30);

                    ctx.fillStyle = pal.textSub;
                    ctx.font = '600 20px system-ui, sans-serif';
                    ctx.fillText(fitText(ctx, `${t.artist} • ${t.scrobbles || t.plays}x`, colW - 92), rx + 66, ry + 57);
                });

                y += listsH + (isCompact ? 18 : 28);

                const extraAvail = H - y - 135;
                if (extraAvail >= 150) {
                    drawRoundRect(ctx, pad, y, innerW, extraAvail, 24);
                    ctx.fillStyle = pal.cardBg;
                    ctx.fill();
                    ctx.strokeStyle = pal.cardBorder;
                    ctx.stroke();

                    const pillSpecs = [
                        ['DIA RECORDE', st.peakCalendarDay.sec > 0 ? st.peakCalendarDay.label : '--'],
                        ['ERA MUSICAL', st.musicalAge > 0 ? `Alma ${st.musicalAge}` : (st.topGenres[0]?.name || 'Variado')],
                        ['ZERO-SKIP', `${st.zeroSkipPct}% foco`],
                        ['SEQUÊNCIA', `${st.longestStreak} dias`]
                    ];
                    const cellW = innerW / 4;
                    pillSpecs.forEach((sp, idx) => {
                        const cx = pad + idx * cellW + 26;
                        ctx.fillStyle = pal.textSub;
                        ctx.font = '700 18px system-ui, sans-serif';
                        ctx.fillText(sp[0], cx, y + 46);

                        ctx.fillStyle = idx === 1 ? pal.accent : pal.textMain;
                        ctx.font = '800 30px system-ui, sans-serif';
                        ctx.fillText(fitText(ctx, sp[1], cellW - 36), cx, y + 86);
                    });

                    if (extraAvail >= 280) {
                        const waveTop = y + 128;
                        const waveH = extraAvail - 165;
                        ctx.fillStyle = pal.textSub;
                        ctx.font = '700 19px system-ui, sans-serif';
                        ctx.fillText(`RELÓGIO DE ESCUTA 24H (PICO ÀS ${String(st.peakHour).padStart(2, '0')}H • ${WRP_WDAYS_FULL[st.peakWday].toUpperCase()})`, pad + 26, waveTop);

                        const maxHr = Math.max(1, ...st.hoursSec);
                        const barAreaW = innerW - 52;
                        const gap = 8;
                        const bw = (barAreaW - gap * 23) / 24;
                        const baseY = y + extraAvail - 28;
                        const usableH = waveH - 28;
                        for (let h = 0; h < 24; h++) {
                            const bh = Math.max(10, Math.round((st.hoursSec[h] / maxHr) * usableH));
                            drawRoundRect(ctx, pad + 26 + h * (bw + gap), baseY - bh, bw, bh, 6);
                            ctx.fillStyle = (h === st.peakHour) ? pal.accent : (pal.accent2 + '99');
                            ctx.fill();
                        }
                    }
                }
            }

            // 5. Rodapé Oficial MPS3 + Assinatura do Usuário
            const footY = H - 64;
            ctx.fillStyle = pal.textSub;
            ctx.font = '700 22px system-ui, sans-serif';
            const leftFooter = handleVal
                ? `${handleVal}  •  Escutado Offline no MPS3 Player`
                : 'Retrospectiva Privada • MPS3 Digital Audio Player';
            ctx.fillText(fitText(ctx, leftFooter, innerW - 260), pad, footY);

            ctx.fillStyle = pal.accent;
            ctx.font = '800 22px ui-monospace, monospace';
            const rightBadge = st.musicalAge > 0 ? `ALMA ${st.musicalAge}` : `${st.zeroSkipPct}% ZERO-SKIP`;
            ctx.fillText(rightBadge, W - pad - ctx.measureText(rightBadge).width, footY);
        }

        function canvasToPngBlob() {
            renderWrappedCanvas();
            const canvas = document.getElementById('wrappedCanvas');
            return new Promise((resolve, reject) => {
                canvas.toBlob(blob => {
                    if (blob) {
                        WRAPPED_STATE.lastBlob = blob;
                        resolve(blob);
                    } else {
                        reject(new Error('Falha ao codificar PNG'));
                    }
                }, 'image/png');
            });
        }

        // --- 7. PONTE INTELIGENTE PC -> SMARTPHONE & COMPARTILHAMENTO NATIVO ---
        function isMobileClientDevice() {
            const ua = navigator.userAgent || '';
            return /Android|iPhone|iPad|iPod|Mobile/i.test(ua) ||
                   (navigator.maxTouchPoints > 1 && window.innerWidth < 900);
        }

        function updateShareButtonsVisibility() {
            const btnNative = document.getElementById('btnWrpNativeShare');
            const btnQr     = document.getElementById('btnWrpSendPhoneQr');
            if (!btnNative || !btnQr) return;

            const mobile = isMobileClientDevice();
            let supportsFileShare = false;
            try {
                if (navigator.canShare && typeof File !== 'undefined') {
                    const testFile = new File([new Uint8Array([137,80,78,71])], 't.png', { type: 'image/png' });
                    supportsFileShare = navigator.canShare({ files: [testFile] });
                }
            } catch (e) {}

            if (mobile && supportsFileShare) {
                btnNative.style.display = 'flex';
                btnQr.innerHTML = '<span>📱 Gerar QR Code p/ Outro Celular</span>';
                btnQr.style.background = 'var(--bg-card)';
                btnQr.style.color = 'var(--text-primary)';
                btnQr.style.border = '1px solid var(--border-color)';
                btnQr.style.boxShadow = 'none';
            } else {
                btnNative.style.display = supportsFileShare ? 'flex' : 'none';
                btnQr.style.display = 'flex';
            }
        }

        async function downloadWrappedPng() {
            const blob = await canvasToPngBlob();
            const url = URL.createObjectURL(blob);
            const a = document.createElement('a');
            a.href = url;
            a.download = `MPS3_Wrapped_${WRAPPED_STATE.format}_${WRAPPED_STATE.cardTheme}.png`;
            document.body.appendChild(a);
            a.click();
            a.remove();
            setTimeout(() => URL.revokeObjectURL(url), 5000);
        }

        async function copyWrappedImageToClipboard() {
            const btn = document.getElementById('btnWrpCopyClipboard');
            const origTxt = btn.textContent;
            try {
                const blob = await canvasToPngBlob();
                if (!navigator.clipboard || typeof ClipboardItem === 'undefined') {
                    throw new Error('Clipboard API indisponível neste navegador');
                }
                await navigator.clipboard.write([new ClipboardItem({ 'image/png': blob })]);
                btn.textContent = '✓ Copiado! Cole com Ctrl+V';
                setTimeout(() => { btn.textContent = origTxt; }, 3000);
            } catch (err) {
                alert('Seu navegador bloqueou a cópia direta (comum em HTTP sem HTTPS). Use o botão QR Code para enviar direto ao Smartphone ou Baixar PNG!');
            }
        }

        async function shareWrappedNative() {
            try {
                const blob = await canvasToPngBlob();
                const file = new File([blob], `MPS3_Wrapped_${WRAPPED_STATE.format}.png`, { type: 'image/png' });
                if (navigator.canShare && navigator.canShare({ files: [file] })) {
                    await navigator.share({
                        files: [file],
                        title: 'Meu MPS3 Wrapped',
                        text: 'Minha retrospectiva musical 100% offline no MPS3 Hi-Res Player!'
                    });
                } else {
                    await downloadWrappedPng();
                }
            } catch (err) {
                if (err && err.name !== 'AbortError') {
                    await downloadWrappedPng();
                }
            }
        }

        function resolvePhoneReachableOrigin() {
            // Prioriza o IP numérico retornado por /api/status (evita localhost e evita falha de mDNS .local no Android!)
            if (WRAPPED_STATE.deviceIp && WRAPPED_STATE.deviceIp.trim()) {
                const ip = WRAPPED_STATE.deviceIp.trim();
                return ip.startsWith('http') ? ip : `http://${ip}`;
            }
            return window.location.origin;
        }

        async function sendWrappedToPhoneViaMps3() {
            const btn = document.getElementById('btnWrpSendPhoneQr');
            const qrBox = document.getElementById('wrpQrBridgeBox');
            const qrSvgBox = document.getElementById('wrpQrSvgContainer');
            const statusMsg = document.getElementById('wrpQrStatusMsg');
            const urlDisp = document.getElementById('wrpQrUrlDisplay');

            const oldHtml = btn.innerHTML;
            btn.disabled = true;
            btn.innerHTML = '<span>⏳ Preparando ponte com o Smartphone...</span>';

            try {
                // Atualiza IP real do dispositivo antes de gerar o link
                try {
                    const stRes = await fetch('/api/status');
                    if (stRes.ok) {
                        const stJson = await stRes.json();
                        if (stJson.ip) WRAPPED_STATE.deviceIp = stJson.ip;
                        if (stJson.wifi_mode) WRAPPED_STATE.wifiMode = stJson.wifi_mode;
                    }
                } catch (e) {}

                const blob = await canvasToPngBlob();
                const shareRelPath = '/.mps3_analytics/share/mps3_wrapped.png';
                const upRes = await fetch('/api/upload?path=' + encodeURIComponent(shareRelPath) + '&idx=1&count=1&batchTotal=' + blob.size, {
                    method: 'PUT',
                    body: blob
                });
                if (!upRes.ok) {
                    throw new Error('Falha ao gravar imagem temporária no MPS3 (HTTP ' + upRes.status + ')');
                }

                const verTag = Date.now().toString(36);
                const origin = resolvePhoneReachableOrigin();
                const shareUrl = `${origin}/?share=wrapped&v=${verTag}`;

                const svgMarkup = generateQrSvg(shareUrl);
                if (svgMarkup) {
                    qrSvgBox.innerHTML = svgMarkup;
                } else {
                    qrSvgBox.innerHTML = `<img src="/api/qr?text=${encodeURIComponent(shareUrl)}" alt="QR Code">`;
                }

                const netHint = WRAPPED_STATE.wifiMode === 'AP'
                    ? 'Conecte o celular no Wi-Fi Hotspot <strong>MPS3</strong> e aponte a câmera:'
                    : 'Com o smartphone conectado no <strong>mesmo Wi-Fi</strong>, aponte a câmera para abrir e compartilhar nos Stories em 1 toque:';
                statusMsg.innerHTML = netHint;
                urlDisp.textContent = shareUrl;
                qrBox.style.display = 'block';
                qrBox.scrollIntoView({ behavior: 'smooth', block: 'nearest' });
            } catch (err) {
                alert('Erro ao preparar envio ao celular: ' + (err.message || err));
            } finally {
                btn.disabled = false;
                btn.innerHTML = oldHtml;
            }
        }

        // --- 8. TELA MOBILE DEDICADA (?share=wrapped) AO ABRIR PELO QR CODE ---
        async function checkMobileShareLandingOnBoot() {
            try {
                const params = new URLSearchParams(window.location.search);
                if (params.get('share') !== 'wrapped') return;

                const isVideo = (params.get('media') === 'video');
                const v = params.get('v') || Date.now();
                const filePath = isVideo ? '/.mps3_analytics/share/mps3_wrapped.mp4' : '/.mps3_analytics/share/mps3_wrapped.png';
                const fileUrl = `/api/download?path=${encodeURIComponent(filePath)}&v=${encodeURIComponent(v)}`;
                const dlUrl  = `${fileUrl}&dl=1`;

                const landing = document.getElementById('wrappedMobileLanding');
                const imgEl   = document.getElementById('wrappedMobileImg');
                const vidEl   = document.getElementById('wrappedMobileVideo');
                const dlBtn   = document.getElementById('btnMobLandingDownload');

                if (landing) {
                    if (isVideo && vidEl) {
                        vidEl.src = fileUrl;
                        vidEl.style.display = 'block';
                        if (imgEl) imgEl.style.display = 'none';
                        if (dlBtn) {
                            dlBtn.href = dlUrl;
                            dlBtn.download = 'MPS3_Wrapped.mp4';
                            dlBtn.textContent = '💾 Baixar Vídeo Original (MP4)';
                        }
                    } else if (imgEl) {
                        imgEl.src = fileUrl;
                        imgEl.style.display = 'block';
                        if (vidEl) vidEl.style.display = 'none';
                        if (dlBtn) {
                            dlBtn.href = dlUrl;
                            dlBtn.download = 'MPS3_Wrapped.png';
                            dlBtn.textContent = '💾 Baixar Imagem Original (PNG HD)';
                        }
                    }
                    landing.style.display = 'flex';
                }
            } catch (e) {}
        }

        function closeMobileShareLanding() {
            const landing = document.getElementById('wrappedMobileLanding');
            if (landing) landing.style.display = 'none';
            if (window.history && window.history.replaceState) {
                window.history.replaceState({}, document.title, window.location.pathname);
            }
        }

        async function shareMobileLandingFile() {
            const btn = document.getElementById('btnMobLandingShare');
            const imgEl = document.getElementById('wrappedMobileImg');
            const vidEl = document.getElementById('wrappedMobileVideo');
            const isVideo = (vidEl && vidEl.style.display !== 'none' && vidEl.src);
            const orig = btn.innerHTML;
            btn.innerHTML = '<span>⏳ Abrindo menu de compartilhamento...</span>';
            try {
                const src = isVideo ? vidEl.src : (imgEl ? imgEl.src : '');
                const res = await fetch(src);
                const blob = await res.blob();
                const fname = isVideo ? 'MPS3_Wrapped.mp4' : 'MPS3_Wrapped.png';
                const ftype = isVideo ? 'video/mp4' : 'image/png';
                const file = new File([blob], fname, { type: ftype });
                if (navigator.canShare && navigator.canShare({ files: [file] })) {
                    await navigator.share({
                        files: [file],
                        title: 'Meu MPS3 Wrapped',
                        text: isVideo ? 'Minha retrospectiva animada 1-bit no MPS3 Hi-Res Player!' : 'Minha retrospectiva musical 100% offline no MPS3!'
                    });
                } else {
                    window.location.href = document.getElementById('btnMobLandingDownload').href;
                }
            } catch (err) {
                if (!err || err.name !== 'AbortError') {
                    window.location.href = document.getElementById('btnMobLandingDownload').href;
                }
            } finally {
                btn.innerHTML = orig;
            }
        }

        // --- 9. CONTROLES DE NAVEGAÇÃO DO MODAL ---
        function openWrappedModal() {
            document.getElementById('wrappedModal').style.display = 'flex';
            updateShareButtonsVisibility();
            refreshWrappedFromSd();
        }

        function closeWrappedModal() {
            document.getElementById('wrappedModal').style.display = 'none';
        }

        function switchWrappedTab(tab) {
            const isStudio = (tab === 'studio');
            document.getElementById('wrpStatsPanel').style.display = isStudio ? 'none' : 'block';
            document.getElementById('wrpStudioPanel').style.display = isStudio ? 'block' : 'none';
            document.getElementById('wrpTabStatsBtn').classList.toggle('active', !isStudio);
            document.getElementById('wrpTabStudioBtn').classList.toggle('active', isStudio);
            if (isStudio) {
                updateShareButtonsVisibility();
                renderWrappedCanvas();
            }
        }

        function setWrappedPeriod(p) {
            WRAPPED_STATE.period = p;
            document.querySelectorAll('#wrpPeriodGroup .wrp-pill').forEach(b => {
                b.classList.toggle('active', b.getAttribute('data-period') === p);
            });
            refreshWrappedDashboardUI();
        }

        function setWrappedCategory(c) {
            WRAPPED_STATE.category = c;
            refreshWrappedDashboardUI();
        }

        function toggleWrappedDemoMode() {
            WRAPPED_STATE.useDemo = !WRAPPED_STATE.useDemo;
            refreshWrappedDashboardUI();
        }

        function setWrappedFormat(fmt) {
            WRAPPED_STATE.format = fmt;
            document.querySelectorAll('#wrpFormatGroup .wrp-pill').forEach(b => {
                b.classList.toggle('active', b.getAttribute('data-fmt') === fmt);
            });
            document.getElementById('wrpQrBridgeBox').style.display = 'none';
            renderWrappedCanvas();
        }

        function setWrappedCardTheme(th) {
            WRAPPED_STATE.cardTheme = th;
            document.querySelectorAll('#wrpThemeGroup .wrp-pill').forEach(b => {
                b.classList.toggle('active', b.getAttribute('data-wtheme') === th);
            });
            const qrBox = document.getElementById('wrpQrBridgeBox');
            if (qrBox) qrBox.style.display = 'none';
            if (th.startsWith('oled_') && typeof setOledColor === 'function') {
                setOledColor(th.replace('oled_', ''), false);
            }
            renderWrappedCanvas();
        }

        function setWrappedTemplate(tpl) {
            WRAPPED_STATE.template = (tpl === 'oled_bezel') ? 'festival' : tpl;
            document.querySelectorAll('#wrpTemplateGroup .wrp-pill').forEach(b => {
                b.classList.toggle('active', b.getAttribute('data-wtpl') === WRAPPED_STATE.template);
            });
            const qrBox = document.getElementById('wrpQrBridgeBox');
            if (qrBox) qrBox.style.display = 'none';
            renderWrappedCanvas();
        }

        // Inicialização de Listeners do Módulo Wrapped
        (function initWrappedModuleBindings() {
            const wBtn = document.getElementById('wrappedBtn');
            if (wBtn) wBtn.addEventListener('click', openWrappedModal);
            const cBtn = document.getElementById('btnCloseWrapped');
            if (cBtn) cBtn.addEventListener('click', closeWrappedModal);
            const modal = document.getElementById('wrappedModal');
            if (modal) {
                modal.addEventListener('click', (ev) => {
                    if (ev.target === modal) closeWrappedModal();
                });
            }
            try {
                const savedOled = (typeof localStorage !== 'undefined' && localStorage.getItem('mps3_oled_color')) || 'blue';
                if (WRP_PALETTES['oled_' + savedOled]) {
                    WRAPPED_STATE.cardTheme = 'oled_' + savedOled;
                }
            } catch (e) {}
            renderHeaderOled128x64();
            checkMobileShareLandingOnBoot();
        })();
