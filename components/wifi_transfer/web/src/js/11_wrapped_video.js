/* ==========================================================================
 * MÓDULO JS 11: MPS3 Wrapped — Motor de Vídeos Curtos OLED 1-Bit (4s/6s/8s/15s)
 * Storyboard Dinâmico, Primitivas 1-Bit, Blitter Rápido, Muxer MP4 & WebCodecs
 * ========================================================================== */

        const WRAPPED_VIDEO_STATE = {
            mode: 'image',          // 'image' | 'video'
            preset: 'rapido',       // 'flash' (4s) | 'rapido' (6s) | 'story' (8s) | 'completo' (15s)
            storyboardStyle: 'festival', // 'festival' | 'storyboard'
            modules: {
                stage_time: true,
                headliner: true,
                coheadliners: true,
                special_guests: true,
                album: true,
                clock24h: true,
                persona: true
            },
            isPreviewing: false,
            previewRafId: null,
            previewStartTs: 0,
            isEncoding: false,
            cancelRequested: false,
            lastVideoBlob: null,
            lastVideoUrl: null,
            encoderEngine: 'none'   // 'webcodecs' | 'mediarecorder'
        };

        const WRP_VIDEO_CFG = {
            fps: 30,
            presets: {
                flash:    { id: 'flash',    label: '⚡ Flash',           sec: 4,  frames: 120, desc: 'Status WhatsApp / Teaser rápido' },
                rapido:   { id: 'rapido',   label: '🚀 Rápido (Padrão)', sec: 6,  frames: 180, desc: 'Story dinâmico que prende' },
                story:    { id: 'story',    label: '🎬 Story',           sec: 8,  frames: 240, desc: 'Story completo e confortável' },
                completo: { id: 'completo', label: '🎞️ Completo',        sec: 15, frames: 450, desc: 'Reels / Retrospectiva íntegra' }
            }
        };

        // --- 1. MATRIZ DE DITHER BAYER 4x4 (DISSOLVE ORDENADO 1-BIT PURO) ---
        const WRP_BAYER_4X4 = [
            [  0,  8,  2, 10 ],
            [ 12,  4, 14,  6 ],
            [  3, 11,  1,  9 ],
            [ 15,  7, 13,  5 ]
        ];

        function wrpBayerThreshold(x, y) {
            return (WRP_BAYER_4X4[(y & 3)][(x & 3)] + 0.5) / 16.0;
        }

        // --- 2. EASING E MATEMÁTICA TEMPORAL ---
        function wrpEaseOutCubic(t) {
            const p = Math.max(0, Math.min(1, t));
            return 1 - Math.pow(1 - p, 3);
        }

        function wrpEaseInOutQuad(t) {
            const p = Math.max(0, Math.min(1, t));
            return p < 0.5 ? 2 * p * p : 1 - Math.pow(-2 * p + 2, 2) / 2;
        }

        function wrpTypewriter(str, progress) {
            if (!str) return '';
            const p = Math.max(0, Math.min(1, progress));
            const total = str.length;
            const count = Math.min(total, Math.floor(p * (total + 1)));
            const slice = str.slice(0, count);
            const cursor = (p < 1.0 && (Math.floor(p * 24) % 2 === 0)) ? '_' : '';
            return slice + cursor;
        }

        function wrpRollingCounter(fromVal, toVal, progress) {
            const p = wrpEaseOutCubic(progress);
            return Math.round(fromVal + (toVal - fromVal) * p);
        }

        // Marquee horizontal suave estilo firmware MPS3 para textos longos (não corta artistas/músicas)
        // Suporta tanto interpolação local (progress 0..1) quanto rolagem contínua global com loop contínuo infinito
        function drawMarqueeText(oled, y, str, progressOrSec, minX = 6, maxX = 121, c = 1, scale = 1, spacing = 1, speedPxPerSec = 22, isSeconds = false) {
            const clean = normAsciiOled(str || '');
            const charW = 5 * scale + spacing;
            const tw = clean.length * charW - spacing;
            const availW = maxX - minX + 1;

            if (tw <= availW) {
                // Cabe perfeitamente: centraliza
                const x = minX + Math.floor((availW - tw) / 2);
                oled.text(x, y, clean, c, scale, spacing);
                return;
            }

            let startX = minX;
            // Se progressOrSec > 1.0 (ou se chamamos com tempo em segundos), tratamos como contínuo infinito
            if (isSeconds || (typeof progressOrSec === 'number' && progressOrSec > 1.0001)) {
                const sep = '   *   ';
                const sepW = sep.length * charW;
                const loopW = tw + sepW;
                const pauseInitialS = 0.5; // Pausa elegante de leitura inicial
                const tEff = Math.max(0, (progressOrSec || 0) - pauseInitialS);
                const shiftPx = Math.floor(tEff * speedPxPerSec) % loopW;
                startX = minX - shiftPx;

                // Desenha instâncias suficientes para cobrir availW
                const fullCycleText = clean + sep;
                for (let cycle = 0; cycle < 3; cycle++) {
                    const cycleStartX = startX + cycle * loopW;
                    let cx = cycleStartX;
                    for (let i = 0; i < fullCycleText.length; i++) {
                        const ch = fullCycleText[i];
                        if (cx + 5 * scale >= minX && cx <= maxX) {
                            const cols = OLED_FONT_5X7[ch] || OLED_FONT_5X7['?'];
                            for (let colIdx = 0; colIdx < 5; colIdx++) {
                                const px = cx + colIdx * scale;
                                const bits = cols[colIdx];
                                for (let rowIdx = 0; rowIdx < 7; rowIdx++) {
                                    if ((bits >>> rowIdx) & 1) {
                                        for (let sy = 0; sy < scale; sy++) {
                                            for (let sx = 0; sx < scale; sx++) {
                                                const fx = px + sx;
                                                const fy = y + rowIdx * scale + sy;
                                                if (fx >= minX && fx <= maxX && fy >= 0 && fy < oled.h) {
                                                    oled.buf[fy][fx] = c ? 1 : 0;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        cx += charW;
                    }
                }
                return;
            }

            // Modo interpolado (0..1)
            const progress = Math.max(0, Math.min(1.0, progressOrSec || 0));
            const overflow = tw - availW + 8;
            let scrollP = 0;
            if (progress <= 0.12) {
                scrollP = 0;
            } else if (progress >= 0.88) {
                scrollP = 1.0;
            } else {
                scrollP = (progress - 0.12) / 0.76;
            }

            const scrollPx = Math.round(scrollP * overflow);
            startX = minX - scrollPx;

            let cx = startX;
            for (let i = 0; i < clean.length; i++) {
                const ch = clean[i];
                if (cx + 5 * scale >= minX && cx <= maxX) {
                    const cols = OLED_FONT_5X7[ch] || OLED_FONT_5X7['?'];
                    for (let colIdx = 0; colIdx < 5; colIdx++) {
                        const px = cx + colIdx * scale;
                        const bits = cols[colIdx];
                        for (let rowIdx = 0; rowIdx < 7; rowIdx++) {
                            if ((bits >>> rowIdx) & 1) {
                                for (let sy = 0; sy < scale; sy++) {
                                    for (let sx = 0; sx < scale; sx++) {
                                        const fx = px + sx;
                                        const fy = y + rowIdx * scale + sy;
                                        if (fx >= minX && fx <= maxX && fy >= 0 && fy < oled.h) {
                                            oled.buf[fy][fx] = c ? 1 : 0;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                cx += charW;
            }
        }

        // --- 3. BLITTER ULTRA-RÁPIDO COM OFFSCREEN ImageData (1-BIT -> GPU) ---
        let _wrpOffCanvas = null;
        let _wrpOffCtx = null;
        let _wrpOffImgData = null;

        function wrpHexToAbgr(hex) {
            let h = String(hex || '#ffffff').replace('#', '').trim();
            if (h.length === 3) h = h.split('').map(c => c + c).join('');
            const num = parseInt(h, 16);
            const r = (num >> 16) & 0xff;
            const g = (num >> 8) & 0xff;
            const b = num & 0xff;
            // Little-endian Uint32Array: 0xAABBGGRR
            return ((0xff << 24) | (b << 16) | (g << 8) | r) >>> 0;
        }

        function blitFast(ctx, oled, ox, oy, pitch, pal) {
            const w = oled.w;
            const h = oled.h;
            const totalW = w * pitch;
            const totalH = h * pitch;

            if (typeof document !== 'undefined' && document.createElement) {
                if (!_wrpOffCanvas || _wrpOffCanvas.width !== w || _wrpOffCanvas.height !== h) {
                    _wrpOffCanvas = document.createElement('canvas');
                    _wrpOffCanvas.width = w;
                    _wrpOffCanvas.height = h;
                    _wrpOffCtx = _wrpOffCanvas.getContext('2d', { willReadFrequently: false });
                    _wrpOffImgData = _wrpOffCtx.createImageData(w, h);
                }

                const offCol  = wrpHexToAbgr(pal.oledOff || '#081828');
                const dualY   = pal.dualZone ? 14 : -1;
                const topCol  = wrpHexToAbgr(pal.oledTop || pal.accent);
                const mainCol = wrpHexToAbgr(pal.oledOn  || pal.accent);

                const u32 = new Uint32Array(_wrpOffImgData.data.buffer);
                const buf = oled.buf;

                for (let py = 0; py < h; py++) {
                    const row = buf[py];
                    const onCol = (py < dualY) ? topCol : mainCol;
                    const rOffset = py * w;
                    for (let px = 0; px < w; px++) {
                        u32[rOffset + px] = row[px] ? onCol : offCol;
                    }
                }

                _wrpOffCtx.putImageData(_wrpOffImgData, 0, 0);

                ctx.fillStyle = '#020408';
                ctx.fillRect(ox - 4, oy - 4, totalW + 8, totalH + 8);

                ctx.imageSmoothingEnabled = false;
                if (ctx.webkitImageSmoothingEnabled !== undefined) ctx.webkitImageSmoothingEnabled = false;
                if (ctx.mozImageSmoothingEnabled !== undefined) ctx.mozImageSmoothingEnabled = false;

                ctx.drawImage(_wrpOffCanvas, ox, oy, totalW, totalH);
            } else {
                // Fallback para Node VM / headless sem DOM completo
                blitOledToCanvas(ctx, oled, ox, oy, pitch, 0, pal);
            }
        }

        // --- 4. MOTOR DE STORYBOARDS E LINHA DO TEMPO DINÂMICA ---
        function fitStoryboard(scenesBlueprint, durationS, fps = 30) {
            const activeScenes = [];
            // Copia profunda inicial dos blueprints
            for (const sc of scenesBlueprint) {
                activeScenes.push(Object.assign({}, sc));
            }

            // Poda por prioridade enquanto a soma dos tempos mínimos ultrapassar durationS
            while (true) {
                const totalMin = activeScenes.reduce((acc, s) => acc + s.minSec, 0);
                if (totalMin <= durationS || activeScenes.length <= 2) break;

                // Encontra a cena de menor prioridade (maior número de prioridade > 1)
                let dropIdx = -1;
                let maxPrio = 1;
                for (let i = 0; i < activeScenes.length; i++) {
                    if (activeScenes[i].prio > maxPrio) {
                        maxPrio = activeScenes[i].prio;
                        dropIdx = i;
                    }
                }
                if (dropIdx >= 0) {
                    activeScenes.splice(dropIdx, 1);
                } else {
                    break;
                }
            }

            // Proporção de compressão/expansão entre minSec e nomSec
            const sumMin = activeScenes.reduce((acc, s) => acc + s.minSec, 0);
            const sumNom = activeScenes.reduce((acc, s) => acc + s.nomSec, 0);
            const diffNom = Math.max(0.001, sumNom - sumMin);

            let allocated = 0;
            for (let i = 0; i < activeScenes.length; i++) {
                const s = activeScenes[i];
                if (durationS <= sumMin) {
                    s.allocatedSec = s.minSec * (durationS / sumMin);
                } else {
                    const ratio = Math.min(1.0, (durationS - sumMin) / diffNom);
                    s.allocatedSec = s.minSec + (s.nomSec - s.minSec) * ratio;
                }
                allocated += s.allocatedSec;
            }

            // Normalização estrita para somar exatamente durationS
            const scale = durationS / allocated;
            for (const s of activeScenes) {
                s.allocatedSec *= scale;
            }

            // Distribuição de quadros discretos
            const totalFrames = Math.round(durationS * fps);
            let currentF = 0;
            for (let i = 0; i < activeScenes.length; i++) {
                const s = activeScenes[i];
                s.startFrame = currentF;
                if (i === activeScenes.length - 1) {
                    s.endFrame = totalFrames - 1;
                } else {
                    const spanF = Math.max(1, Math.round(s.allocatedSec * fps));
                    s.endFrame = Math.min(totalFrames - 2, currentF + spanF - 1);
                }
                s.frameCount = Math.max(1, s.endFrame - s.startFrame + 1);
                currentF = s.endFrame + 1;
            }

            return {
                durationS,
                totalFrames,
                fps,
                scenes: activeScenes
            };
        }

        // --- 5. ROTEIRO FESTIVAL 1-BIT (CENAS DINÂMICAS) ---
        function getFestivalHeaderLabels(period) {
            const curY = new Date().getFullYear();
            let festHeader = `MPS3 FEST ${curY}`;
            let stageHeader = 'LINEUP DO MEU ANO';
            if (period === '30d') {
                festHeader = 'MPS3 FEST 30D';
                stageHeader = 'LINEUP DESTE MES';
            } else if (period === '7d') {
                festHeader = 'MPS3 FEST 7D';
                stageHeader = 'LINEUP DA SEMANA';
            } else if (period === 'all') {
                festHeader = 'MPS3 SOUNDFEST';
                stageHeader = 'LINEUP HISTORICO';
            }
            return { festHeader, stageHeader };
        }

        // Renderiza o pôster em qualquer altura yMax (animação cascata de cima para baixo estrita)
        // Modular: omite seções desativadas em modules e reorganiza coordenadas sem deixar lacunas vazias
        function renderFestivalLineupCascading(oled, st, gridW, gridH, period, globalS, yMax, modules) {
            const limY = Math.min(gridH - 1, Math.floor(yMax));
            if (limY < 2) return;

            const elapsed = typeof globalS === 'number' ? globalS : 0;
            const { festHeader, stageHeader } = getFestivalHeaderLabels(period);
            const mods = modules || WRAPPED_VIDEO_STATE.modules || {};

            // Moldura externa
            oled.rect(0, 0, gridW, limY + 1, false, 1);
            if (limY >= 3) {
                oled.line(2, 2, gridW - 3, 2, 1);
                oled.line(2, 2, 2, limY, 1);
                oled.line(gridW - 3, 2, gridW - 3, limY, 1);
                oled.line(2, limY, gridW - 3, limY, 1);
            }

            // 1. Cabeçalho (y = 5..14) - Sempre presente
            if (limY >= 14) {
                oled.drawSdIcon(6, 5, 1);
                oled.text(15, 5, festHeader, 1, 1, 1, 14);
                oled.drawSpeakerIcon(102, 5, 1);
                oled.drawBatteryIcon(112, 5, 1);
                oled.line(2, 14, gridW - 3, 14, 1);
            }

            let curY = 14;

            // 2. Banner Palco Principal (stage_time)
            if (mods.stage_time !== false) {
                const bStart = curY + 3;
                const bEnd = curY + 22; // 14 + 22 = 36
                if (limY >= bEnd - 1) {
                    oled.rrect(5, bStart, gridW - 10, 18, true, 1);
                    oled.textCenter(bStart + 2, stageHeader, 0, 1, 1, 19);
                    oled.textCenter(bStart + 10, `${fmtHoursMinutes(st.totalSec)} DE PALCO`, 0, 1, 1, 19);
                }
                curY = bEnd;
            }

            // 3. Headliner (headliner)
            const arts = st.topArtists.map(a => normAsciiOled(a.name));
            const headliner = arts[0] || 'SEU ARTISTA #1';
            const headShare = st.topArtists[0] ? `${st.topArtists[0].sharePct}% DO MEU SOM` : 'HEADLINER';

            if (mods.headliner !== false) {
                const bStart = curY + 3;
                const bEnd = curY + 30; // 36 + 30 = 66
                if (limY >= bStart + 7) {
                    oled.textCenter(bStart, `* ${headShare} *`, 1, 1, 1, 19);
                }
                if (limY >= bEnd) {
                    if (headliner.length <= 11) {
                        oled.textCenter(bStart + 10, headliner, 1, 2, 1, 11);
                    } else {
                        drawMarqueeText(oled, bStart + 10, headliner, elapsed, 6, gridW - 7, 1, 2, 1);
                    }
                    oled.line(6, bEnd, gridW - 7, bEnd, 1);
                }
                curY = bEnd;
            }

            // 4. Co-Headliners (coheadliners)
            if (mods.coheadliners !== false) {
                const bStart = curY + 3;
                const bEnd = curY + 40; // 66 + 40 = 106
                if (limY >= bStart + 9) {
                    oled.textCenter(bStart + 1, '- CO-HEADLINERS -', 1, 1, 1, 18);
                }
                if (limY >= bStart + 22) {
                    const a2 = arts[1] || 'CONVIDADO #2';
                    oled.rrect(6, bStart + 10, gridW - 12, 11, true, 1);
                    drawMarqueeText(oled, bStart + 12, a2, elapsed, 9, gridW - 10, 0, 1, 1);
                }
                if (limY >= bEnd - 2) {
                    const a3 = arts[2] || 'CONVIDADO #3';
                    oled.rrect(6, bStart + 23, gridW - 12, 11, false, 1);
                    drawMarqueeText(oled, bStart + 25, a3, elapsed, 9, gridW - 10, 1, 1, 1);
                }
                curY = bEnd;
            }

            // 5. Special Guests (special_guests)
            if (mods.special_guests !== false) {
                const bStart = curY + 1;
                const bEnd = curY + 41; // 106 + 41 = 147
                if (gridH >= 155 && limY >= bStart + 12) {
                    oled.line(6, bStart, gridW - 7, bStart, 1);
                    oled.textCenter(bStart + 3, 'SPECIAL GUESTS', 1, 1, 1, 18);
                }
                if (gridH >= 155 && limY >= bStart + 22) {
                    const line45 = [arts[3], arts[4]].filter(Boolean).join(' * ') || 'ARTISTA #4 * #5';
                    drawMarqueeText(oled, bStart + 13, line45, elapsed, 8, gridW - 8, 1, 1, 1);
                }
                if (gridH >= 155 && limY >= bStart + 32) {
                    const line67 = [arts[5], arts[6]].filter(Boolean).join(' * ') || 'ARTISTA #6 * #7';
                    drawMarqueeText(oled, bStart + 23, line67, elapsed, 8, gridW - 8, 1, 1, 1);
                }
                if (gridH >= 210 && limY >= bEnd) {
                    const line89 = [arts[7], arts[8], arts[9]].filter(Boolean).join(' * ');
                    if (line89) drawMarqueeText(oled, bStart + 33, line89, elapsed, 8, gridW - 8, 1, 1, 1);
                }
                curY = bEnd;
            }

            // 6. Álbum no Palco (album)
            if (mods.album !== false) {
                const bStart = curY + 1;
                const bEnd = curY + 27; // 147 + 27 = 174
                if (gridH >= 210 && limY >= bStart + 12) {
                    oled.line(6, bStart, gridW - 7, bStart, 1);
                    oled.textCenter(bStart + 4, 'ALBUM NO PALCO:', 1, 1, 1, 18);
                }
                if (gridH >= 210 && limY >= bEnd - 2) {
                    const rawAlb = st.topAlbums[0] ? normAsciiOled(st.topAlbums[0].name) : 'DISCOTECA PROPRIA';
                    oled.rrect(6, bStart + 13, gridW - 12, 11, true, 1);
                    drawMarqueeText(oled, bStart + 15, rawAlb, elapsed, 9, gridW - 10, 0, 1, 1);
                }
                curY = bEnd;
            }

            // 7. Rodapé do Festival: Canhoto VIP, Maratona/Pico, Barcode e Persona (clock24h || persona)
            if (mods.clock24h !== false || mods.persona !== false) {
                const bStart = curY + 1;
                const bEnd = curY + 41; // 174 + 41 = 215
                if (gridH >= 210 && limY >= bStart + 11) {
                    for (let dx = 4; dx < gridW - 4; dx += 4) {
                        oled.line(dx, bStart, dx + 1, bStart, 1);
                    }
                    if (mods.clock24h !== false) {
                        let ticketBadge = st.peakCalendarDay.sec > 0 ? `PICO ${st.peakCalendarDay.shortLabel}` : `STREAK ${st.longestStreak}`;
                        if (st.musicalAge > 0 && ticketBadge.length + 8 <= 19) ticketBadge += ` * A${st.musicalAge}`;
                        oled.textCenter(bStart + 4, ticketBadge, 1, 1, 1, 19);
                    }
                }
                if (gridH >= 210 && limY >= bStart + 25) {
                    for (let bx = 10; bx < gridW - 10; bx += 2) {
                        const barW = ((bx * 7 + (st.totalScrobbles || 5)) % 3 === 0) ? 2 : 1;
                        oled.rect(bx, bStart + 13, barW, 10, true, 1);
                        if (barW === 2) bx += 1;
                    }
                }
                if (mods.persona !== false && gridH >= 210 && limY >= bEnd - 3) {
                    const fullTitle = normAsciiOled(st.persona.title);
                    drawMarqueeText(oled, bStart + 26, fullTitle, elapsed, 8, gridW - 8, 1, 1, 1);
                } else if (gridH < 210 && limY >= gridH - 1) {
                    const footLbl = st.musicalAge > 0 ? `VIP * ALMA ${st.musicalAge}` : `VIP * FOCO ${st.zeroSkipPct}%`;
                    oled.rrect(6, gridH - 16, gridW - 12, 11, true, 1);
                    oled.textCenter(gridH - 14, footLbl, 0, 1, 1, 18);
                }
            }
        }

        // --- 5. ROTEIRO FESTIVAL 1-BIT (CENAS DINÂMICAS) ---
        function getFestivalHeaderLabels(period) {
            const curY = new Date().getFullYear();
            let festHeader = `MPS3 FEST ${curY}`;
            let stageHeader = 'LINEUP DO MEU ANO';
            if (period === '30d') {
                festHeader = 'MPS3 FEST 30D';
                stageHeader = 'LINEUP DESTE MES';
            } else if (period === '7d') {
                festHeader = 'MPS3 FEST 7D';
                stageHeader = 'LINEUP DA SEMANA';
            } else if (period === 'all') {
                festHeader = 'MPS3 SOUNDFEST';
                stageHeader = 'LINEUP HISTORICO';
            }
            return { festHeader, stageHeader };
        }

        // Renderiza o card Meu Festival Stories 9:16 em animação fluida contínua
        // Harmonizado pixel-a-pixel com renderStoriesNativeOledV95, com marquees em textos longos,
        // barras horárias 24h emergindo de baixo para cima e constelação de estrelas procedural
        function renderFestivalAnimatedCard(oled, st, gridW, gridH, period, globalS, storyboard) {
            // Em dimensões menores que 210px (ex: feeds/quadrados secundários), usa fallback cascata
            if (gridH < 210) {
                renderFestivalLineupCascading(oled, st, gridW, gridH, period, globalS, gridH);
                return;
            }

            const p = period || (typeof WRAPPED_STATE !== 'undefined' ? WRAPPED_STATE.period : 'all') || 'all';
            let periodName = 'MENSAL';
            if (p === '7d') periodName = 'SEMANAL';
            else if (p === '30d') periodName = 'MENSAL';
            else if (p === '24h' || p === '1d') periodName = 'DIARIO';
            else if (p === 'year') {
                const curMonth = new Date().getMonth();
                periodName = (curMonth === 11) ? 'WRAPPED' : `${new Date().getFullYear()}`;
            } else if (p === 'all') {
                periodName = 'MENSAL';
            }

            const dur = (storyboard && storyboard.totalFrames)
                ? (storyboard.totalFrames / (storyboard.fps || 30))
                : 6.0;
            const elapsed = (typeof globalS === 'number') ? globalS : 0;

            // 1. Moldura do display SSD1306 (pixel perfect)
            oled.rect(0, 0, gridW, gridH, false, 1);
            oled.line(2, 2, gridW - 3, 2, 1);
            oled.line(2, 2, 2, gridH - 3, 1);
            oled.line(gridW - 3, 2, gridW - 3, gridH - 3, 1);
            oled.line(2, gridH - 3, gridW - 3, gridH - 3, 1);

            // 2. Primeira Linha: Ícone Play + Badge MPS3 + Periodicidade + Bateria Real MPS3
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
            const byCards = gridH - 20 - 3;
            const bottomY = gridH - 3;
            const contentMaxY = byCards - 3;

            const arts = (st && st.topArtists) ? st.topArtists.map(a => normAsciiOled((a && a.name) || '')) : [];
            const headliner = (arts[0] && arts[0].trim()) || 'ARTISTA #1';
            const headShare = (st && st.topArtists && st.topArtists[0]) ? (st.topArtists[0].sharePct || 0) : 0;

            // A. Headliner com Bracket e Marquee se > 11 caracteres
            const tag = headShare > 0 ? `* HEADLINER (${headShare}%) *` : '* HEADLINER *';
            mps3DrawArtistBrackets(oled, curY, tag);

            const nameY = curY + 12;
            const cleanHead = normAsciiOled(headliner);
            if (cleanHead.length <= 8) {
                const tw = cleanHead.length * (5 * 2 + 2) - 2;
                const x = Math.max(4, Math.floor((gridW - tw) / 2));
                oled.text(x, nameY, cleanHead, 1, 2, 2);
            } else if (cleanHead.length <= 11) {
                const tw = cleanHead.length * 7 - 1;
                const boxW = Math.min(gridW - 12, tw + 10);
                const boxX = Math.floor((gridW - boxW) / 2);
                const textX = Math.floor((gridW - tw) / 2);

                oled.rrect(boxX, nameY, boxW, 12, false, 1);
                oled.text(textX, nameY + 2, cleanHead, 1, 1, 2);
                oled.text(textX + 1, nameY + 2, cleanHead, 1, 1, 2);
            } else {
                drawMarqueeText(oled, nameY, cleanHead, elapsed, 6, gridW - 7, 1, 2, 1, 24, true);
            }
            curY = 44;

            // B. Barra de Palco Integrada (preenchimento animado emergindo com o tempo)
            const fillRatio = wrpEaseOutCubic(Math.max(0, Math.min(1.0, (elapsed - 0.1) / Math.max(0.5, 0.35 * dur))));
            const curSec = Math.round(((st && st.totalSec) || 0) * fillRatio);
            mps3DrawProgressBarWithTime(
                oled, 6, curY, gridW - 12, 10,
                curSec, ((st && st.totalSec) || 1),
                'PALCO',
                fmtHoursMinutes((st && st.totalSec) || 0)
            );
            curY += 10;

            // C. Destaque Especial para CO-HEADLINERS (5 linhas com marquees)
            curY += 3;
            const tagCo = '- CO-HEADLINERS -';
            const tagCoW = tagCo.length * 6 - 1;
            const tagCoX = Math.floor((gridW - tagCoW) / 2);

            oled.line(6, curY + 3, tagCoX - 4, curY + 3, 1);
            oled.text(tagCoX, curY, tagCo, 1, 1, 1);
            oled.line(tagCoX + tagCoW + 3, curY + 3, gridW - 7, curY + 3, 1);
            curY += 7 + 3;

            const listRows = 5;
            const rowH = 10;
            const rowX = 6;
            const rowW = gridW - 12;

            for (let r = 0; r < listRows; r++) {
                const rowY = curY + r * (rowH + 1);
                const isSelected = (r === 0); // 2º lugar com fundo invertido/branco
                const c = isSelected ? 0 : 1;
                const textY = rowY + Math.floor((rowH - 7) / 2);

                if (isSelected) {
                    oled.rect(rowX, rowY, rowW, rowH, true, 1);
                }

                const num = r + 2;
                const numStr = `${num}.`;
                oled.text(rowX + 3, textY, numStr, c, 1, 1);
                const minX = rowX + 16;

                const artItem = (st && st.topArtists && st.topArtists[r + 1]);
                const rawName = (artItem && artItem.name) ? artItem.name : (arts[r + 1] || `ARTISTA #${num}`);
                const cleanName = normAsciiOled(rawName);
                const pctStr = (artItem && typeof artItem.sharePct === 'number' && artItem.sharePct > 0)
                    ? `${artItem.sharePct}%`
                    : '';

                let maxX = rowX + rowW - 4;
                if (pctStr) {
                    const mw = pctStr.length * 6 - 1;
                    const metaX = rowX + rowW - 4 - mw;
                    oled.text(metaX, textY, pctStr, c, 1, 1);
                    maxX = metaX - 3;
                }

                const availW = maxX - minX + 1;
                const nameW = cleanName.length * 6 - 1;

                if (nameW <= availW) {
                    oled.text(minX, textY, cleanName, c, 1, 1);
                } else {
                    drawMarqueeText(oled, textY, cleanName, elapsed, minX, maxX, c, 1, 1, 20, true);
                }
            }
            curY += listRows * (rowH + 1);

            // D. Álbum em badge direto do firmware (título em marquee dentro do badge se longo)
            const topAlb = (st && st.topAlbums && st.topAlbums[0]);
            const rawAlb = (topAlb && topAlb.name) ? normAsciiOled(topAlb.name) : 'DISCOTECA MPS3';
            oled.rrect(6, curY, gridW - 12, 11, false, 1);
            oled.text(9, curY + 2, 'ALBUM:', 1, 1, 1);

            const albMinX = 48;
            const albMaxX = gridW - 10;
            const albAvailW = albMaxX - albMinX + 1;
            const albW = rawAlb.length * 6 - 1;

            if (albW <= albAvailW) {
                oled.text(albMinX, curY + 2, rawAlb, 1, 1, 1);
            } else {
                drawMarqueeText(oled, curY + 2, rawAlb, elapsed, albMinX, albMaxX, 1, 1, 1, 20, true);
            }
            curY += 13;

            oled.line(6, curY, gridW - 7, curY, 1);
            curY += 3;

            // E. Ciclo 24h & Estrelas (barras emergem de baixo para cima, estrelas aparecem posteriormente)
            const rtaBase = contentMaxY - 1;
            const peakH = (st && typeof st.peakHour === 'number') ? st.peakHour : 12;
            oled.textCenter(curY, `DIA 24H : PICO ${String(peakH).padStart(2, '0')}H`, 1, 1, 1);
            curY += 8;

            const rtaBarMaxH = Math.min(28, rtaBase - curY - 10);
            const sunY = rtaBase - rtaBarMaxH - 9;

            const hours = (st && st.hoursSec) ? st.hoursSec : new Array(24).fill(0);
            const maxVal = Math.max(1, ...hours);
            const startX = 17;

            // Lua da Esquerda (00h) - Crateras na madrugada
            const leftMoonX = 6;
            const moonY = rtaBase - 13;
            mps3DrawUserMoonCrater(oled, leftMoonX, moonY, false);

            // 24 barras horárias emergindo de baixo para cima (da base rtaBase)
            const tStartBars = 0.15 * dur;
            const tSpanBars = Math.max(0.6, 0.30 * dur);
            const barsGlobalT = wrpEaseOutCubic(Math.max(0, Math.min(1.0, (elapsed - tStartBars) / tSpanBars)));

            for (let h = 0; h < 24; h++) {
                const val = hours[h] || 0;
                const ratio = maxVal > 0 ? (val / maxVal) : 0;
                const fullBarH = ratio > 0 ? Math.max(4, Math.round(ratio * rtaBarMaxH)) : 2;

                const hStagger = (h / 24) * 0.12;
                const barLocalT = wrpEaseOutCubic(Math.max(0, Math.min(1.0, (barsGlobalT - hStagger) / (1.0 - 0.12))));
                const curBarH = Math.max(1, Math.round(fullBarH * barLocalT));
                const bx = startX + h * 4;

                oled.rect(bx, rtaBase - curBarH, 3, curBarH, true, 1);
                if (curBarH >= 8 && barLocalT >= 0.85) {
                    oled.line(bx, rtaBase - curBarH - 2, bx + 2, rtaBase - curBarH - 2, 1);
                }
            }

            const sunX = startX + 12 * 4 - 2;
            mps3DrawSunIcon(oled, sunX, sunY);

            // Lua da Direita (23h) - Crescente no final da noite
            const rightMoonW = 5;
            const rightMoonX = (gridW - 7) - rightMoonW + 1;
            mps3DrawUserMoonCrescent(oled, rightMoonX, moonY, true);

            // Estrelas procedurais surgindo posteriormente
            const tStartStars = tStartBars + tSpanBars * 0.9;
            const tSpanStars = Math.max(0.6, 0.25 * dur);
            const starsProgress = wrpEaseInOutQuad(Math.max(0, Math.min(1.0, (elapsed - tStartStars) / tSpanStars)));

            if (starsProgress > 0) {
                mps3DrawCelestialStars(oled, leftMoonX, rightMoonX, sunX, sunY, curY, rtaBase, st, starsProgress);
            }

            oled.line(6, rtaBase + 1, gridW - 7, rtaBase + 1, 1);

            // F. Rodapé: 3 Cards nativos MPS3 (com streak em número puro)
            const totScrobbles = (st && st.totalScrobbles) || 152;
            const playsVal = (totScrobbles >= 1000) ? `${(totScrobbles / 1000).toFixed(1)}k` : `${totScrobbles}`;
            const artsVal = `${(st && st.uniqueArtists) || 9}`;
            const streakVal = `${(st && st.longestStreak) || 27}`;

            const bW = 36;
            const gap = 4;
            mps3DrawMetricCard(oled, 6, byCards, bW, bottomY, playsVal, 'PLAYS');
            mps3DrawMetricCard(oled, 6 + (bW + gap), byCards, bW, bottomY, artsVal, 'ART');
            mps3DrawMetricCard(oled, 6 + (bW + gap) * 2, byCards, bW, bottomY, streakVal, 'DIAS');
        }

        const FESTIVAL_SCENES_BLUEPRINT = [
            {
                id: 'boot',
                mod: null,
                prio: 2,
                minSec: 0.5,
                nomSec: 1.0,
                render: function(oled, t, st, gridW, gridH, period, globalS, storyboard) {
                    const dur = (storyboard && storyboard.totalFrames) ? (storyboard.totalFrames / (storyboard.fps || 30)) : 6.0;
                    const elapsed = (typeof globalS === 'number') ? globalS : ((typeof t === 'number') ? t * dur : 0);
                    renderFestivalAnimatedCard(oled, st, gridW, gridH, period, elapsed, storyboard);
                }
            },
            {
                id: 'stage_time',
                mod: 'stage_time',
                prio: 1,
                minSec: 0.9,
                nomSec: 2.0,
                render: function(oled, t, st, gridW, gridH, period, globalS, storyboard) {
                    const dur = (storyboard && storyboard.totalFrames) ? (storyboard.totalFrames / (storyboard.fps || 30)) : 6.0;
                    const elapsed = (typeof globalS === 'number') ? globalS : ((typeof t === 'number') ? t * dur : 0);
                    renderFestivalAnimatedCard(oled, st, gridW, gridH, period, elapsed, storyboard);
                }
            },
            {
                id: 'headliner',
                mod: 'headliner',
                prio: 1,
                minSec: 1.2,
                nomSec: 2.5,
                render: function(oled, t, st, gridW, gridH, period, globalS, storyboard) {
                    const dur = (storyboard && storyboard.totalFrames) ? (storyboard.totalFrames / (storyboard.fps || 30)) : 6.0;
                    const elapsed = (typeof globalS === 'number') ? globalS : ((typeof t === 'number') ? t * dur : 0);
                    renderFestivalAnimatedCard(oled, st, gridW, gridH, period, elapsed, storyboard);
                }
            },
            {
                id: 'coheadliners',
                mod: 'coheadliners',
                prio: 2,
                minSec: 1.0,
                nomSec: 2.0,
                render: function(oled, t, st, gridW, gridH, period, globalS, storyboard) {
                    const dur = (storyboard && storyboard.totalFrames) ? (storyboard.totalFrames / (storyboard.fps || 30)) : 6.0;
                    const elapsed = (typeof globalS === 'number') ? globalS : ((typeof t === 'number') ? t * dur : 0);
                    renderFestivalAnimatedCard(oled, st, gridW, gridH, period, elapsed, storyboard);
                }
            },
            {
                id: 'special_guests',
                mod: 'special_guests',
                prio: 1,
                minSec: 1.2,
                nomSec: 2.5,
                render: function(oled, t, st, gridW, gridH, period, globalS, storyboard) {
                    const dur = (storyboard && storyboard.totalFrames) ? (storyboard.totalFrames / (storyboard.fps || 30)) : 6.0;
                    const elapsed = (typeof globalS === 'number') ? globalS : ((typeof t === 'number') ? t * dur : 0);
                    renderFestivalAnimatedCard(oled, st, gridW, gridH, period, elapsed, storyboard);
                }
            },
            {
                id: 'album_persona',
                mod: ['album', 'persona', 'clock24h'],
                prio: 2,
                minSec: 1.2,
                nomSec: 2.2,
                render: function(oled, t, st, gridW, gridH, period, globalS, storyboard) {
                    const dur = (storyboard && storyboard.totalFrames) ? (storyboard.totalFrames / (storyboard.fps || 30)) : 6.0;
                    const elapsed = (typeof globalS === 'number') ? globalS : ((typeof t === 'number') ? t * dur : 0);
                    renderFestivalAnimatedCard(oled, st, gridW, gridH, period, elapsed, storyboard);
                }
            },
            {
                id: 'final_poster',
                mod: null,
                prio: 1,
                minSec: 0.8,
                nomSec: 2.0,
                render: function(oled, t, st, gridW, gridH, period, globalS, storyboard) {
                    const dur = (storyboard && storyboard.totalFrames) ? (storyboard.totalFrames / (storyboard.fps || 30)) : 6.0;
                    const elapsed = (typeof globalS === 'number') ? globalS : ((typeof t === 'number') ? t * dur : 0);
                    renderFestivalAnimatedCard(oled, st, gridW, gridH, period, elapsed, storyboard);
                }
            }
        ];

        // --- 5B. ROTEIRO STORYBOARD WRAPPED (JORNADA MULTI-TELAS 1-BIT EM TELA CHEIA) ---
        const STORYBOARD_SCENES_BLUEPRINT = [
            {
                id: 'sb_boot',
                mod: null,
                prio: 2,
                minSec: 0.6,
                nomSec: 1.2,
                render: function(oled, t, st, gridW, gridH, period) {
                    oled.rect(0, 0, gridW, gridH, false, 1);
                    oled.rect(2, 2, gridW - 4, gridH - 4, false, 1);

                    oled.drawSdIcon(6, 6, 1);
                    oled.drawSpeakerIcon(104, 6, 1);
                    oled.drawBatteryIcon(114, 6, 1);
                    oled.line(2, 16, gridW - 3, 16, 1);

                    const midY = Math.floor(gridH / 2);
                    oled.textCenter(midY - 26, 'MPS3 WRAPPED', 1, 2, 1, 11);
                    oled.textCenter(midY - 6, 'RETROSPECTIVA DE AUDIO', 1, 1, 1, 22);

                    const barW = Math.round(wrpEaseOutCubic(t) * (gridW - 28));
                    oled.rect(14, midY + 8, gridW - 28, 10, false, 1);
                    oled.rect(16, midY + 10, barW, 6, true, 1);

                    const subTxt = wrpTypewriter('CARREGANDO STATS...', t);
                    oled.textCenter(midY + 24, subTxt, 1, 1, 1, 20);
                }
            },
            {
                id: 'sb_stage',
                mod: 'stage_time',
                prio: 1,
                minSec: 1.2,
                nomSec: 2.5,
                render: function(oled, t, st, gridW, gridH, period, globalS) {
                    oled.rect(0, 0, gridW, gridH, false, 1);
                    oled.line(2, 16, gridW - 3, 16, 1);
                    oled.textCenter(6, 'TEMPO DE PALCO', 1, 1, 1, 18);

                    const midY = Math.floor(gridH / 2);
                    const curSec = wrpRollingCounter(0, st.totalSec, wrpEaseOutCubic(t));
                    oled.rrect(6, midY - 28, gridW - 12, 34, true, 1);
                    oled.textCenter(midY - 20, fmtHoursMinutes(curSec), 0, 2, 1, 12);

                    if (t > 0.3) {
                        const trkCnt = wrpRollingCounter(0, st.totalTracks || st.uniqueTracks || 0, (t - 0.3) / 0.7);
                        oled.textCenter(midY + 14, `${trkCnt} FAIXAS OUVIDAS`, 1, 1, 1, 20);
                    }
                    if (t > 0.5) {
                        const streak = st.longestStreak || 1;
                        oled.rrect(10, midY + 28, gridW - 20, 14, false, 1);
                        oled.textCenter(midY + 31, `SEQUENCIA: ${streak} DIAS`, 1, 1, 1, 20);
                    }
                }
            },
            {
                id: 'sb_headliner',
                mod: 'headliner',
                prio: 1,
                minSec: 1.5,
                nomSec: 3.0,
                render: function(oled, t, st, gridW, gridH, period, globalS) {
                    oled.rect(0, 0, gridW, gridH, false, 1);
                    oled.line(2, 16, gridW - 3, 16, 1);
                    oled.textCenter(6, 'ARTISTA #1 NO TOPO', 1, 1, 1, 19);

                    const midY = Math.floor(gridH / 2);
                    const arts = st.topArtists.map(a => normAsciiOled(a.name));
                    const headliner = arts[0] || 'SEU ARTISTA #1';
                    const headShare = st.topArtists[0] ? st.topArtists[0].sharePct : 0;

                    oled.textCenter(midY - 32, '* DOMINIO ABSOLUTO *', 1, 1, 1, 20);

                    if (headliner.length <= 11) {
                        oled.textCenter(midY - 18, headliner, 1, 2, 1, 11);
                    } else {
                        drawMarqueeText(oled, midY - 18, headliner, globalS, 6, gridW - 7, 1, 2, 1);
                    }

                    const curPct = wrpRollingCounter(0, headShare, wrpEaseOutCubic(t));
                    oled.rrect(8, midY + 8, gridW - 16, 14, true, 1);
                    oled.textCenter(midY + 11, `${curPct}% DO MEU SOM`, 0, 1, 1, 18);

                    if (t > 0.4 && st.topArtists[0]) {
                        const plays = st.topArtists[0].scrobbles || st.topArtists[0].plays || 0;
                        oled.textCenter(midY + 28, `${plays} REPRODUCOES`, 1, 1, 1, 19);
                    }
                }
            },
            {
                id: 'sb_lineup',
                mod: ['coheadliners', 'special_guests'],
                prio: 2,
                minSec: 1.2,
                nomSec: 2.5,
                render: function(oled, t, st, gridW, gridH, period, globalS) {
                    oled.rect(0, 0, gridW, gridH, false, 1);
                    oled.line(2, 16, gridW - 3, 16, 1);
                    oled.textCenter(6, 'TOP ARTISTAS & CRIADORES', 1, 1, 1, 22);

                    const arts = st.topArtists.slice(0, 5);
                    let y = 24;
                    for (let i = 0; i < arts.length; i++) {
                        const rowT = Math.max(0, Math.min(1.0, (t - i * 0.15) / 0.4));
                        if (rowT > 0) {
                            const a = arts[i];
                            oled.text(6, y, `${i + 1}.`, 1, 1, 1);
                            const name = normAsciiOled(a.name);
                            drawMarqueeText(oled, y, name, globalS, 20, gridW - 32, 1, 1, 1);
                            oled.text(gridW - 28, y, `${a.sharePct}%`, 1, 1, 1);
                            oled.line(6, y + 9, gridW - 7, y + 9, 1);
                        }
                        y += 14;
                    }
                }
            },
            {
                id: 'sb_album',
                mod: 'album',
                prio: 2,
                minSec: 1.0,
                nomSec: 2.2,
                render: function(oled, t, st, gridW, gridH, period, globalS) {
                    oled.rect(0, 0, gridW, gridH, false, 1);
                    oled.line(2, 16, gridW - 3, 16, 1);
                    oled.textCenter(6, 'ALBUM NO PALCO', 1, 1, 1, 18);

                    const midY = Math.floor(gridH / 2);
                    const topAlb = st.topAlbums[0];
                    const albName = topAlb ? normAsciiOled(topAlb.name) : 'DISCOTECA PROPRIA';
                    const albArt = topAlb ? normAsciiOled(topAlb.artist) : 'VARIOS ARTISTAS';

                    const cx = Math.floor(gridW / 2);
                    const cy = midY - 20;
                    oled.circle(cx, cy, 18, false, 1);
                    oled.circle(cx, cy, 14, false, 1);
                    oled.circle(cx, cy, 7, false, 1);
                    oled.circle(cx, cy, 2, true, 1);

                    oled.rrect(6, midY + 8, gridW - 12, 13, true, 1);
                    drawMarqueeText(oled, midY + 10, albName, globalS, 8, gridW - 9, 0, 1, 1);

                    if (t > 0.3) {
                        drawMarqueeText(oled, midY + 25, albArt, globalS, 8, gridW - 9, 1, 1, 1);
                    }
                    if (t > 0.5 && topAlb && topAlb.fullSessions > 0) {
                        oled.textCenter(midY + 37, `${topAlb.fullSessions}X DO INICIO AO FIM`, 1, 1, 1, 20);
                    }
                }
            },
            {
                id: 'sb_clock',
                mod: 'clock24h',
                prio: 2,
                minSec: 1.2,
                nomSec: 2.5,
                render: function(oled, t, st, gridW, gridH, period, globalS) {
                    oled.rect(0, 0, gridW, gridH, false, 1);
                    oled.line(2, 16, gridW - 3, 16, 1);
                    oled.textCenter(6, 'RITMO BIOLOGICO 24H', 1, 1, 1, 20);

                    const midY = Math.floor(gridH / 2);
                    const peakH = st.peakHour !== undefined ? st.peakHour : 20;
                    const hours = st.hoursSec || new Array(24).fill(0);
                    const maxVal = Math.max(1, ...hours);

                    const startX = Math.floor((gridW - 96) / 2);
                    const baseLine = midY + 14;
                    const maxBarH = 34;

                    const progress = wrpEaseOutCubic(t);
                    for (let h = 0; h < 24; h++) {
                        const ratio = hours[h] / maxVal;
                        const barH = Math.round(ratio * maxBarH * progress);
                        const bx = startX + h * 4;
                        if (barH > 0) {
                            oled.rect(bx, baseLine - barH, 3, barH, true, 1);
                        }
                        if (h === peakH && t > 0.5) {
                            oled.setPx(bx + 1, baseLine - barH - 3, 1);
                        }
                    }
                    oled.line(startX - 2, baseLine + 1, startX + 97, baseLine + 1, 1);
                    oled.text(startX, baseLine + 4, '00H', 1, 1, 1);
                    oled.text(startX + 40, baseLine + 4, '12H', 1, 1, 1);
                    oled.text(startX + 78, baseLine + 4, '23H', 1, 1, 1);

                    if (t > 0.4) {
                        oled.rrect(12, baseLine + 18, gridW - 24, 13, false, 1);
                        oled.textCenter(baseLine + 21, `PICO AS ${String(peakH).padStart(2, '0')}H`, 1, 1, 1, 18);
                    }
                }
            },
            {
                id: 'sb_persona',
                mod: 'persona',
                prio: 1,
                minSec: 1.5,
                nomSec: 3.0,
                render: function(oled, t, st, gridW, gridH, period, globalS) {
                    oled.rect(0, 0, gridW, gridH, false, 1);
                    oled.circle(0, 0, 8, true, 0);
                    oled.circle(gridW - 1, 0, 8, true, 0);
                    oled.circle(0, gridH - 1, 8, true, 0);
                    oled.circle(gridW - 1, gridH - 1, 8, true, 0);

                    oled.line(2, 16, gridW - 3, 16, 1);
                    oled.textCenter(6, 'MPS3 VIP PASS', 1, 1, 1, 18);

                    const midY = Math.floor(gridH / 2);
                    oled.textCenter(midY - 30, st.persona.badge || 'VIP IDENTIFIED', 1, 1, 1, 20);

                    const title = normAsciiOled(st.persona.title || 'EXPLORADOR SONORO');
                    oled.rrect(6, midY - 16, gridW - 12, 18, true, 1);
                    drawMarqueeText(oled, midY - 11, title, globalS, 8, gridW - 9, 0, 1, 1);

                    const ageTxt = st.musicalAge > 0 ? `ALMA MUSICAL: ANOS ${st.musicalAge}S` : `FOCO EM FAIXAS: ${st.zeroSkipPct}%`;
                    oled.textCenter(midY + 10, ageTxt, 1, 1, 1, 20);

                    const prog = Math.min(1.0, t * 1.5);
                    const barW = Math.round(prog * (gridW - 24));
                    for (let bx = 12; bx < 12 + barW; bx += 2) {
                        const thick = ((bx * 7 + (st.totalScrobbles || 3)) % 3 === 0);
                        oled.rect(bx, midY + 24, thick ? 2 : 1, 10, true, 1);
                        if (thick) bx++;
                    }
                }
            },
            {
                id: 'sb_final',
                mod: null,
                prio: 1,
                minSec: 0.8,
                nomSec: 2.0,
                render: function(oled, t, st, gridW, gridH, period, globalS) {
                    oled.rect(0, 0, gridW, gridH, false, 1);
                    oled.rect(2, 2, gridW - 4, gridH - 4, false, 1);

                    const { festHeader } = getFestivalHeaderLabels(period);
                    oled.line(2, 16, gridW - 3, 16, 1);
                    oled.textCenter(6, festHeader, 1, 1, 1, 18);

                    const midY = Math.floor(gridH / 2);
                    oled.textCenter(midY - 26, 'MPS3 SOUND FEST', 1, 2, 1, 12);
                    oled.textCenter(midY - 6, `${fmtHoursMinutes(st.totalSec)} OUVIDOS`, 1, 1, 1, 20);

                    const arts = st.topArtists.map(a => normAsciiOled(a.name));
                    const headliner = arts[0] || 'MEU ARTISTA #1';
                    oled.rrect(6, midY + 8, gridW - 12, 14, true, 1);
                    drawMarqueeText(oled, midY + 11, `HEADLINER: ${headliner}`, globalS, 8, gridW - 9, 0, 1, 1);

                    oled.textCenter(midY + 28, 'ATE O PROXIMO CICLO!', 1, 1, 1, 20);
                }
            }
        ];

        // --- 5C. ROTEIRO MÓDULO FÍSICO OLED 128x64 (SIMULAÇÃO HARDWARE MPS3) ---
        const OLED_BEZEL_SCENES_BLUEPRINT = [
            {
                id: 'bez_player',
                mod: 'headliner',
                prio: 1,
                minSec: 1.8,
                nomSec: 3.5,
                render: function(oled, t, st, gridW, gridH, period, globalS) {
                    // Barra superior de status (y = 0..9)
                    oled.drawSdIcon(2, 1, 1);
                    oled.text(12, 1, 'VOL 24', 1, 1, 1);
                    oled.drawSpeakerIcon(98, 1, 1);
                    oled.drawBatteryIcon(110, 1, 1);
                    oled.line(0, 9, gridW - 1, 9, 1);

                    // Carretéis da Fita Cassete retrô animada (y = 12..40)
                    oled.rrect(20, 12, 88, 28, false, 1);
                    oled.rect(34, 16, 60, 20, false, 1);

                    const cx1 = 44, cx2 = 84, cy = 26;
                    oled.circle(cx1, cy, 7, false, 1);
                    oled.circle(cx2, cy, 7, false, 1);
                    oled.circle(cx1, cy, 2, true, 1);
                    oled.circle(cx2, cy, 2, true, 1);

                    const rot = globalS * 6;
                    for (let i = 0; i < 3; i++) {
                        const ang = rot + i * (Math.PI * 2 / 3);
                        const cosA = Math.cos(ang);
                        const sinA = Math.sin(ang);
                        oled.line(cx1 + Math.round(2 * cosA), cy + Math.round(2 * sinA), cx1 + Math.round(6 * cosA), cy + Math.round(6 * sinA), 1);
                        oled.line(cx2 + Math.round(2 * cosA), cy + Math.round(2 * sinA), cx2 + Math.round(6 * cosA), cy + Math.round(6 * sinA), 1);
                    }

                    oled.line(cx1, cy + 7, cx2, cy + 7, 1);

                    const arts = st.topArtists.map(a => normAsciiOled(a.name));
                    const headliner = arts[0] || 'ARTISTA #1';
                    const topTrk = st.topTracks[0] ? normAsciiOled(st.topTracks[0].title) : 'FAIXA PRINCIPAL';
                    drawMarqueeText(oled, 43, `${headliner} - ${topTrk}`, globalS, 2, gridW - 3, 1, 1, 1);

                    const barW = Math.round(wrpEaseOutCubic(t) * (gridW - 8));
                    oled.rect(3, 54, gridW - 6, 6, false, 1);
                    oled.rect(4, 55, barW, 4, true, 1);
                }
            },
            {
                id: 'bez_stage',
                mod: 'stage_time',
                prio: 2,
                minSec: 1.0,
                nomSec: 2.0,
                render: function(oled, t, st, gridW, gridH, period, globalS) {
                    oled.drawSdIcon(2, 1, 1);
                    oled.text(12, 1, 'INFO > TEMPO', 1, 1, 1);
                    oled.drawBatteryIcon(110, 1, 1);
                    oled.line(0, 9, gridW - 1, 9, 1);

                    oled.textCenter(14, 'TEMPO TOTAL DE PALCO', 1, 1, 1, 20);
                    const curSec = wrpRollingCounter(0, st.totalSec, wrpEaseOutCubic(t));
                    oled.textCenter(28, fmtHoursMinutes(curSec), 1, 2, 1, 12);

                    const trkCnt = wrpRollingCounter(0, st.totalTracks || st.uniqueTracks || 0, t);
                    oled.textCenter(49, `${trkCnt} FAIXAS NO CARTAO`, 1, 1, 1, 20);
                    oled.line(4, 60, gridW - 5, 60, 1);
                }
            },
            {
                id: 'bez_lineup',
                mod: ['coheadliners', 'special_guests'],
                prio: 2,
                minSec: 1.2,
                nomSec: 2.5,
                render: function(oled, t, st, gridW, gridH, period, globalS) {
                    oled.text(4, 1, 'MENU > LINEUP', 1, 1, 1);
                    oled.drawBatteryIcon(110, 1, 1);
                    oled.line(0, 9, gridW - 1, 9, 1);

                    const arts = st.topArtists.slice(0, 4);
                    let y = 13;
                    for (let i = 0; i < arts.length; i++) {
                        const isSelected = (i === 0);
                        const name = normAsciiOled(arts[i].name);
                        if (isSelected) {
                            oled.rect(2, y - 1, gridW - 4, 11, true, 1);
                            drawMarqueeText(oled, y, `> 1. ${name}`, globalS, 4, gridW - 6, 0, 1, 1);
                        } else {
                            drawMarqueeText(oled, y, `  ${i + 1}. ${name}`, globalS, 4, gridW - 6, 1, 1, 1);
                        }
                        y += 12;
                    }
                }
            },
            {
                id: 'bez_album',
                mod: 'album',
                prio: 2,
                minSec: 1.0,
                nomSec: 2.0,
                render: function(oled, t, st, gridW, gridH, period, globalS) {
                    oled.text(4, 1, 'ALBUM NO PALCO', 1, 1, 1);
                    oled.drawBatteryIcon(110, 1, 1);
                    oled.line(0, 9, gridW - 1, 9, 1);

                    const topAlb = st.topAlbums[0];
                    const name = topAlb ? normAsciiOled(topAlb.name) : 'DISCOTECA PROPRIA';
                    const art = topAlb ? normAsciiOled(topAlb.artist) : 'VARIOS ARTISTAS';

                    oled.rrect(6, 14, 28, 28, false, 1);
                    oled.circle(20, 28, 9, false, 1);
                    oled.circle(20, 28, 3, true, 1);

                    drawMarqueeText(oled, 18, name, globalS, 38, gridW - 4, 1, 1, 1);
                    drawMarqueeText(oled, 30, art, globalS, 38, gridW - 4, 1, 1, 1);

                    oled.rrect(4, 48, gridW - 8, 12, true, 1);
                    const sess = topAlb && topAlb.fullSessions ? `${topAlb.fullSessions}X COMPLETO` : 'ALBUM MAIS OUVIDO';
                    oled.textCenter(50, sess, 0, 1, 1, 20);
                }
            },
            {
                id: 'bez_spectrum',
                mod: 'clock24h',
                prio: 2,
                minSec: 1.2,
                nomSec: 2.5,
                render: function(oled, t, st, gridW, gridH, period, globalS) {
                    oled.text(4, 1, 'RTA > ESPECTRO 24H', 1, 1, 1);
                    oled.drawBatteryIcon(110, 1, 1);
                    oled.line(0, 9, gridW - 1, 9, 1);

                    const peakH = st.peakHour !== undefined ? st.peakHour : 20;
                    const hours = st.hoursSec || new Array(24).fill(0);
                    const maxVal = Math.max(1, ...hours);

                    const startX = 8;
                    const baseLine = 52;
                    const maxH = 38;

                    for (let b = 0; b < 16; b++) {
                        const hourIdx = Math.floor((b / 16) * 24);
                        const ratio = hours[hourIdx] / maxVal;
                        const wave = 0.5 + 0.5 * Math.sin(globalS * 10 + b * 0.8);
                        const barH = Math.min(maxH, Math.max(2, Math.round((ratio * 0.6 + wave * 0.4) * maxH * wrpEaseOutCubic(t))));
                        const bx = startX + b * 7;
                        oled.rect(bx, baseLine - barH, 5, barH, true, 1);
                        oled.line(bx, Math.max(11, baseLine - barH - 2), bx + 4, Math.max(11, baseLine - barH - 2), 1);
                    }
                    oled.line(startX - 2, baseLine + 1, startX + 113, baseLine + 1, 1);
                    oled.textCenter(55, `PICO PRINCIPAL: ${String(peakH).padStart(2, '0')}H`, 1, 1, 1, 20);
                }
            },
            {
                id: 'bez_ticket',
                mod: 'persona',
                prio: 1,
                minSec: 1.5,
                nomSec: 2.8,
                render: function(oled, t, st, gridW, gridH, period, globalS) {
                    oled.text(4, 1, 'MPS3 VIP BADGE', 1, 1, 1);
                    oled.drawBatteryIcon(110, 1, 1);
                    oled.line(0, 9, gridW - 1, 9, 1);

                    const title = normAsciiOled(st.persona.title || 'EXPLORADOR SONORO');
                    oled.rrect(4, 13, gridW - 8, 14, true, 1);
                    drawMarqueeText(oled, 16, title, globalS, 6, gridW - 8, 0, 1, 1);

                    const badge = st.persona.badge || 'VIP IDENTIFIED';
                    oled.textCenter(31, badge, 1, 1, 1, 20);

                    for (let bx = 10; bx < gridW - 10; bx += 2) {
                        const barH = ((bx * 7 + (st.totalScrobbles || 3)) % 4 === 0) ? 8 : 5;
                        oled.rect(bx, 44, 1, barH, true, 1);
                    }
                    oled.textCenter(55, 'MPS3 * BIT-PERFECT', 1, 1, 1, 20);
                }
            },
            {
                id: 'bez_final',
                mod: null,
                prio: 1,
                minSec: 0.8,
                nomSec: 2.0,
                render: function(oled, t, st, gridW, gridH, period, globalS) {
                    oled.rrect(2, 2, gridW - 4, gridH - 4, false, 1);
                    oled.textCenter(14, 'MPS3 PORTABLE', 1, 2, 1, 13);
                    oled.textCenter(34, 'HI-RES AUDIO PLAYER', 1, 1, 1, 20);
                    oled.line(14, 46, gridW - 15, 46, 1);
                    oled.textCenter(50, 'PRONTO PARA O PLAY', 1, 1, 1, 20);
                }
            }
        ];

        const STORYBOARDS = {
            festival: FESTIVAL_SCENES_BLUEPRINT,
            storyboard: STORYBOARD_SCENES_BLUEPRINT,
            oled_bezel: OLED_BEZEL_SCENES_BLUEPRINT
        };

        function getActiveStoryboard(durationS, fps = 30) {
            const style = WRAPPED_VIDEO_STATE.storyboardStyle || 'festival';
            const blueprints = STORYBOARDS[style] || FESTIVAL_SCENES_BLUEPRINT;
            const modules = WRAPPED_VIDEO_STATE.modules || {};

            const filtered = blueprints.filter(scene => {
                if (!scene.mod) return true;
                if (Array.isArray(scene.mod)) {
                    return scene.mod.some(m => modules[m] !== false);
                }
                return modules[scene.mod] !== false;
            });

            const scenesToFit = filtered.length >= 2 ? filtered : blueprints;
            const sb = fitStoryboard(scenesToFit, durationS, fps);
            sb.style = style;
            return sb;
        }

        // --- 6. RENDERIZADOR DE QUADRO DE VÍDEO (CANVAS SOCIAL + OLED) ---
        function renderWrappedVideoFrame(ctx, storyboard, frameIdx, st, pal, format, template, handleVal) {
            const W = ctx.canvas.width;
            const H = ctx.canvas.height;
            const period = WRAPPED_STATE.period || 'all';

            // 1. Fundo Gradiente e Moldura Social do MPS3 Wrapped
            ctx.fillStyle = pal.bgGradient ? pal.bgGradient[0] : '#040711';
            ctx.fillRect(0, 0, W, H);

            const glow1 = ctx.createRadialGradient(W * 0.82, H * 0.14, 20, W * 0.82, H * 0.14, W * 0.65);
            glow1.addColorStop(0, pal.accent + '3A');
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
            const pad = 36;
            let y = 74;
            drawMps3Emblem(ctx, pad + 26, y + 22, 24, pal.accent);
            ctx.fillStyle = pal.textMain;
            ctx.font = '800 34px system-ui, -apple-system, Segoe UI, sans-serif';
            ctx.fillText('MPS3', pad + 68, y + 33);
            ctx.fillStyle = pal.accent;
            ctx.font = '700 22px system-ui, -apple-system, Segoe UI, sans-serif';
            ctx.fillText('WRAPPED', pad + 174, y + 31);

            const periodLabels = { all: 'HISTÓRICO COMPLETO', year: String(new Date().getFullYear()), '30d': 'ÚLTIMOS 30 DIAS', '7d': 'ÚLTIMOS 7 DIAS' };
            const pTag = periodLabels[period] || String(new Date().getFullYear());
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

            // 3. Localiza a cena ativa do Storyboard para este frameIdx
            const scenes = storyboard.scenes;
            let activeScene = scenes[scenes.length - 1];
            for (const sc of scenes) {
                if (frameIdx >= sc.startFrame && frameIdx <= sc.endFrame) {
                    activeScene = sc;
                    break;
                }
            }

            const tScene = activeScene.frameCount > 1
                ? Math.max(0, Math.min(1.0, (frameIdx - activeScene.startFrame) / (activeScene.frameCount - 1)))
                : 1.0;

            const isCompact = (H <= 1120);
            const isFeed = (H === 1350);
            const isBezel = (storyboard.style === 'oled_bezel' || WRAPPED_VIDEO_STATE.storyboardStyle === 'oled_bezel');
            const gridW = 128;
            const gridH = isBezel ? 64 : (isCompact ? 116 : (isFeed ? 150 : 216));
            const pitch = isBezel ? (isCompact ? 6 : 7) : 8;
            const ox = Math.floor((W - gridW * pitch) / 2);
            const oy = isBezel ? Math.floor((H - gridH * pitch) / 2) + 18 : (isCompact ? y : (isFeed ? y : 146));

            // Bezel metálico e chassi do hardware MPS3 quando no estilo oled_bezel
            if (isBezel) {
                const totalW = gridW * pitch;
                const totalH = gridH * pitch;
                const bezelMarginX = 36;
                const bezelMarginY = 32;
                const bx = ox - bezelMarginX;
                const by = oy - bezelMarginY;
                const bw = totalW + bezelMarginX * 2;
                const bh = totalH + bezelMarginY * 2;

                // Corpo do chassi
                drawRoundRect(ctx, bx, by, bw, bh, 18);
                ctx.fillStyle = '#10141d';
                ctx.fill();
                ctx.strokeStyle = '#2a3547';
                ctx.lineWidth = 4;
                ctx.stroke();

                // Chanfro interno escuro
                drawRoundRect(ctx, ox - 10, oy - 10, totalW + 20, totalH + 20, 8);
                ctx.fillStyle = '#060a10';
                ctx.fill();
                ctx.strokeStyle = '#1b222f';
                ctx.lineWidth = 2;
                ctx.stroke();

                // 4 Parafusos Allen nos cantos do chassi
                const screws = [
                    [bx + 16, by + 16],
                    [bx + bw - 16, by + 16],
                    [bx + 16, by + bh - 16],
                    [bx + bw - 16, by + bh - 16]
                ];
                ctx.fillStyle = '#3a4659';
                for (const [sx, sy] of screws) {
                    ctx.beginPath();
                    ctx.arc(sx, sy, 5, 0, Math.PI * 2);
                    ctx.fill();
                }

                // Inscrição gravada a laser no topo e base do bezel
                ctx.fillStyle = '#6e809c';
                ctx.font = '700 13px ui-monospace, SFMono-Regular, Consolas, monospace';
                ctx.textAlign = 'center';
                ctx.fillText('MPS3 HIGH-RESOLUTION PORTABLE AUDIO', W / 2, by + 20);
                ctx.fillText('DUAL ES9038Q2M • BIT-PERFECT DIRECT', W / 2, by + bh - 12);
                ctx.textAlign = 'left';
            }

            const oled = createOled1BitBuffer(gridW, gridH);
            const globalS = frameIdx / (storyboard.fps || 30);
            activeScene.render(oled, tScene, st, gridW, gridH, period, globalS, storyboard);

            // 4. Blit rápido e nítido
            blitFast(ctx, oled, ox, oy, pitch, pal);

            // 5. Assinatura do usuário no rodapé
            if (handleVal) {
                let subTag = 'MPS3 MENSAL';
                if (period === '7d') subTag = 'MPS3 SEMANAL';
                else if (period === '24h' || period === '1d') subTag = 'MPS3 DIARIO';
                else if (period === 'year' && new Date().getMonth() === 11) subTag = 'MPS3 WRAPPED';
                ctx.fillStyle = pal.textSub;
                ctx.font = '700 21px ui-monospace, monospace';
                const innerW = W - pad * 2;
                ctx.fillText(fitText(ctx, `${handleVal} • ${subTag}`, innerW), pad, H - 24);
            }
        }

        // --- 7. MUXER MP4 PURO EM JAVASCRIPT (ISO-BMFF / H.264 AVC1, 0 DEPENDÊNCIAS) ---
        class MiniMp4Muxer {
            constructor(width, height, fps, timescale = 30000) {
                this.width = width;
                this.height = height;
                this.fps = fps;
                this.timescale = timescale;
                this.samples = [];
                this.avcC = null;
            }

            setAvcC(bytes) {
                this.avcC = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
            }

            addSample(data, isKey, durationTimescale) {
                const sampleBytes = data instanceof Uint8Array ? data : new Uint8Array(data);
                this.samples.push({
                    data: sampleBytes,
                    isKey: !!isKey,
                    size: sampleBytes.byteLength,
                    duration: durationTimescale || Math.round(this.timescale / this.fps)
                });
            }

            _u32(val) {
                return [ (val >>> 24) & 0xff, (val >>> 16) & 0xff, (val >>> 8) & 0xff, val & 0xff ];
            }

            _u16(val) {
                return [ (val >>> 8) & 0xff, val & 0xff ];
            }

            _fourcc(str) {
                return [ str.charCodeAt(0), str.charCodeAt(1), str.charCodeAt(2), str.charCodeAt(3) ];
            }

            _box(type, payload) {
                const size = 8 + payload.length;
                const out = new Uint8Array(size);
                out.set(this._u32(size), 0);
                out.set(this._fourcc(type), 4);
                out.set(payload, 8);
                return out;
            }

            _concat(...arrays) {
                let total = 0;
                for (const a of arrays) total += a.length;
                const out = new Uint8Array(total);
                let off = 0;
                for (const a of arrays) {
                    out.set(a, off);
                    off += a.length;
                }
                return out;
            }

            buildFtyp() {
                const payload = new Uint8Array([
                    ...this._fourcc('isom'),
                    ...this._u32(0x00000200),
                    ...this._fourcc('isom'),
                    ...this._fourcc('iso2'),
                    ...this._fourcc('avc1'),
                    ...this._fourcc('mp41')
                ]);
                return this._box('ftyp', payload);
            }

            finalize(mdatOffsetBase = 0) {
                let totalMediaSize = 0;
                let totalDuration = 0;
                for (const s of this.samples) {
                    totalMediaSize += s.size;
                    totalDuration += s.duration;
                }

                // 1. Constroi o payload de mdat
                const mdatPayload = new Uint8Array(totalMediaSize);
                let mOff = 0;
                for (const s of this.samples) {
                    mdatPayload.set(s.data, mOff);
                    mOff += s.size;
                }
                const mdatBox = this._box('mdat', mdatPayload);

                // 2. Cria as caixas moov/trak/mdia/minf/stbl
                const mvhd = this._box('mvhd', new Uint8Array([
                    0, 0, 0, 0,                      // version & flags
                    0, 0, 0, 0,                      // creation_time
                    0, 0, 0, 0,                      // modification_time
                    ...this._u32(this.timescale),    // timescale
                    ...this._u32(totalDuration),     // duration
                    0, 1, 0, 0,                      // rate = 1.0 (fixed 16.16)
                    1, 0,                            // volume = 1.0 (fixed 8.8)
                    0, 0,                            // reserved
                    0, 0, 0, 0, 0, 0, 0, 0,          // reserved
                    // Matrix identity 3x3 (fixed point)
                    0x00, 0x01, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0,
                    0, 0, 0, 0, 0x00, 0x01, 0x00, 0x00, 0, 0, 0, 0,
                    0, 0, 0, 0, 0, 0, 0, 0, 0x40, 0x00, 0x00, 0x00,
                    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                    ...this._u32(2)                  // next_track_ID
                ]));

                const tkhd = this._box('tkhd', new Uint8Array([
                    0, 0, 0, 3,                      // flags = enabled | in_movie
                    0, 0, 0, 0,                      // creation_time
                    0, 0, 0, 0,                      // modification_time
                    ...this._u32(1),                 // track_id = 1
                    0, 0, 0, 0,                      // reserved
                    ...this._u32(totalDuration),     // duration
                    0, 0, 0, 0, 0, 0, 0, 0,          // reserved
                    0, 0,                            // layer
                    0, 0,                            // alternate_group
                    0, 0,                            // volume (0 for video)
                    0, 0,                            // reserved
                    // Matrix identity
                    0x00, 0x01, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0,
                    0, 0, 0, 0, 0x00, 0x01, 0x00, 0x00, 0, 0, 0, 0,
                    0, 0, 0, 0, 0, 0, 0, 0, 0x40, 0x00, 0x00, 0x00,
                    ...this._u32(this.width << 16),  // width (16.16)
                    ...this._u32(this.height << 16)  // height (16.16)
                ]));

                const mdhd = this._box('mdhd', new Uint8Array([
                    0, 0, 0, 0,
                    0, 0, 0, 0,
                    0, 0, 0, 0,
                    ...this._u32(this.timescale),
                    ...this._u32(totalDuration),
                    0x55, 0xc4,                      // language 'und'
                    0, 0
                ]));

                const hdlr = this._box('hdlr', new Uint8Array([
                    0, 0, 0, 0,
                    0, 0, 0, 0,
                    ...this._fourcc('vide'),
                    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                    ...Array.from('VideoHandler\0', c => c.charCodeAt(0))
                ]));

                const vmhd = this._box('vmhd', new Uint8Array([
                    0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0
                ]));

                const drefUrl = this._box('url ', new Uint8Array([ 0, 0, 0, 1 ])); // self-contained
                const dref = this._box('dref', new Uint8Array([
                    0, 0, 0, 0,
                    ...this._u32(1),
                    ...drefUrl
                ]));
                const dinf = this._box('dinf', dref);

                // stsd (avc1 + avcC)
                const avcCBox = this.avcC ? this._box('avcC', this.avcC) : new Uint8Array(0);
                const avc1Payload = new Uint8Array([
                    0, 0, 0, 0, 0, 0,               // reserved
                    ...this._u16(1),                // data_reference_index
                    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                    ...this._u16(this.width),
                    ...this._u16(this.height),
                    0x00, 0x48, 0x00, 0x00,         // 72 dpi horiz
                    0x00, 0x48, 0x00, 0x00,         // 72 dpi vert
                    0, 0, 0, 0,
                    ...this._u16(1),                // frame_count
                    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // compressorname (32 bytes)
                    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                    ...this._u16(0x0018),           // depth 24
                    0xff, 0xff,                     // pre_defined -1
                    ...avcCBox
                ]);
                const avc1 = this._box('avc1', avc1Payload);
                const stsd = this._box('stsd', new Uint8Array([
                    0, 0, 0, 0,
                    ...this._u32(1),
                    ...avc1
                ]));

                // stts: time to sample
                const stts = this._box('stts', new Uint8Array([
                    0, 0, 0, 0,
                    ...this._u32(1),
                    ...this._u32(this.samples.length),
                    ...this._u32(this.samples[0] ? this.samples[0].duration : Math.round(this.timescale / this.fps))
                ]));

                // stss: sync samples (keyframes, 1-based)
                const keyIndices = [];
                for (let i = 0; i < this.samples.length; i++) {
                    if (this.samples[i].isKey) keyIndices.push(i + 1);
                }
                const stssPayload = [ 0, 0, 0, 0, ...this._u32(keyIndices.length) ];
                for (const ki of keyIndices) stssPayload.push(...this._u32(ki));
                const stss = this._box('stss', new Uint8Array(stssPayload));

                // stsc: sample to chunk (1 chunk com todas as amostras)
                const stsc = this._box('stsc', new Uint8Array([
                    0, 0, 0, 0,
                    ...this._u32(1),
                    ...this._u32(1),                    // first_chunk = 1
                    ...this._u32(this.samples.length),  // samples_per_chunk
                    ...this._u32(1)                     // sample_description_index = 1
                ]));

                // stsz: sample sizes
                const stszPayload = [ 0, 0, 0, 0, 0, 0, 0, 0, ...this._u32(this.samples.length) ];
                for (const s of this.samples) stszPayload.push(...this._u32(s.size));
                const stsz = this._box('stsz', new Uint8Array(stszPayload));

                // stco: chunk offset provisório
                const stcoPayload = [ 0, 0, 0, 0, ...this._u32(1), ...this._u32(0) ];
                const stco = this._box('stco', new Uint8Array(stcoPayload));

                const stbl = this._box('stbl', this._concat(stsd, stts, stss, stsc, stsz, stco));
                const minf = this._box('minf', this._concat(vmhd, dinf, stbl));
                const mdia = this._box('mdia', this._concat(mdhd, hdlr, minf));
                const trak = this._box('trak', this._concat(tkhd, mdia));
                const moov = this._box('moov', this._concat(mvhd, trak));

                const ftyp = this.buildFtyp();

                // Calcula offset final real do mdat: ftyp.length + moov.length + 8
                const actualMdatOffset = ftyp.length + moov.length + 8;
                // Patch cirúrgico do stco no moov construído
                const stcoOffsetInMoov = moov.length - 8;
                moov.set(this._u32(actualMdatOffset), stcoOffsetInMoov + 4);

                return this._concat(ftyp, moov, mdatBox);
            }
        }

        // --- 8. PIPELINE DE CODIFICAÇÃO (WEBCODECS OFFLINE + MEDIARECORDER FALLBACK) ---
        async function checkWebCodecsAvcSupport(w, h, fps) {
            if (typeof VideoEncoder === 'undefined' || typeof VideoFrame === 'undefined') {
                return false;
            }
            try {
                const res = await VideoEncoder.isConfigSupported({
                    codec: 'avc1.42001f', // Baseline @ L3.1
                    width: w,
                    height: h,
                    bitrate: 5_000_000,
                    framerate: fps
                });
                return !!(res && res.supported);
            } catch (e) {
                return false;
            }
        }

        async function encodeWithWebCodecs(canvas, storyboard, st, pal, format, template, handleVal, onProgress, abortSignal) {
            const W = canvas.width;
            const H = canvas.height;
            const fps = storyboard.fps;
            const totalFrames = storyboard.totalFrames;

            const muxer = new MiniMp4Muxer(W, H, fps, 30000);
            let encoderError = null;

            const encoder = new VideoEncoder({
                output: (chunk, metadata) => {
                    if (metadata && metadata.decoderConfig && metadata.decoderConfig.description) {
                        muxer.setAvcC(metadata.decoderConfig.description);
                    }
                    const sampleData = new Uint8Array(chunk.byteLength);
                    chunk.copyTo(sampleData);
                    muxer.addSample(sampleData, chunk.type === 'key', chunk.duration);
                },
                error: (e) => {
                    encoderError = e;
                }
            });

            encoder.configure({
                codec: 'avc1.42001f',
                width: W,
                height: H,
                bitrate: 5_500_000,
                framerate: fps,
                avc: { format: 'avc' }
            });

            const ctx = canvas.getContext('2d');
            const frameDurationMicro = Math.round(1_000_000 / fps);

            for (let f = 0; f < totalFrames; f++) {
                if (abortSignal && abortSignal.aborted) {
                    encoder.close();
                    throw new Error('Codificação cancelada.');
                }
                if (encoderError) {
                    throw encoderError;
                }

                renderWrappedVideoFrame(ctx, storyboard, f, st, pal, format, template, handleVal);

                const timestampMicro = Math.round(f * frameDurationMicro);
                const vFrame = new VideoFrame(canvas, {
                    timestamp: timestampMicro,
                    duration: frameDurationMicro
                });

                const isKey = (f % (fps * 2) === 0);
                encoder.encode(vFrame, { keyFrame: isKey });
                vFrame.close();

                if (onProgress) {
                    onProgress(f + 1, totalFrames, Math.round(((f + 1) / totalFrames) * 100));
                }

                // Cede o loop de eventos para manter a UI responsiva
                if (f % 12 === 0) {
                    await new Promise(r => setTimeout(r, 0));
                }
            }

            await encoder.flush();
            encoder.close();

            const mp4Bytes = muxer.finalize();
            return new Blob([mp4Bytes], { type: 'video/mp4' });
        }

        async function encodeWithMediaRecorder(canvas, storyboard, st, pal, format, template, handleVal, onProgress, abortSignal) {
            const fps = storyboard.fps;
            const totalFrames = storyboard.totalFrames;
            const ctx = canvas.getContext('2d');

            const mimeCandidates = [
                'video/mp4; codecs="avc1.42001f"',
                'video/mp4',
                'video/webm; codecs="vp9"',
                'video/webm; codecs="vp8"',
                'video/webm'
            ];
            let chosenMime = '';
            for (const m of mimeCandidates) {
                if (typeof MediaRecorder !== 'undefined' && MediaRecorder.isTypeSupported && MediaRecorder.isTypeSupported(m)) {
                    chosenMime = m;
                    break;
                }
            }

            const stream = canvas.captureStream ? canvas.captureStream(fps) : null;
            if (!stream) throw new Error('O navegador não suporta captura de vídeo do canvas.');

            const chunks = [];
            const recorder = new MediaRecorder(stream, chosenMime ? { mimeType: chosenMime, videoBitsPerSecond: 5_000_000 } : undefined);

            recorder.ondataavailable = (e) => {
                if (e.data && e.data.size > 0) chunks.push(e.data);
            };

            const recordPromise = new Promise((resolve, reject) => {
                recorder.onstop = () => {
                    const finalType = chosenMime.startsWith('video/mp4') ? 'video/mp4' : 'video/webm';
                    resolve(new Blob(chunks, { type: finalType }));
                };
                recorder.onerror = reject;
            });

            recorder.start();

            const frameDelayMs = 1000 / fps;
            for (let f = 0; f < totalFrames; f++) {
                if (abortSignal && abortSignal.aborted) {
                    recorder.stop();
                    throw new Error('Codificação cancelada.');
                }
                renderWrappedVideoFrame(ctx, storyboard, f, st, pal, format, template, handleVal);
                if (onProgress) {
                    onProgress(f + 1, totalFrames, Math.round(((f + 1) / totalFrames) * 100));
                }
                await new Promise(r => setTimeout(r, frameDelayMs));
            }

            recorder.stop();
            return await recordPromise;
        }

        // --- 9. INTERFACE DO USUÁRIO & CONTROLADORES ---
        function setWrappedMediaType(type) {
            WRAPPED_VIDEO_STATE.mode = type;
            const isVideo = (type === 'video');

            document.querySelectorAll('#wrpMediaTypeGroup .wrp-pill').forEach(b => {
                b.classList.toggle('active', b.getAttribute('data-mtype') === type);
            });

            const vBox = document.getElementById('wrpVideoControlsBox');
            if (vBox) vBox.style.display = isVideo ? 'block' : 'none';

            const imgStack = document.getElementById('wrpImageActionsStack');
            if (imgStack) imgStack.style.display = isVideo ? 'none' : 'flex';

            const vidStack = document.getElementById('wrpVideoActionsStack');
            if (vidStack) vidStack.style.display = isVideo ? 'flex' : 'none';

            if (!isVideo && WRAPPED_VIDEO_STATE.isPreviewing) {
                stopWrappedVideoPreview();
            }
        }

        function setWrappedVideoStyle(style) {
            if (!['festival', 'storyboard', 'oled_bezel'].includes(style)) return;
            WRAPPED_VIDEO_STATE.storyboardStyle = (style === 'oled_bezel') ? 'festival' : style;
            if (typeof document !== 'undefined' && document.querySelectorAll) {
                document.querySelectorAll('#wrpVideoStyleGroup .wrp-pill').forEach(b => {
                    b.classList.toggle('active', b.getAttribute('data-vstyle') === WRAPPED_VIDEO_STATE.storyboardStyle);
                });
            }
            if (WRAPPED_VIDEO_STATE.isPreviewing) {
                stopWrappedVideoPreview();
                startWrappedVideoPreview();
            }
        }

        function toggleWrappedVideoModule(modKey) {
            if (!WRAPPED_VIDEO_STATE.modules || WRAPPED_VIDEO_STATE.modules[modKey] === undefined) return;
            const activeCount = Object.values(WRAPPED_VIDEO_STATE.modules).filter(Boolean).length;
            if (WRAPPED_VIDEO_STATE.modules[modKey] && activeCount <= 1) {
                return; // Mantém pelo menos um módulo ativo
            }
            WRAPPED_VIDEO_STATE.modules[modKey] = !WRAPPED_VIDEO_STATE.modules[modKey];
            syncWrappedVideoModulesUI();
            if (WRAPPED_VIDEO_STATE.isPreviewing) {
                stopWrappedVideoPreview();
                startWrappedVideoPreview();
            }
        }

        function resetWrappedVideoModules() {
            for (const k in WRAPPED_VIDEO_STATE.modules) {
                WRAPPED_VIDEO_STATE.modules[k] = true;
            }
            syncWrappedVideoModulesUI();
            if (WRAPPED_VIDEO_STATE.isPreviewing) {
                stopWrappedVideoPreview();
                startWrappedVideoPreview();
            }
        }

        function syncWrappedVideoModulesUI() {
            if (typeof document !== 'undefined' && document.querySelectorAll) {
                document.querySelectorAll('#wrpVideoModulesGroup .wrp-pill').forEach(b => {
                    const mod = b.getAttribute('data-vmod');
                    b.classList.toggle('active', !!WRAPPED_VIDEO_STATE.modules[mod]);
                });
            }
        }

        function setWrappedVideoPreset(presetId) {
            if (!WRP_VIDEO_CFG.presets[presetId]) return;
            WRAPPED_VIDEO_STATE.preset = presetId;
            document.querySelectorAll('#wrpVideoDurationGroup .wrp-pill').forEach(b => {
                b.classList.toggle('active', b.getAttribute('data-vpreset') === presetId);
            });
            if (WRAPPED_VIDEO_STATE.isPreviewing) {
                stopWrappedVideoPreview();
                startWrappedVideoPreview();
            }
        }

        function toggleWrappedVideoPreview() {
            if (WRAPPED_VIDEO_STATE.isPreviewing) {
                stopWrappedVideoPreview();
            } else {
                startWrappedVideoPreview();
            }
        }

        function startWrappedVideoPreview() {
            const cvs = document.getElementById('wrappedCanvas');
            if (!cvs) return;
            const ctx = cvs.getContext('2d');
            const st = WRAPPED_STATE.stats || computeWrappedAnalytics(
                WRAPPED_STATE.rawEvents.length ? WRAPPED_STATE.rawEvents : generateDemoEvents(),
                WRAPPED_STATE.period,
                WRAPPED_STATE.category
            );
            const pal = WRP_PALETTES[WRAPPED_STATE.cardTheme] || WRP_PALETTES.oled_blue;
            const handleVal = (document.getElementById('wrpUserHandleInput')?.value || '').trim();
            const preset = WRP_VIDEO_CFG.presets[WRAPPED_VIDEO_STATE.preset] || WRP_VIDEO_CFG.presets.rapido;

            const storyboard = getActiveStoryboard(preset.sec, WRP_VIDEO_CFG.fps);

            WRAPPED_VIDEO_STATE.isPreviewing = true;
            WRAPPED_VIDEO_STATE.previewStartTs = performance.now();

            const pBtn = document.getElementById('btnWrpPreviewVideo');
            if (pBtn) pBtn.textContent = '⏹ Parar Prévia';

            function loop(now) {
                if (!WRAPPED_VIDEO_STATE.isPreviewing) return;
                const elapsedS = ((now - WRAPPED_VIDEO_STATE.previewStartTs) / 1000) % storyboard.durationS;
                const frameIdx = Math.min(storyboard.totalFrames - 1, Math.floor(elapsedS * storyboard.fps));

                renderWrappedVideoFrame(ctx, storyboard, frameIdx, st, pal, WRAPPED_STATE.format, WRAPPED_STATE.template, handleVal);
                WRAPPED_VIDEO_STATE.previewRafId = requestAnimationFrame(loop);
            }
            WRAPPED_VIDEO_STATE.previewRafId = requestAnimationFrame(loop);
        }

        function stopWrappedVideoPreview() {
            WRAPPED_VIDEO_STATE.isPreviewing = false;
            if (WRAPPED_VIDEO_STATE.previewRafId) {
                cancelAnimationFrame(WRAPPED_VIDEO_STATE.previewRafId);
                WRAPPED_VIDEO_STATE.previewRafId = null;
            }
            const pBtn = document.getElementById('btnWrpPreviewVideo');
            if (pBtn) pBtn.textContent = '▶ Pré-visualizar Animação';
            // Restaura o pôster estático no canvas
            renderWrappedCanvas();
        }

        async function generateWrappedVideo() {
            if (WRAPPED_VIDEO_STATE.isEncoding) return;
            if (WRAPPED_VIDEO_STATE.isPreviewing) stopWrappedVideoPreview();

            const cvs = document.getElementById('wrappedCanvas');
            if (!cvs) return;

            const progWrap = document.getElementById('wrpVideoProgressWrap');
            const progStatus = document.getElementById('wrpVideoProgressStatus');
            const progPct = document.getElementById('wrpVideoProgressPct');
            const progBar = document.getElementById('wrpVideoProgressBar');
            const genBtn = document.getElementById('btnWrpGenerateVideo');
            const cancelBtn = document.getElementById('btnWrpCancelVideo');

            const preset = WRP_VIDEO_CFG.presets[WRAPPED_VIDEO_STATE.preset] || WRP_VIDEO_CFG.presets.rapido;
            const st = WRAPPED_STATE.stats || computeWrappedAnalytics(
                WRAPPED_STATE.rawEvents.length ? WRAPPED_STATE.rawEvents : generateDemoEvents(),
                WRAPPED_STATE.period,
                WRAPPED_STATE.category
            );
            const pal = WRP_PALETTES[WRAPPED_STATE.cardTheme] || WRP_PALETTES.oled_blue;
            const handleVal = (document.getElementById('wrpUserHandleInput')?.value || '').trim();
            const storyboard = getActiveStoryboard(preset.sec, WRP_VIDEO_CFG.fps);

            WRAPPED_VIDEO_STATE.isEncoding = true;
            WRAPPED_VIDEO_STATE.cancelRequested = false;
            const abortController = new AbortController();

            if (progWrap) progWrap.style.display = 'block';
            if (cancelBtn) {
                cancelBtn.style.display = 'inline-block';
                cancelBtn.onclick = () => {
                    abortController.abort();
                };
            }
            if (genBtn) genBtn.disabled = true;

            const onProgress = (curF, totF, pct) => {
                if (progStatus) progStatus.textContent = `Renderizando quadro ${curF}/${totF} (1-bit)...`;
                if (progPct) progPct.textContent = `${pct}%`;
                if (progBar) progBar.style.width = `${pct}%`;
            };

            try {
                const canAvc = await checkWebCodecsAvcSupport(cvs.width, cvs.height, WRP_VIDEO_CFG.fps);
                let blob = null;

                if (canAvc) {
                    if (progStatus) progStatus.textContent = `Codificando H.264 offline (${preset.frames} quadros)...`;
                    blob = await encodeWithWebCodecs(
                        cvs, storyboard, st, pal, WRAPPED_STATE.format, WRAPPED_STATE.template, handleVal, onProgress, abortController.signal
                    );
                    WRAPPED_VIDEO_STATE.encoderEngine = 'webcodecs';
                } else {
                    if (progStatus) progStatus.textContent = `Codificando em tempo real (MediaRecorder)...`;
                    blob = await encodeWithMediaRecorder(
                        cvs, storyboard, st, pal, WRAPPED_STATE.format, WRAPPED_STATE.template, handleVal, onProgress, abortController.signal
                    );
                    WRAPPED_VIDEO_STATE.encoderEngine = 'mediarecorder';
                }

                WRAPPED_VIDEO_STATE.lastVideoBlob = blob;
                if (WRAPPED_VIDEO_STATE.lastVideoUrl) {
                    URL.revokeObjectURL(WRAPPED_VIDEO_STATE.lastVideoUrl);
                }
                WRAPPED_VIDEO_STATE.lastVideoUrl = URL.createObjectURL(blob);

                if (progStatus) progStatus.textContent = `✅ Vídeo pronto! (${(blob.size / 1024).toFixed(0)} KB)`;
                if (progPct) progPct.textContent = '100%';
                if (progBar) progBar.style.width = '100%';

                const ext = blob.type.includes('mp4') ? 'mp4' : 'webm';
                const dlBtn = document.getElementById('btnWrpDownloadVideo');
                if (dlBtn) dlBtn.textContent = `💾 Baixar Vídeo (${ext.toUpperCase()})`;

            } catch (err) {
                if (progStatus) progStatus.textContent = `Erro: ${err.message || err}`;
            } finally {
                WRAPPED_VIDEO_STATE.isEncoding = false;
                if (genBtn) genBtn.disabled = false;
                if (cancelBtn) cancelBtn.style.display = 'none';
            }
        }

        function cancelWrappedVideoEncoding() {
            WRAPPED_VIDEO_STATE.cancelRequested = true;
        }

        async function downloadWrappedVideo() {
            if (!WRAPPED_VIDEO_STATE.lastVideoBlob) {
                await generateWrappedVideo();
            }
            if (!WRAPPED_VIDEO_STATE.lastVideoBlob) return;

            const blob = WRAPPED_VIDEO_STATE.lastVideoBlob;
            const ext = blob.type.includes('mp4') ? 'mp4' : 'webm';
            const url = URL.createObjectURL(blob);
            const a = document.createElement('a');
            a.href = url;
            a.download = `MPS3_Wrapped_${WRAPPED_VIDEO_STATE.preset}_${WRAPPED_STATE.cardTheme}.${ext}`;
            document.body.appendChild(a);
            a.click();
            a.remove();
            setTimeout(() => URL.revokeObjectURL(url), 5000);
        }

        async function sendWrappedVideoToPhoneViaMps3() {
            if (!WRAPPED_VIDEO_STATE.lastVideoBlob) {
                await generateWrappedVideo();
            }
            if (!WRAPPED_VIDEO_STATE.lastVideoBlob) return;

            const btn = document.getElementById('btnWrpSendPhoneQrVideo');
            const qrBox = document.getElementById('wrpQrBridgeBox');
            const qrSvgBox = document.getElementById('wrpQrSvgContainer');
            const statusMsg = document.getElementById('wrpQrStatusMsg');
            const urlDisp = document.getElementById('wrpQrUrlDisplay');

            const oldHtml = btn ? btn.innerHTML : '';
            if (btn) {
                btn.disabled = true;
                btn.innerHTML = '<span>⏳ Enviando vídeo para o MPS3...</span>';
            }

            try {
                const blob = WRAPPED_VIDEO_STATE.lastVideoBlob;
                const isMp4 = blob.type.includes('mp4');
                const fname = isMp4 ? 'mps3_wrapped.mp4' : 'mps3_wrapped.webm';
                const shareRelPath = `/.mps3_analytics/share/${fname}`;

                const upRes = await fetch('/api/upload?path=' + encodeURIComponent(shareRelPath) + '&idx=1&count=1&batchTotal=' + blob.size, {
                    method: 'PUT',
                    body: blob
                });
                if (!upRes.ok) {
                    throw new Error('Falha ao gravar vídeo temporário no MPS3 (HTTP ' + upRes.status + ')');
                }

                const verTag = Date.now().toString(36);
                const origin = resolvePhoneReachableOrigin();
                const shareUrl = `${origin}/?share=wrapped&media=video&v=${verTag}`;

                const svgMarkup = generateQrSvg(shareUrl);
                if (svgMarkup && qrSvgBox) {
                    qrSvgBox.innerHTML = svgMarkup;
                } else if (qrSvgBox) {
                    qrSvgBox.innerHTML = `<img src="/api/qr?text=${encodeURIComponent(shareUrl)}" alt="QR Code">`;
                }

                const netHint = WRAPPED_STATE.wifiMode === 'AP'
                    ? 'Conecte o celular no Wi-Fi Hotspot <strong>MPS3</strong> e aponte a câmera para abrir o vídeo:'
                    : 'Com o smartphone no <strong>mesmo Wi-Fi</strong>, aponte a câmera para abrir o vídeo animado e compartilhar:';
                if (statusMsg) statusMsg.innerHTML = netHint;
                if (urlDisp) urlDisp.textContent = shareUrl;
                if (qrBox) {
                    qrBox.style.display = 'block';
                    qrBox.scrollIntoView({ behavior: 'smooth', block: 'nearest' });
                }
            } catch (err) {
                alert('Erro ao enviar vídeo ao celular: ' + (err.message || err));
            } finally {
                if (btn) {
                    btn.disabled = false;
                    btn.innerHTML = oldHtml;
                }
            }
        }

        async function shareWrappedVideoNative() {
            if (!WRAPPED_VIDEO_STATE.lastVideoBlob) {
                await generateWrappedVideo();
            }
            if (!WRAPPED_VIDEO_STATE.lastVideoBlob) return;

            try {
                const blob = WRAPPED_VIDEO_STATE.lastVideoBlob;
                const ext = blob.type.includes('mp4') ? 'mp4' : 'webm';
                const file = new File([blob], `MPS3_Wrapped_${WRAPPED_VIDEO_STATE.preset}.${ext}`, { type: blob.type });
                if (navigator.canShare && navigator.canShare({ files: [file] })) {
                    await navigator.share({
                        files: [file],
                        title: 'Meu MPS3 Wrapped Animado',
                        text: 'Minha retrospectiva musical animada em 1-bit no MPS3 Hi-Res Player!'
                    });
                } else {
                    await downloadWrappedVideo();
                }
            } catch (err) {
                if (err && err.name !== 'AbortError') {
                    await downloadWrappedVideo();
                }
            }
        }

        if (typeof window !== 'undefined') {
            window.WRAPPED_VIDEO_STATE = WRAPPED_VIDEO_STATE;
            window.WRP_VIDEO_CFG = WRP_VIDEO_CFG;
            window.setWrappedMediaType = setWrappedMediaType;
            window.setWrappedVideoPreset = setWrappedVideoPreset;
            window.setWrappedVideoStyle = setWrappedVideoStyle;
            window.toggleWrappedVideoModule = toggleWrappedVideoModule;
            window.resetWrappedVideoModules = resetWrappedVideoModules;
            window.toggleWrappedVideoPreview = toggleWrappedVideoPreview;
            window.startWrappedVideoPreview = startWrappedVideoPreview;
            window.stopWrappedVideoPreview = stopWrappedVideoPreview;
            window.generateWrappedVideo = generateWrappedVideo;
            window.cancelWrappedVideoEncoding = cancelWrappedVideoEncoding;
            window.downloadWrappedVideo = downloadWrappedVideo;
            window.sendWrappedVideoToPhoneViaMps3 = sendWrappedVideoToPhoneViaMps3;
            window.shareWrappedVideoNative = shareWrappedVideoNative;
            window.getActiveStoryboard = getActiveStoryboard;
        }

