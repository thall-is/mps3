/* ==========================================================================
 * MÓDULO JS 05: Operações de Arquivo (Mkdir, Move, Copy, Rename, Delete)
 * ========================================================================== */
        // --- Context Menu Actions ---
        document.getElementById('ctxNext').onclick = ()=>{
            if(!currentCtxItem || !currentCtxItem.isAudio) return;
            if(playIndex === -1) { playlist=[currentCtxItem]; playIndex=0; loadTrack(); }
            else { playlist.splice(playIndex+1, 0, currentCtxItem); }
        };
        document.getElementById('ctxQueue').onclick = ()=>{
            if(!currentCtxItem || !currentCtxItem.isAudio) return;
            if(playIndex === -1) { playlist=[currentCtxItem]; playIndex=0; loadTrack(); }
            else { playlist.push(currentCtxItem); }
        };
        let isDownloading = false;

        async function downloadFileWithProgress(item) {
            if (!item || item.isDir) return;
            if (typeof isUploading !== 'undefined' && isUploading) {
                alert('Upload em andamento! Aguarde o envio terminar.');
                return;
            }
            const path = item.path;
            const name = item.name || path.split('/').pop() || 'arquivo';
            const size = item.size || 0;

            // Se o arquivo for muito grande (> 500 MB) ou ReadableStream não suportado, fallback nativo
            if (size > 500 * 1024 * 1024 || !window.ReadableStream || !window.Response) {
                const a = document.createElement('a');
                a.href = '/api/download?path=' + encodeURIComponent(path);
                a.download = name;
                document.body.appendChild(a);
                a.click();
                document.body.removeChild(a);
                return;
            }

            if (isDownloading) {
                alert('Já existe um download em andamento!');
                return;
            }
            isDownloading = true;

            if (el.dlBox) {
                el.dlBox.style.display = 'block';
                if (el.dlFileName) el.dlFileName.textContent = 'Baixando: ' + name;
                if (el.dlFilePct) el.dlFilePct.textContent = '0%';
                if (el.dlFileBar) el.dlFileBar.style.width = '0%';
                if (el.dlSpeed) el.dlSpeed.textContent = '0 KB/s';
                if (el.dlEta) el.dlEta.textContent = '--';
                if (el.dlSize) el.dlSize.textContent = size > 0 ? ('0 / ' + (size / (1024*1024)).toFixed(1) + ' MB') : '';
            }

            let startTime = performance.now();
            let lastTime = startTime;
            let lastBytes = 0;
            let receivedBytes = 0;
            let chunks = [];

            try {
                const response = await fetch('/api/download?path=' + encodeURIComponent(path));
                if (!response.ok) throw new Error('Falha HTTP ' + response.status);

                const totalBytes = parseInt(response.headers.get('Content-Length') || size || 0, 10);
                const reader = response.body.getReader();

                while (true) {
                    const { done, value } = await reader.read();
                    if (done) break;
                    chunks.push(value);
                    receivedBytes += value.length;

                    const now = performance.now();
                    if (now - lastTime >= 150) {
                        const intervalSec = (now - lastTime) / 1000;
                        const speedBps = (receivedBytes - lastBytes) / intervalSec;
                        lastTime = now;
                        lastBytes = receivedBytes;

                        const speedKB = speedBps / 1024;
                        const speedMB = speedKB / 1024;
                        const speedStr = speedMB >= 1 ? (speedMB.toFixed(2) + ' MB/s') : (speedKB.toFixed(0) + ' KB/s');

                        let pct = totalBytes > 0 ? Math.min(100, (receivedBytes / totalBytes) * 100) : 0;
                        let etaStr = '--';
                        if (totalBytes > 0 && speedBps > 0) {
                            const remainingSec = Math.max(0, (totalBytes - receivedBytes) / speedBps);
                            etaStr = remainingSec >= 60 ? (Math.floor(remainingSec / 60) + 'm ' + Math.floor(remainingSec % 60) + 's') : (remainingSec.toFixed(0) + 's');
                        }

                        if (el.dlFilePct) el.dlFilePct.textContent = pct.toFixed(0) + '%';
                        if (el.dlFileBar) el.dlFileBar.style.width = pct.toFixed(1) + '%';
                        if (el.dlSpeed) el.dlSpeed.textContent = speedStr;
                        if (el.dlEta) el.dlEta.textContent = 'ETA: ' + etaStr;
                        if (el.dlSize) el.dlSize.textContent = (receivedBytes / (1024*1024)).toFixed(1) + (totalBytes > 0 ? (' / ' + (totalBytes / (1024*1024)).toFixed(1) + ' MB') : ' MB');
                    }
                }

                if (el.dlFilePct) el.dlFilePct.textContent = '100%';
                if (el.dlFileBar) el.dlFileBar.style.width = '100%';
                if (el.dlEta) el.dlEta.textContent = 'Concluído!';
                const totalDurationSec = (performance.now() - startTime) / 1000;
                const avgSpeedKB = totalDurationSec > 0 ? (receivedBytes / 1024 / totalDurationSec) : 0;
                if (el.dlSpeed) el.dlSpeed.textContent = avgSpeedKB >= 1024 ? (avgSpeedKB / 1024).toFixed(2) + ' MB/s méd' : avgSpeedKB.toFixed(0) + ' KB/s méd';

                const blob = new Blob(chunks);
                chunks = [];
                const downloadUrl = URL.createObjectURL(blob);
                const a = document.createElement('a');
                a.href = downloadUrl;
                a.download = name;
                document.body.appendChild(a);
                a.click();
                document.body.removeChild(a);
                setTimeout(() => URL.revokeObjectURL(downloadUrl), 10000);

                setTimeout(() => {
                    if (el.dlBox) el.dlBox.style.display = 'none';
                }, 2500);
            } catch (e) {
                alert('Erro ao baixar arquivo: ' + (e.message || e));
                if (el.dlBox) el.dlBox.style.display = 'none';
            } finally {
                isDownloading = false;
            }
        }

        document.getElementById('ctxDownload').onclick = ()=>{
            if(!currentCtxItem || currentCtxItem.isDir) return;
            downloadFileWithProgress(currentCtxItem);
        };

        document.getElementById('ctxRename').onclick = async ()=>{
            if(!currentCtxItem) return;
            let newName = prompt('Novo nome para ' + currentCtxItem.name + ':', currentCtxItem.name);
            if(newName && newName.trim() && newName.trim() !== currentCtxItem.name) {
                newName = newName.trim();
                let newPath = joinPath(curDir, newName);
                let res = await fetch('/api/rename', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    body: JSON.stringify({ old_path: currentCtxItem.path, new_path: newPath })
                });
                if(res.ok) { dirCache = {}; load(); } else { alert('Erro ao renomear'); }
            }
        };

        
        // --- Navegação e Seleção de Pastas para Mover/Copiar Arquivos (Individual ou em Lote) ---
        let moveTargetItem = null;
        let moveBatchItems = [];
        let moveMode = 'move'; // 'move' ou 'copy'
        let moveCurrentDir = '/';

        async function openMoveModal(item, mode = 'move', batch = []) {
            moveMode = mode;
            moveBatchItems = batch || [];
            moveTargetItem = item;
            moveCurrentDir = curDir || '/';

            const nameEl = document.getElementById('moveItemName');
            const confirmBtn = document.getElementById('btnConfirmMove');

            if (moveBatchItems.length > 0) {
                nameEl.textContent = `${moveBatchItems.length} itens selecionados`;
                confirmBtn.textContent = mode === 'move' ? `✓ Mover ${moveBatchItems.length} itens para cá` : `✓ Copiar ${moveBatchItems.length} itens para cá`;
            } else if (item) {
                nameEl.textContent = item.name;
                confirmBtn.textContent = mode === 'move' ? '✓ Mover para esta pasta' : '✓ Copiar para esta pasta';
            }
            document.getElementById('moveModal').style.display = 'flex';
            await renderMoveFolderList();
        }

        function closeMoveModal() {
            document.getElementById('moveModal').style.display = 'none';
            moveTargetItem = null;
            moveBatchItems = [];
        }

        async function renderMoveFolderList() {
            const pathSpan = document.getElementById('moveCurrentPath');
            pathSpan.textContent = moveCurrentDir;
            const list = document.getElementById('moveFolderList');
            list.innerHTML = '<div style="padding:14px;color:#888;text-align:center;font-size:0.9em;">Carregando pastas...</div>';
            
            try {
                const res = await fetch('/api/list?path=' + encodeURIComponent(moveCurrentDir));
                if(!res.ok) throw new Error();
                const data = await res.json();
                list.innerHTML = '';
                
                // Se não estiver na raiz, adiciona item para subir pasta "📁 .."
                if (moveCurrentDir !== '/' && moveCurrentDir !== '') {
                    const upDiv = document.createElement('div');
                    upDiv.className = 'folder-picker-item up-dir';
                    upDiv.innerHTML = '<span style="font-size:1.15em;">📁</span> <strong>.. (Subir uma pasta)</strong>';
                    upDiv.onclick = async () => {
                        let parts = moveCurrentDir.replace(/\/$/, '').split('/');
                        parts.pop();
                        moveCurrentDir = parts.join('/') || '/';
                        await renderMoveFolderList();
                    };
                    list.appendChild(upDiv);
                }

                // Apenas pastas (exclui a própria pasta selecionada caso seja uma pasta individual)
                const dirs = (data.entries || []).filter(f => {
                    if (!f.dir) return false;
                    if (moveTargetItem && moveTargetItem.isDir && f.name === moveTargetItem.name) return false;
                    return true;
                });
                
                if (dirs.length === 0) {
                    const emptyDiv = document.createElement('div');
                    emptyDiv.style.padding = '12px 14px';
                    emptyDiv.style.color = '#888';
                    emptyDiv.style.fontSize = '0.9em';
                    emptyDiv.textContent = 'Nenhuma subpasta aqui.';
                    list.appendChild(emptyDiv);
                } else {
                    dirs.forEach(d => {
                        const itemDiv = document.createElement('div');
                        itemDiv.className = 'folder-picker-item';
                        itemDiv.innerHTML = `<span style="font-size:1.15em;">📁</span> <span>${d.name}</span>`;
                        itemDiv.onclick = async () => {
                            moveCurrentDir = joinPath(moveCurrentDir, d.name);
                            await renderMoveFolderList();
                        };
                        list.appendChild(itemDiv);
                    });
                }
            } catch(e) {
                list.innerHTML = '<div style="padding:14px;color:#c33;text-align:center;font-size:0.9em;">Erro ao carregar pastas.</div>';
            }
        }

        document.getElementById('btnMoveNewFolder').onclick = async () => {
            let name = prompt('Nome da nova pasta em ' + moveCurrentDir + ':');
            if (!name || !name.trim()) return;
            name = name.trim();
            let newDirPath = joinPath(moveCurrentDir, name);
            try {
                let res = await fetch('/api/mkdir', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    body: JSON.stringify({ path: newDirPath })
                });
                if (res.ok) {
                    dirCache = {};
                    await renderMoveFolderList();
                } else {
                    alert('Erro ao criar pasta');
                }
            } catch(e) {
                alert('Falha na requisição');
            }
        };

        document.getElementById('btnConfirmMove').onclick = async () => {
            const confirmBtn = document.getElementById('btnConfirmMove');
            confirmBtn.disabled = true;

            try {
                // Modo Operação em Lote
                if (moveBatchItems && moveBatchItems.length > 0) {
                    let total = moveBatchItems.length;
                    for (let i = 0; i < total; i++) {
                        let item = moveBatchItems[i];
                        confirmBtn.textContent = `Processando (${i + 1}/${total})...`;

                        if (item.isDir && (moveCurrentDir === item.path || moveCurrentDir.startsWith(item.path + '/'))) {
                            continue; // Não mover pasta para dentro dela mesma
                        }
                        let newPath = joinPath(moveCurrentDir, item.name);
                        if (newPath === item.path) continue;

                        try {
                            let url = moveMode === 'move' ? '/api/move' : '/api/copy';
                            let body = moveMode === 'move' 
                                ? JSON.stringify({ old_path: item.path, new_path: newPath })
                                : JSON.stringify({ src_path: item.path, dest_path: newPath });
                            await fetch(url, {
                                method: 'POST',
                                headers: {'Content-Type': 'application/json'},
                                body: body
                            });
                        } catch(e) {}
                    }
                    selectedItems.clear();
                    dirCache = {};
                    closeMoveModal();
                    updateBatchBar();
                    load();
                    return;
                }

                // Modo Item Único
                if (!moveTargetItem) return;
                if (moveTargetItem.isDir &&
                    (moveCurrentDir === moveTargetItem.path || moveCurrentDir.startsWith(moveTargetItem.path + '/'))) {
                    alert('Não é possível mover uma pasta para dentro dela mesma.');
                    return;
                }
                let newPath = joinPath(moveCurrentDir, moveTargetItem.name);
                if (newPath === moveTargetItem.path) {
                    alert('O item já está nesta pasta.');
                    return;
                }
                let url = moveMode === 'move' ? '/api/move' : '/api/copy';
                let body = moveMode === 'move' 
                    ? JSON.stringify({ old_path: moveTargetItem.path, new_path: newPath })
                    : JSON.stringify({ src_path: moveTargetItem.path, dest_path: newPath });
                let res = await fetch(url, {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    body: body
                });
                if (res.ok) {
                    dirCache = {};
                    closeMoveModal();
                    load();
                } else {
                    let errData = {};
                    try { errData = await res.json(); } catch(e) {}
                    alert(errData.error || `Erro ao ${moveMode === 'move' ? 'mover' : 'copiar'}`);
                }
            } catch(e) {
                alert('Falha na requisição');
            } finally {
                confirmBtn.disabled = false;
            }
        };

        document.getElementById('ctxMove').onclick = () => {
            if(!currentCtxItem) return;
            document.getElementById('ctxMenu').classList.remove('active');
            openMoveModal(currentCtxItem, 'move');
        };

        // --- HANDLERS DA BARRA DE AÇÕES EM LOTE ---
        document.getElementById('chkSelectAll').onchange = (ev) => {
            let filter = el.filter.value.toLowerCase().trim();
            let filtered = (currentEntries || []).filter(e => e.name.toLowerCase().includes(filter));
            if (ev.target.checked) {
                filtered.forEach(e => {
                    let itemPath = joinPath(curDir, e.name);
                    selectedItems.set(itemPath, { path: itemPath, name: e.name, isDir: e.dir });
                });
            } else {
                filtered.forEach(e => {
                    let itemPath = joinPath(curDir, e.name);
                    selectedItems.delete(itemPath);
                });
            }
            renderTable(currentEntries);
        };

        document.getElementById('btnBatchClear').onclick = () => {
            selectedItems.clear();
            renderTable(currentEntries);
        };

        document.getElementById('btnBatchDelete').onclick = async () => {
            if (selectedItems.size === 0) return;
            const sz = selectedItems.size;
            if (!confirm(`Deseja realmente excluir permanentemente os ${sz} itens selecionados?`)) return;

            const items = Array.from(selectedItems.values());
            for (let item of items) {
                try {
                    await fetch('/api/delete?path=' + encodeURIComponent(item.path), { method: 'DELETE' });
                } catch(e) {}
            }
            selectedItems.clear();
            dirCache = {};
            updateBatchBar();
            load();
        };

        document.getElementById('btnBatchDownload').onclick = async () => {
            const files = Array.from(selectedItems.values()).filter(x => !x.isDir);
            if (!files.length) {
                alert('Nenhum arquivo para download selecionado (pastas não podem ser baixadas diretamente).');
                return;
            }
            for (let i = 0; i < files.length; i++) {
                await downloadFileWithProgress(files[i]);
                if (i < files.length - 1) {
                    await new Promise(r => setTimeout(r, 400));
                }
            }
        };

        document.getElementById('btnBatchMove').onclick = () => {
            if (selectedItems.size === 0) return;
            openMoveModal(null, 'move', Array.from(selectedItems.values()));
        };

        document.getElementById('btnBatchCopy').onclick = () => {
            if (selectedItems.size === 0) return;
            openMoveModal(null, 'copy', Array.from(selectedItems.values()));
        };

        document.getElementById('ctxDel').onclick = async ()=>{
            if(!currentCtxItem) return;
            let msg = currentCtxItem.isDir ? 
                'Tem certeza que deseja excluir a pasta "' + currentCtxItem.name + '" e todos os seus arquivos recursivamente?' : 
                'Excluir "' + currentCtxItem.name + '"?';
            if(confirm(msg)) {
                let res = await fetch('/api/delete?path='+encodeURIComponent(currentCtxItem.path), {method:'DELETE'});
                if(res.ok) { dirCache = {}; load(); } else { alert('Erro ao excluir'); }
            }
        };

        function openMkdirModal() {
            document.getElementById('mkdirCurrentPathLabel').textContent = curDir;
            const inp = document.getElementById('mkdirInputName');
            inp.value = '';
            document.getElementById('mkdirModal').style.display = 'flex';
            setTimeout(() => inp.focus(), 50);
        }

        function closeMkdirModal() {
            document.getElementById('mkdirModal').style.display = 'none';
        }

        document.getElementById('mkdirBtn').onclick = () => openMkdirModal();

        document.getElementById('btnConfirmMkdir').onclick = async () => {
            const inp = document.getElementById('mkdirInputName');
            let name = inp.value.trim();
            if (!name) {
                alert('Digite um nome para a pasta.');
                inp.focus();
                return;
            }
            if (/[\/\\:*?"<>|]/.test(name)) {
                alert('Nome de pasta contém caracteres inválidos.');
                return;
            }
            let path = joinPath(curDir, name);
            try {
                let res = await fetch('/api/mkdir', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    body: JSON.stringify({ path: path })
                });
                if (res.ok) {
                    closeMkdirModal();
                    dirCache = {};
                    load();
                } else {
                    let errData = {};
                    try { errData = await res.json(); } catch(e) {}
                    alert(errData.error || 'Erro ao criar pasta');
                }
            } catch(e) {
                alert('Falha na requisição ao criar pasta');
            }
        };

        // Permite pressionar ENTER no campo do modal de nova pasta
        document.getElementById('mkdirInputName').onkeydown = (e) => {
            if (e.key === 'Enter') {
                e.preventDefault();
                document.getElementById('btnConfirmMkdir').click();
            } else if (e.key === 'Escape') {
                closeMkdirModal();
            }
        };

        // =====================================================================
        // ESTÚDIO DE PLAYLISTS (.m3u / .m3u8) — CRIAÇÃO, EDIÇÃO E REORDENAÇÃO
        // =====================================================================
        let plStudioTracks = []; // Array de caminhos web relativos à raiz SD, ex: "/Rock/Song.mp3"
        let plBrowserDir = '/';
        let plBrowserEntries = [];

        function normalizeSdWebPath(rawLine, baseDir = '/') {
            let s = (rawLine || '').trim().replace(/\\/g, '/');
            if (!s || s.startsWith('#')) return null;
            if (s.toLowerCase().startsWith('/sdcard/')) {
                s = s.substring(7);
            } else if (s.toLowerCase() === '/sdcard') {
                s = '/';
            } else if (!s.startsWith('/')) {
                s = joinPath(baseDir || '/', s);
            }
            return s;
        }

        function toFirmwareSdPath(webPath) {
            let clean = (webPath || '').trim().replace(/\\/g, '/');
            if (clean.toLowerCase().startsWith('/sdcard/')) return clean;
            if (!clean.startsWith('/')) clean = '/' + clean;
            return '/sdcard' + clean;
        }

        function setPlStatus(msg, isError = false) {
            const st = document.getElementById('plStatusMsg');
            if (!st) return;
            st.textContent = msg;
            st.style.color = isError ? '#ef4444' : 'var(--text-muted)';
        }

        async function refreshExistingPlaylistsDropdown(selectPath = null) {
            const sel = document.getElementById('plSelectExisting');
            if (!sel) return;
            let foundPlaylists = [];
            try {
                const r = await fetch('/api/list?path=%2F');
                if (r.ok) {
                    const data = await r.json();
                    (data.entries || []).forEach(e => {
                        if (!e.dir) {
                            const ext = e.name.toLowerCase().split('.').pop();
                            if (ext === 'm3u' || ext === 'm3u8') {
                                foundPlaylists.push('/' + e.name);
                            }
                        }
                    });
                }
                if (curDir && curDir !== '/') {
                    const r2 = await fetch('/api/list?path=' + encodeURIComponent(curDir));
                    if (r2.ok) {
                        const d2 = await r2.json();
                        (d2.entries || []).forEach(e => {
                            if (!e.dir) {
                                const ext = e.name.toLowerCase().split('.').pop();
                                if (ext === 'm3u' || ext === 'm3u8') {
                                    const p = joinPath(curDir, e.name);
                                    if (!foundPlaylists.includes(p)) foundPlaylists.push(p);
                                }
                            }
                        });
                    }
                }
            } catch (e) {}

            foundPlaylists.sort((a, b) => a.localeCompare(b));
            sel.innerHTML = '<option value="__new__">✨ + Criar Nova Playlist...</option>';
            foundPlaylists.forEach(p => {
                const opt = document.createElement('option');
                opt.value = p;
                opt.textContent = '🎶 ' + p.replace(/^\//, '');
                sel.appendChild(opt);
            });

            if (selectPath && foundPlaylists.includes(selectPath)) {
                sel.value = selectPath;
            } else if (!selectPath && foundPlaylists.includes('/Favoritos.m3u8')) {
                sel.value = '/Favoritos.m3u8';
            } else {
                sel.value = '__new__';
            }
        }

        function syncNameInputFromPath(plPath) {
            const inp = document.getElementById('plNameInput');
            const extSel = document.getElementById('plExtSelect');
            if (!inp || !extSel) return;
            if (!plPath || plPath === '__new__') {
                inp.value = 'MinhaLista';
                extSel.value = '.m3u8';
                return;
            }
            const base = plPath.split('/').pop() || 'Favoritos.m3u8';
            const lower = base.toLowerCase();
            if (lower.endsWith('.m3u8')) {
                inp.value = base.slice(0, -5);
                extSel.value = '.m3u8';
            } else if (lower.endsWith('.m3u')) {
                inp.value = base.slice(0, -4);
                extSel.value = '.m3u';
            } else {
                inp.value = base;
                extSel.value = '.m3u8';
            }
        }

        async function loadPlaylistFileFromSd(plPath) {
            plStudioTracks = [];
            if (!plPath || plPath === '__new__') {
                syncNameInputFromPath('__new__');
                renderPlaylistStudioTracks();
                renderPlaylistStudioBrowser();
                return;
            }
            syncNameInputFromPath(plPath);
            setPlStatus('Lendo ' + plPath + '...');
            try {
                const r = await fetch('/api/download?path=' + encodeURIComponent(plPath));
                if (r.ok) {
                    const txt = await r.text();
                    const baseDir = plPath.split('/').slice(0, -1).join('/') || '/';
                    const lines = txt.split(/\r?\n/);
                    lines.forEach(line => {
                        const norm = normalizeSdWebPath(line, baseDir);
                        if (norm && !plStudioTracks.includes(norm)) {
                            plStudioTracks.push(norm);
                        }
                    });
                    setPlStatus(`Carregada "${plPath.replace(/^\//, '')}" (${plStudioTracks.length} faixas).`);
                } else {
                    setPlStatus('Nova lista: ' + plPath.replace(/^\//, ''));
                }
            } catch (e) {
                setPlStatus('Erro ao ler playlist do SD.', true);
            }
            renderPlaylistStudioTracks();
            renderPlaylistStudioBrowser();
        }

        function renderPlaylistStudioTracks() {
            const box = document.getElementById('plTracksList');
            const badge = document.getElementById('plTrackCountBadge');
            if (badge) badge.textContent = plStudioTracks.length;
            if (!box) return;

            if (plStudioTracks.length === 0) {
                box.innerHTML = '<div style="padding:24px 12px;text-align:center;color:var(--text-muted);font-size:0.86em;">Nenhuma faixa na playlist.<br>Clique nas músicas ou pastas à direita para adicionar!</div>';
                return;
            }

            box.innerHTML = '';
            plStudioTracks.forEach((trkPath, idx) => {
                const parts = trkPath.split('/').filter(Boolean);
                const fileName = parts.pop() || trkPath;
                const folderName = parts.length ? ('/' + parts.join('/')) : '/';

                const row = document.createElement('div');
                row.style.cssText = 'display:flex;align-items:center;justify-content:space-between;gap:6px;padding:6px 8px;border-bottom:1px solid var(--border-color);font-size:0.84em;';
                row.innerHTML = `
                    <div style="display:flex;align-items:center;gap:6px;min-width:0;flex:1;">
                        <span style="color:var(--text-muted);font-family:monospace;font-size:0.85em;min-width:22px;">${idx + 1}.</span>
                        <div style="min-width:0;flex:1;">
                            <div style="font-weight:600;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;" title="${trkPath.replace(/"/g, '&quot;')}">${fileName}</div>
                            <div style="font-size:0.76em;color:var(--text-muted);overflow:hidden;text-overflow:ellipsis;white-space:nowrap;">${folderName}</div>
                        </div>
                    </div>
                    <div style="display:flex;gap:3px;flex-shrink:0;">
                        <button type="button" class="btn-secondary" style="padding:2px 6px;font-size:0.8em;" title="Subir" ${idx === 0 ? 'disabled' : ''}>↑</button>
                        <button type="button" class="btn-secondary" style="padding:2px 6px;font-size:0.8em;" title="Descer" ${idx === plStudioTracks.length - 1 ? 'disabled' : ''}>↓</button>
                        <button type="button" class="btn-secondary" style="padding:2px 6px;font-size:0.8em;color:#ef4444;" title="Remover">✕</button>
                    </div>
                `;
                const btns = row.querySelectorAll('button');
                btns[0].onclick = () => {
                    if (idx > 0) {
                        const tmp = plStudioTracks[idx - 1];
                        plStudioTracks[idx - 1] = plStudioTracks[idx];
                        plStudioTracks[idx] = tmp;
                        renderPlaylistStudioTracks();
                    }
                };
                btns[1].onclick = () => {
                    if (idx < plStudioTracks.length - 1) {
                        const tmp = plStudioTracks[idx + 1];
                        plStudioTracks[idx + 1] = plStudioTracks[idx];
                        plStudioTracks[idx] = tmp;
                        renderPlaylistStudioTracks();
                    }
                };
                btns[2].onclick = () => {
                    plStudioTracks.splice(idx, 1);
                    renderPlaylistStudioTracks();
                    renderPlaylistStudioBrowser();
                };
                box.appendChild(row);
            });
        }

        async function loadPlaylistBrowserDir(dirPath) {
            plBrowserDir = dirPath || '/';
            const pathEl = document.getElementById('plBrowserPath');
            if (pathEl) pathEl.textContent = plBrowserDir;
            const box = document.getElementById('plBrowserList');
            if (box) box.innerHTML = '<div style="padding:16px;text-align:center;color:var(--text-muted);font-size:0.85em;">Carregando...</div>';
            try {
                const r = await fetch('/api/list?path=' + encodeURIComponent(plBrowserDir));
                if (r.ok) {
                    const data = await r.json();
                    plBrowserEntries = (data.entries || []).slice().sort((a, b) => a.dir !== b.dir ? (a.dir ? -1 : 1) : a.name.localeCompare(b.name));
                } else {
                    plBrowserEntries = [];
                }
            } catch (e) {
                plBrowserEntries = [];
            }
            renderPlaylistStudioBrowser();
        }

        function renderPlaylistStudioBrowser() {
            const box = document.getElementById('plBrowserList');
            if (!box) return;
            box.innerHTML = '';

            if (plBrowserDir !== '/' && plBrowserDir !== '') {
                const upRow = document.createElement('div');
                upRow.style.cssText = 'display:flex;align-items:center;gap:6px;padding:6px 8px;border-bottom:1px solid var(--border-color);cursor:pointer;font-size:0.85em;font-weight:bold;';
                upRow.innerHTML = '<span>📁 .. (Subir pasta)</span>';
                upRow.onclick = () => {
                    const parent = plBrowserDir.replace(/\/$/, '').split('/').slice(0, -1).join('/') || '/';
                    loadPlaylistBrowserDir(parent);
                };
                box.appendChild(upRow);
            }

            const audioExts = ['mp3','flac','wav','m4a','m4b','ogg','opus','aac'];
            const visible = (plBrowserEntries || []).filter(e => {
                if (e.dir) return true;
                const ext = e.name.toLowerCase().split('.').pop();
                return audioExts.includes(ext);
            });

            if (visible.length === 0) {
                const empty = document.createElement('div');
                empty.style.cssText = 'padding:18px;text-align:center;color:var(--text-muted);font-size:0.85em;';
                empty.textContent = 'Nenhuma pasta ou áudio aqui.';
                box.appendChild(empty);
                return;
            }

            visible.forEach(e => {
                const itemPath = joinPath(plBrowserDir, e.name);
                const row = document.createElement('div');
                row.style.cssText = 'display:flex;align-items:center;justify-content:space-between;gap:6px;padding:6px 8px;border-bottom:1px solid var(--border-color);font-size:0.84em;';

                if (e.dir) {
                    row.innerHTML = `
                        <span style="cursor:pointer;font-weight:600;color:var(--accent-color);overflow:hidden;text-overflow:ellipsis;white-space:nowrap;flex:1;" title="Abrir pasta">📁 ${e.name}</span>
                        <button type="button" class="btn-secondary" style="padding:2px 7px;font-size:0.76em;flex-shrink:0;">+ Pasta</button>
                    `;
                    row.querySelector('span').onclick = () => loadPlaylistBrowserDir(itemPath);
                    row.querySelector('button').onclick = async () => {
                        await addFolderTracksToPlaylistStudio(itemPath);
                    };
                } else {
                    const inList = plStudioTracks.includes(itemPath);
                    row.innerHTML = `
                        <span style="overflow:hidden;text-overflow:ellipsis;white-space:nowrap;flex:1;" title="${e.name.replace(/"/g, '&quot;')}">🎵 ${e.name}</span>
                        <button type="button" class="${inList ? 'btn-secondary' : 'btn-primary'}" style="padding:2px 8px;font-size:0.76em;flex-shrink:0;">${inList ? '✓ Na Lista' : '+ Adic.'}</button>
                    `;
                    row.querySelector('button').onclick = () => {
                        const idx = plStudioTracks.indexOf(itemPath);
                        if (idx >= 0) {
                            plStudioTracks.splice(idx, 1);
                        } else {
                            plStudioTracks.push(itemPath);
                        }
                        renderPlaylistStudioTracks();
                        renderPlaylistStudioBrowser();
                    };
                }
                box.appendChild(row);
            });
        }

        async function addFolderTracksToPlaylistStudio(folderPath) {
            try {
                const r = await fetch('/api/list?path=' + encodeURIComponent(folderPath));
                if (!r.ok) return 0;
                const data = await r.json();
                const audioExts = ['mp3','flac','wav','m4a','m4b','ogg','opus','aac'];
                const files = (data.entries || [])
                    .filter(e => !e.dir && audioExts.includes(e.name.toLowerCase().split('.').pop()))
                    .sort((a, b) => a.name.localeCompare(b.name));
                let added = 0;
                files.forEach(f => {
                    const p = joinPath(folderPath, f.name);
                    if (!plStudioTracks.includes(p)) {
                        plStudioTracks.push(p);
                        added++;
                    }
                });
                renderPlaylistStudioTracks();
                renderPlaylistStudioBrowser();
                setPlStatus(`+ ${added} faixas adicionadas de "${folderPath}".`);
                return added;
            } catch (e) {
                return 0;
            }
        }

        async function addAllCurrentBrowserFolderToPlaylist() {
            await addFolderTracksToPlaylistStudio(plBrowserDir);
        }

        function sortPlaylistStudioTracks() {
            plStudioTracks.sort((a, b) => {
                const na = a.split('/').pop() || a;
                const nb = b.split('/').pop() || b;
                return na.localeCompare(nb);
            });
            renderPlaylistStudioTracks();
        }

        function shufflePlaylistStudioTracks() {
            for (let i = plStudioTracks.length - 1; i > 0; i--) {
                const j = Math.floor(Math.random() * (i + 1));
                const tmp = plStudioTracks[i];
                plStudioTracks[i] = plStudioTracks[j];
                plStudioTracks[j] = tmp;
            }
            renderPlaylistStudioTracks();
        }

        function clearPlaylistStudioTracks() {
            if (plStudioTracks.length > 0 && !confirm('Remover todas as faixas desta playlist?')) return;
            plStudioTracks = [];
            renderPlaylistStudioTracks();
            renderPlaylistStudioBrowser();
        }

        function startNewPlaylistInStudio() {
            const sel = document.getElementById('plSelectExisting');
            if (sel) sel.value = '__new__';
            plStudioTracks = [];
            syncNameInputFromPath('__new__');
            renderPlaylistStudioTracks();
            renderPlaylistStudioBrowser();
            const inp = document.getElementById('plNameInput');
            if (inp) {
                inp.focus();
                inp.select();
            }
            setPlStatus('Nova playlist iniciada — escolha o nome e adicione faixas.');
        }

        async function savePlaylistToSd() {
            const inp = document.getElementById('plNameInput');
            const extSel = document.getElementById('plExtSelect');
            const btn = document.getElementById('btnSavePlaylist');
            if (!inp || !extSel) return;

            let rawName = inp.value.trim().replace(/[\/\\:*?"<>|]/g, '');
            if (!rawName) {
                alert('Digite um nome para a playlist.');
                inp.focus();
                return;
            }
            const ext = extSel.value || '.m3u8';
            if (rawName.toLowerCase().endsWith('.m3u8')) rawName = rawName.slice(0, -5);
            else if (rawName.toLowerCase().endsWith('.m3u')) rawName = rawName.slice(0, -4);

            const finalFileName = rawName + ext;
            const targetSdWebPath = '/' + finalFileName;

            let content = '#EXTM3U\n';
            plStudioTracks.forEach(trk => {
                content += toFirmwareSdPath(trk) + '\n';
            });

            const blob = new Blob([content], { type: 'audio/x-mpegurl;charset=utf-8' });
            if (btn) btn.disabled = true;
            setPlStatus('Salvando ' + finalFileName + ' no cartão SD...');

            try {
                const url = '/api/upload?path=' + encodeURIComponent(targetSdWebPath) +
                            '&idx=1&count=1&batchTotal=' + blob.size;
                const res = await fetch(url, { method: 'PUT', body: blob });
                if (res.ok) {
                    setPlStatus(`✓ Playlist "${finalFileName}" salva com sucesso (${plStudioTracks.length} faixas)!`);
                    dirCache = {};
                    await refreshExistingPlaylistsDropdown(targetSdWebPath);
                    load();
                } else {
                    setPlStatus('Erro ao salvar playlist no cartão SD.', true);
                }
            } catch (e) {
                setPlStatus('Falha de conexão ao salvar playlist.', true);
            } finally {
                if (btn) btn.disabled = false;
            }
        }

        function playCurrentM3uInWebPlayer() {
            if (!plStudioTracks.length) {
                alert('Esta playlist ainda não possui faixas para tocar.');
                return;
            }
            playlist = plStudioTracks.map(p => ({
                name: p.split('/').pop() || p,
                path: p
            }));
            playIndex = 0;
            loadTrack();
            closePlaylistModal();
        }

        async function deleteCurrentM3uFromSd() {
            const sel = document.getElementById('plSelectExisting');
            if (!sel || !sel.value || sel.value === '__new__') {
                alert('Selecione uma playlist já salva no cartão SD para excluir.');
                return;
            }
            const target = sel.value;
            if (!confirm(`Excluir permanentemente a playlist "${target.replace(/^\//, '')}" do cartão SD? (As músicas originais não serão apagadas)`)) {
                return;
            }
            try {
                const r = await fetch('/api/delete?path=' + encodeURIComponent(target), { method: 'DELETE' });
                if (r.ok) {
                    dirCache = {};
                    await refreshExistingPlaylistsDropdown('__new__');
                    startNewPlaylistInStudio();
                    load();
                    setPlStatus('Playlist excluída.');
                } else {
                    setPlStatus('Erro ao excluir playlist.', true);
                }
            } catch (e) {
                setPlStatus('Falha ao excluir playlist.', true);
            }
        }

        async function openPlaylistModal(initialPlaylistPath = null, itemsToAppend = []) {
            document.getElementById('playlistModal').style.display = 'flex';
            await refreshExistingPlaylistsDropdown(initialPlaylistPath);
            const sel = document.getElementById('plSelectExisting');
            const chosen = initialPlaylistPath || (sel ? sel.value : '__new__');
            await loadPlaylistFileFromSd(chosen);
            await loadPlaylistBrowserDir(curDir || '/');

            if (itemsToAppend && itemsToAppend.length > 0) {
                let addedCount = 0;
                for (const item of itemsToAppend) {
                    if (item.isDir) {
                        addedCount += await addFolderTracksToPlaylistStudio(item.path);
                    } else if (item.isAudio) {
                        if (!plStudioTracks.includes(item.path)) {
                            plStudioTracks.push(item.path);
                            addedCount++;
                        }
                    }
                }
                renderPlaylistStudioTracks();
                renderPlaylistStudioBrowser();
                setPlStatus(`+ ${addedCount} faixa(s) adicionada(s) — clique em "💾 Salvar Playlist no SD" para gravar!`);
            }
        }

        function closePlaylistModal() {
            document.getElementById('playlistModal').style.display = 'none';
        }

        document.getElementById('plSelectExisting').onchange = async (ev) => {
            await loadPlaylistFileFromSd(ev.target.value);
        };

        document.getElementById('playlistBtn').onclick = () => openPlaylistModal();

        document.getElementById('btnBatchPlaylist').onclick = () => {
            if (selectedItems.size === 0) return;
            const items = Array.from(selectedItems.values());
            openPlaylistModal(null, items);
        };

        document.getElementById('ctxPlaylist').onclick = () => {
            if (!currentCtxItem) return;
            document.getElementById('ctxMenu').classList.remove('active');
            if (currentCtxItem.isPlaylist) {
                openPlaylistModal(currentCtxItem.path, []);
            } else {
                openPlaylistModal(null, [currentCtxItem]);
            }
        };
