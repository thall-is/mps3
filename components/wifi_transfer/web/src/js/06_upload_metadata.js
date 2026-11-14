/* ==========================================================================
 * MÓDULO JS 06: Fila de Upload recursivo, Extrator ID3v2/FLAC/M4A e Redimensionador
 * ========================================================================== */
        // --- UPLOAD ENGINE (WITH MUTEX & QUEUE) ---
        function renderQueue() {
            if(!queueItems.length) { el.queue.innerHTML=''; return; }
            let html = '';
            for(let q of queueItems) {
                let cls = q.status==='done'?'done':(q.status==='error'?'error':(q.status==='active'?'active':''));
                let icon = q.status==='done'?'✓':(q.status==='error'?'✕':(q.status==='active'?'⏳':'⋯'));
                html += `<div class="q-item ${cls}"><span>${q.name}</span><span>${icon}</span></div>`;
            }
            el.queue.innerHTML = html;
            el.queue.scrollTop = el.queue.scrollHeight;
        }

        async function getFilesFromEntry(entry, basePath = '') {
            const files = [];
            if (entry.isFile) {
                return new Promise((res) => {
                    entry.file((f) => { f.relativePath = basePath ? basePath + '/' + f.name : f.name; res([f]); });
                });
            } else if (entry.isDirectory) {
                const reader = entry.createReader();
                const entries = await new Promise((res) => {
                    let results = [];
                    const readBatch = () => {
                        reader.readEntries((b) => {
                            if (!b.length) res(results);
                            else { results = results.concat(b); readBatch(); }
                        });
                    };
                    readBatch();
                });
                const subPath = basePath ? basePath + '/' + entry.name : entry.name;
                for (const sub of entries) {
                    const subFiles = await getFilesFromEntry(sub, subPath);
                    files.push(...subFiles);
                }
                return files;
            }
            return files;
        }

        function uploadOne(relPath, file, idx, count, batchTotal, qid) {
            return new Promise((resolve, reject) => {
                const cleanRelPath = relPath.replace(/\uFF1F/g, '-').replace(/[?*:|"<>\\]/g, '-');
                const xhr = new XMLHttpRequest();
                const url = '/api/upload?path=' + encodeURIComponent(joinPath(curDir, cleanRelPath)) +
                    '&idx=' + idx + '&count=' + count + '&batchTotal=' + batchTotal;
                xhr.open('PUT', url, true);
                // Timeout dinamico proporcional ao tamanho do arquivo (garante tempo suficiente mesmo para 100MB+ e 500MB+)
                xhr.timeout = Math.max(300000, Math.ceil(file.size / 150000) * 1000);
                let t0 = performance.now();
                let lastCalcTime = t0;
                let lastCalcBytes = 0;
                let smoothKbps = 0;
                let lastUiUpdate = 0;

                xhr.upload.onprogress = (ev) => {
                    if (!ev.lengthComputable) return;
                    const now = performance.now();
                    const pct = Math.round(ev.loaded * 100 / ev.total);

                    el.upFileBar.style.width = pct + '%';
                    el.upFilePct.textContent = pct + '%';

                    if (count > 1) {
                        const overallDone = window.__batchDoneBefore + ev.loaded;
                        const opct = batchTotal > 0 ? Math.round(overallDone * 100 / batchTotal) : 0;
                        el.upOverallBar.style.width = opct + '%';
                        el.upOverallPct.textContent = opct + '%';
                        el.upOverallLabel.textContent = 'Geral (' + idx + '/' + count + ')';
                    }

                    const dt = (now - lastCalcTime) / 1000;
                    if (dt >= 0.12) {
                        const dBytes = ev.loaded - lastCalcBytes;
                        const instKbps = dBytes > 0 ? (dBytes / 1024) / dt : smoothKbps;
                        if (smoothKbps <= 0) {
                            smoothKbps = instKbps;
                        } else {
                            // Filtro EMA (75% historico + 25% instantaneo) para eliminar efeito serrote e pausas bruscas
                            smoothKbps = (smoothKbps * 0.75) + (instKbps * 0.25);
                        }
                        lastCalcTime = now;
                        lastCalcBytes = ev.loaded;

                        // Atualiza as legendas de velocidade e tempo a cada 200ms para manter a leitura limpa e estavel
                        if (now - lastUiUpdate >= 200 || pct >= 100) {
                            lastUiUpdate = now;
                            el.upSpeed.textContent = smoothKbps >= 1024 
                                ? (smoothKbps / 1024).toFixed(2) + ' MB/s' 
                                : Math.round(smoothKbps) + ' KB/s';
                            const remKb = (ev.total - ev.loaded) / 1024;
                            const etaSec = smoothKbps > 5 ? remKb / smoothKbps : -1;
                            el.upEta.textContent = etaSec >= 0 ? fmtEta(etaSec) : '--';
                        }
                    }
                };

                xhr.onload = () => {
                    if (xhr.status >= 200 && xhr.status < 300) {
                        let q = queueItems.find(x=>x.id===qid); if(q) q.status='done';
                        renderQueue(); resolve();
                    } else {
                        let q = queueItems.find(x=>x.id===qid); if(q) q.status='error';
                        renderQueue(); reject(new Error('HTTP ' + xhr.status));
                    }
                };
                xhr.onerror = () => { let q = queueItems.find(x=>x.id===qid); if(q) q.status='error'; renderQueue(); reject(new Error('Falha de rede')); };
                xhr.ontimeout = () => { let q = queueItems.find(x=>x.id===qid); if(q) q.status='error'; renderQueue(); reject(new Error('Timeout')); };

                xhr.send(file);
            });
        }

        async function uploadBatch(files) {
            const list = Array.from(files);
            if (!list.length) return;

            // MUTEX: Pausar reprodução e encerrar socket HTTP de streaming imediatamente
            if (el.audio) {
                el.audio.pause();
                if (el.audio.getAttribute('src')) {
                    el.audio.removeAttribute('src');
                    el.audio.load();
                    updatePlayBtn(false);
                }
            }
            isUploading = true;
            updateSdTraffic('writing');

            queueItems = [];
            for (const f of list) {
                const relPath = f.relativePath || f.webkitRelativePath || f.name;
                queueItems.push({ id: ++queueId, name: relPath, size: f.size, status: 'waiting' });
            }
            renderQueue();

            const batchTotal = list.reduce((s, f) => s + f.size, 0);
            const count = list.length;

            el.upBox.style.display = 'block';
            el.upOverallBar.style.width = '0%';
            el.upOverallPct.textContent = '0%';
            el.upFileBar.style.width = '0%';
            el.upFilePct.textContent = '0%';
            el.upSpeed.textContent = '0 KB/s';
            el.upEta.textContent = '--';
            el.upOverallLabel.textContent = 'Geral (0/' + count + ')';
            window.__batchDoneBefore = 0;

            let errored = false;
            for (let i = 0; i < list.length; i++) {
                const f = list[i];
                const relPath = f.relativePath || f.webkitRelativePath || f.name;
                const q = queueItems[i];
                q.status = 'active';
                renderQueue();
                el.upFileName.textContent = relPath;

                let uploaded = false;
                let lastErr = null;
                for (let attempt = 1; attempt <= 3; attempt++) {
                    try {
                        if (attempt > 1) {
                            el.upFileName.textContent = `[Tentativa ${attempt}/3] ${relPath}`;
                            await new Promise(r => setTimeout(r, 800));
                        }
                        await uploadOne(relPath, f, i + 1, count, batchTotal, q.id);
                        uploaded = true;
                        break;
                    } catch (e) {
                        lastErr = e;
                        console.warn(`Tentativa ${attempt} falhou para ${relPath}:`, e);
                    }
                }

                if (!uploaded) {
                    errored = true;
                    alert('Falha ao enviar ' + relPath + ': ' + (lastErr ? lastErr.message : 'Erro de conexao'));
                    break;
                }
                window.__batchDoneBefore += f.size;
                if (i + 1 < list.length) {
                    await new Promise(r => setTimeout(r, 25)); // Pausa minima de 25ms entre arquivos
                }
            }

            isUploading = false;
            fetchStatus();
            load();
            setTimeout(() => {
                el.upBox.style.display = 'none';
                queueItems = [];
                renderQueue();
            }, 4000);
        }

        // --- DRAG & DROP & INPUTS ---
        ['dragenter','dragover','dragleave','drop'].forEach(evt => {
            el.dropzone.addEventListener(evt, e => { e.preventDefault(); e.stopPropagation(); });
        });
        el.dropzone.addEventListener('dragover', () => el.dropzone.classList.add('dragover'));
        el.dropzone.addEventListener('dragleave', () => el.dropzone.classList.remove('dragover'));
        el.dropzone.addEventListener('drop', async (e) => {
            el.dropzone.classList.remove('dragover');
            const dt = e.dataTransfer;
            let files = [];
            if (dt.items && dt.items.length && dt.items[0].webkitGetAsEntry) {
                for (let i = 0; i < dt.items.length; i++) {
                    const entry = dt.items[i].webkitGetAsEntry();
                    if (entry) {
                        const entryFiles = await getFilesFromEntry(entry);
                        files.push(...entryFiles);
                    }
                }
            } else if (dt.files) {
                files = Array.from(dt.files);
            }
            if (files.length) uploadBatch(files);
        });

        el.inpFiles.onchange = (e) => { if (e.target.files.length) uploadBatch(e.target.files); e.target.value = ''; };
        el.inpFolder.onchange = (e) => { if (e.target.files.length) uploadBatch(e.target.files); e.target.value = ''; };

        // --- METADATA EXTRACTION (SAFE NON-BLOCKING & LEAN 128KB) ---
        async function extractMetadata(url) {
            try {
                let ctrl = new AbortController();
                let tid = setTimeout(() => ctrl.abort(), 5000);
                let res = await fetch(url, { headers: { 'Range': 'bytes=0-131071' }, signal: ctrl.signal });
                clearTimeout(tid);
                if (!res.ok && res.status !== 206) return {};
                let buf = await res.arrayBuffer();
                let u8 = new Uint8Array(buf);
                let view = new DataView(buf);
                if (u8[0]===0x49 && u8[1]===0x44 && u8[2]===0x33) return parseID3(u8, view);
                if (u8[0]===0x66 && u8[1]===0x4C && u8[2]===0x61 && u8[3]===0x43) return parseFLAC(u8, view);
            } catch(e) {}
            return {};
        }

        function parseFLAC(u8, view) {
            let offset = 4, tags = { title: null, artist: null, picture: null };
            while(offset < u8.length - 4) {
                let header = view.getUint32(offset), isLast = (header & 0x80000000) !== 0;
                let type = (header >> 24) & 0x7F, length = header & 0xFFFFFF;
                offset += 4;
                if (offset + length > u8.length) break;
                if (type === 4) {
                    let vOffset = offset + 4 + view.getUint32(offset, true);
                    let listLen = view.getUint32(vOffset, true); vOffset += 4;
                    for(let i=0; i<listLen; i++) {
                        let strLen = view.getUint32(vOffset, true); vOffset += 4;
                        let str = new TextDecoder('utf-8').decode(u8.subarray(vOffset, vOffset+strLen));
                        vOffset += strLen;
                        let idx = str.indexOf('=');
                        if (idx > 0) {
                            let k = str.substring(0, idx).toUpperCase(), v = str.substring(idx+1);
                            if (k === 'TITLE') tags.title = v;
                            if (k === 'ARTIST') tags.artist = v;
                        }
                    }
                } else if (type === 6) {
                    let pOffset = offset + 4;
                    let mimeLen = view.getUint32(pOffset); pOffset += 4;
                    let mime = new TextDecoder('ascii').decode(u8.subarray(pOffset, pOffset+mimeLen));
                    pOffset += mimeLen;
                    let descLen = view.getUint32(pOffset); pOffset += 4 + descLen + 16;
                    let picLen = view.getUint32(pOffset); pOffset += 4;
                    let imgData = u8.subarray(pOffset, pOffset+picLen);
                    let binary = '', chunk = 8192;
                    for(let i=0; i<imgData.length; i+=chunk) binary += String.fromCharCode.apply(null, imgData.subarray(i, i+chunk));
                    if (!mime || mime.toLowerCase() === 'jpg') mime = 'image/jpeg';
                    tags.picture = 'data:' + mime + ';base64,' + btoa(binary);
                }
                offset += length;
                if (isLast) break;
            }
            return tags;
        }

        function parseID3(u8, view) {
            let version = u8[3], size = (u8[6]<<21) | (u8[7]<<14) | (u8[8]<<7) | u8[9];
            let offset = 10 + (((u8[5] & 0x40)!==0) ? (version===3 ? 4+view.getUint32(10) : (u8[10]<<21)|(u8[11]<<14)|(u8[12]<<7)|u8[13]) : 0);
            let tags = { title: null, artist: null, picture: null };
            while (offset < size && offset < u8.length - 10) {
                let id = String.fromCharCode(u8[offset], u8[offset+1], u8[offset+2], u8[offset+3]);
                let fsize = version===3 ? view.getUint32(offset+4) : (u8[offset+4]<<21)|(u8[offset+5]<<14)|(u8[offset+6]<<7)|u8[offset+7];
                if (id === '\0\0\0\0' || fsize === 0) break;
                offset += 10;
                if (offset + fsize > u8.length) break;
                
                if (id === 'TIT2') tags.title = decodeString(u8.subarray(offset, offset+fsize));
                if (id === 'TPE1') tags.artist = decodeString(u8.subarray(offset, offset+fsize));
                if (id === 'APIC') {
                    let pBuf = u8.subarray(offset, offset+fsize);
                    let pOff=1, mimeStart=pOff;
                    while(pOff < pBuf.length && pBuf[pOff]!==0) pOff++;
                    let mime = new TextDecoder('ascii').decode(pBuf.subarray(mimeStart, pOff));
                    pOff++;
                    if(mime !== '-->') {
                        pOff++;
                        if(pBuf[0]===1 || pBuf[0]===2) {
                            while(pOff < pBuf.length-1) { if(pBuf[pOff]===0 && pBuf[pOff+1]===0) break; pOff++; }
                            pOff += 2;
                        } else {
                            while(pOff < pBuf.length && pBuf[pOff]!==0) pOff++; pOff++;
                        }
                        let iData = pBuf.subarray(pOff);
                        let binary = '', chunk = 8192;
                        for(let i=0; i<iData.length; i+=chunk) binary += String.fromCharCode.apply(null, iData.subarray(i, i+chunk));
                        if(!mime||mime.toLowerCase()==='jpg') mime='image/jpeg';
                        tags.picture = 'data:'+mime+';base64,'+btoa(binary);
                    }
                }
                offset += fsize;
            }
            return tags;
        }

        function decodeString(b) {
            if(b.length<2) return '';
            let e=b[0];
            return (e===0||e===3) ? new TextDecoder('iso-8859-1').decode(b.subarray(1)).replace(/\0/g,'') : new TextDecoder('utf-16').decode(b.subarray(1)).replace(/\0/g,'');
        }
