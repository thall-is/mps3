/* ==========================================================================
 * MÓDULO JS 03: Polling de Telemetria (/api/status), Tabela SD e Ações em Lote
 * ========================================================================== */
        // --- SYSTEM STATUS & DIRECTORY RENDER ---
        let isSdTransferring = false;
        async function fetchStatus(){
            if (isUploading || isPodSyncing || otaXhr) {
                updateSdTraffic('writing');
                return;
            }
            try{
                let r=await fetch('/api/status'), j=await r.json();
                el.sdInfoText.textContent = 'SD: '+fmtSize(j.total-j.free)+' / '+fmtSize(j.total);
                if (j.total > 0) el.sdBar.style.width = ((j.total-j.free)/j.total*100) + '%';
                el.batInfo.innerHTML = '🔋 ' + j.battery_percent + '% <span style="color:#888;font-size:0.85em;font-weight:normal;">(' + (j.battery_mv/1000).toFixed(2) + 'V)</span>' + (j.battery_time_left>0?' <span style="font-size:0.8em;color:#666;font-weight:normal">~'+Math.floor(j.battery_time_left/60)+'h '+j.battery_time_left%60+'m</span>':'');
                
                // Atualiza o Semáforo do Cartão SD
                updateSdTraffic(j.sd_status || 'free');
            }catch(e){}
        }

        function updateSdTraffic(status) {
            const dot = document.getElementById('sdDot');
            const txt = document.getElementById('sdTrafficText');
            if (!dot || !txt) return;
            
            // Prioridade: se o player estiver tocando localmente no navegador, indica leitura suave
            const isAudioPlaying = el.audio && !el.audio.paused;
            
            if (status === 'writing' || isUploading) {
                dot.className = 'sd-dot busy-write';
                txt.textContent = 'Gravando (Ocupado)';
                txt.style.color = '#ef4444';
            } else if (isAudioPlaying || status === 'reading') {
                dot.className = 'sd-dot busy-read';
                txt.textContent = 'Tocando (Leitura)';
                txt.style.color = '#f59e0b';
            } else {
                dot.className = 'sd-dot';
                txt.textContent = 'Livre';
                txt.style.color = '#10b981';
            }
        }

        function updateBatchBar() {
            const bar = document.getElementById('batchBar');
            const countTxt = document.getElementById('batchCountText');
            const chkAll = document.getElementById('chkSelectAll');
            if (!bar || !countTxt) return;

            const sz = selectedItems.size;
            if (sz > 0) {
                bar.classList.add('active');
                countTxt.textContent = sz + (sz === 1 ? ' item selecionado' : ' itens selecionados');
            } else {
                bar.classList.remove('active');
            }

            if (chkAll) {
                let visibleCheckboxes = el.tbody.querySelectorAll('.item-chk');
                if (visibleCheckboxes.length === 0) {
                    chkAll.checked = false;
                    chkAll.indeterminate = false;
                } else {
                    let checkedCount = 0;
                    visibleCheckboxes.forEach(c => { if (c.checked) checkedCount++; });
                    if (checkedCount === 0) {
                        chkAll.checked = false;
                        chkAll.indeterminate = false;
                    } else if (checkedCount === visibleCheckboxes.length) {
                        chkAll.checked = true;
                        chkAll.indeterminate = false;
                    } else {
                        chkAll.checked = false;
                        chkAll.indeterminate = true;
                    }
                }
            }
        }

        function renderTable(entries) {
            let filter = el.filter.value.toLowerCase().trim();
            el.tbody.innerHTML='';
            if(curDir!=='/'){
                let tr=document.createElement('tr'); 
                tr.innerHTML='<td class="col-check"></td><td class="name dir"><span>📁 ..</span></td>';
                tr.querySelector('span').onclick=()=>{ curDir=curDir.split('/').slice(0,-1).join('/')||'/'; load(); };
                el.tbody.appendChild(tr);
            }
            let filtered = entries.filter(e => e.name.toLowerCase().includes(filter));
            if (!filtered.length) {
                el.tbody.innerHTML += '<tr><td class="col-check"></td><td style="text-align:center; color:#888; padding:1.5em;">Nenhum arquivo encontrado</td></tr>';
                updateBatchBar();
                return;
            }
            filtered.forEach(e=>{
                let tr=document.createElement('tr');
                let ext = e.name.toLowerCase().split('.').pop();
                let isAudio = !e.dir && ['mp3','flac','wav','m4a','m4b','ogg','opus','aac'].includes(ext);
                let isPlaylist = !e.dir && ['m3u','m3u8'].includes(ext);
                let itemPath = joinPath(curDir, e.name);
                let isChecked = selectedItems.has(itemPath);
                let icon = e.dir ? '📁 ' : (isPlaylist ? '🎶 ' : '🎵 ');
                
                tr.innerHTML = `
                    <td class="col-check">
                        <input type="checkbox" class="item-chk" ${isChecked ? 'checked' : ''} title="Selecionar item">
                    </td>
                    <td class="name ${e.dir?'dir':'file'}">
                        <span>${icon}${e.name}</span>
                        <div class="actions">⋮</div>
                    </td>
                `;
                
                let chk = tr.querySelector('.item-chk');
                chk.onchange = (ev)=>{
                    ev.stopPropagation();
                    if(chk.checked) {
                        selectedItems.set(itemPath, { path: itemPath, name: e.name, isDir: e.dir, isAudio: isAudio, isPlaylist: isPlaylist });
                    } else {
                        selectedItems.delete(itemPath);
                    }
                    updateBatchBar();
                };

                let span = tr.querySelector('span');
                if(e.dir) span.onclick = ()=>{ curDir=itemPath; el.filter.value=''; load(); };
                else if(isPlaylist) span.onclick = ()=>openPlaylistModal(itemPath);
                else if(isAudio) span.onclick = ()=>playFromFolder(e.name);
                
                let act = tr.querySelector('.actions');
                if(act) {
                    act.onclick = (ev)=>{
                        ev.stopPropagation();
                        currentCtxItem = { name: e.name, path: itemPath, isAudio: isAudio, isDir: e.dir, isPlaylist: isPlaylist };
                        document.getElementById('ctxNext').style.display = isAudio ? 'block' : 'none';
                        document.getElementById('ctxQueue').style.display = isAudio ? 'block' : 'none';
                        const ctxPl = document.getElementById('ctxPlaylist');
                        if (ctxPl) {
                            ctxPl.style.display = (isAudio || e.dir || isPlaylist) ? 'block' : 'none';
                            ctxPl.textContent = isPlaylist ? '🎶 Editar Playlist (.m3u8)...' : '🎶 Adicionar à Playlist...';
                        }
                        document.getElementById('ctxDownload').style.display = e.dir ? 'none' : 'block';
                        el.ctx.style.left = Math.min(ev.pageX - 120, window.innerWidth - 160) + 'px';
                        el.ctx.style.top = ev.pageY + 'px';
                        el.ctx.classList.add('active');
                    };
                }
                el.tbody.appendChild(tr);
            });
            updateBatchBar();
        }

        el.filter.oninput = () => renderTable(currentEntries);

        async function load(){
            let parts=curDir.split('/').filter(x=>x), html="<a data-p='/'>raiz</a>", acc='';
            for(let p of parts){ acc=joinPath(acc,p); html+=' / <a data-p="'+acc.replace(/"/g,'&quot;')+'">'+p+'</a>'; }
            el.crumbs.innerHTML=html;
            el.crumbs.querySelectorAll('a').forEach(a=>a.onclick=()=>{curDir=a.dataset.p; el.filter.value=''; load();});
            
            if (dirCache[curDir]) {
                currentEntries = dirCache[curDir];
                renderTable(currentEntries);
            } else {
                el.tbody.innerHTML = '<tr><td colspan="2" style="text-align:center; padding: 2em; color: #888;"><div style="display:inline-block; animation: spin 1s linear infinite;">⏳</div> Carregando pasta...</td></tr>';
            }
            
            try{
                let r=await fetch('/api/list?path='+encodeURIComponent(curDir));
                let files=(await r.json()).entries || [];
                files.sort((a,b)=>a.dir!==b.dir ? (a.dir?-1:1) : a.name.localeCompare(b.name));
                dirCache[curDir] = files;
                currentEntries = files;
                renderTable(currentEntries);
            }catch(e){ 
                if (!dirCache[curDir]) el.tbody.innerHTML='<tr><td colspan=2>Erro ao ler pasta</td></tr>'; 
            }
        }

        document.onclick = ()=>el.ctx.classList.remove('active');
