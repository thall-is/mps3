

        
        
        function showLoginModal() {
            document.getElementById('loginModal').style.display = 'flex';
            document.getElementById('djPass').value = '';
            document.getElementById('djPass').focus();
        }
        function hideLoginModal() {
            document.getElementById('loginModal').style.display = 'none';
        }
        function submitLogin() {
            let pwd = document.getElementById('djPass').value;
            if (pwd === "1234") { // Temporary hardcoded password for Phase 1
                localStorage.setItem('mps3_admin', 'true');
                isAdmin = true;
                hideLoginModal();
                applyRoles();
            } else {
                alert("Senha incorreta");
            }
        }
        
        let radioAudio = new Audio();
        
        const el={
            tbody:document.getElementById('tbody'), crumbs:document.getElementById('crumbs'), filter:document.getElementById('filterInput'),
            batInfo:document.getElementById('batInfo'), sdInfoText:document.getElementById('sdInfoText'), sdBar:document.getElementById('sdBar'),
            playerBar:document.getElementById('player-bar'), audio:document.getElementById('audioEl'),
            art:document.getElementById('albumArt'), title:document.getElementById('trackTitle'), artist:document.getElementById('trackArtist'),
            playBtn:document.getElementById('playBtn'), cur:document.getElementById('timeCur'), tot:document.getElementById('timeTot'),
            prog:document.getElementById('progressFill'),
            
            fs:document.getElementById('fs-player'), fsBg:document.getElementById('fs-bg'),
            fsArt:document.getElementById('fsArt'), fsTitle:document.getElementById('fsTitle'), fsArtist:document.getElementById('fsArtist'),
            fsCur:document.getElementById('fsTimeCur'), fsTot:document.getElementById('fsTimeTot'),
            fsProg:document.getElementById('fsProgressFill'), fsPlay:document.getElementById('fsPlayBtn'),
            btnShuf:document.getElementById('btnShuffle'), btnRep:document.getElementById('btnRepeat'),
            canvas:document.getElementById('visualizer'),
            eqBass:document.getElementById('eqBass'), eqMid:document.getElementById('eqMid'), eqTreble:document.getElementById('eqTreble'),
            
            dropzone:document.getElementById('dropzone'), inpFiles:document.getElementById('inpFiles'), inpFolder:document.getElementById('inpFolder'),
            upBox:document.getElementById('upload-status-box'), upFileName:document.getElementById('upFileName'), upFilePct:document.getElementById('upFilePct'),
            upFileBar:document.getElementById('upFileBar'), upOverallLabel:document.getElementById('upOverallLabel'), upOverallPct:document.getElementById('upOverallPct'),
            upOverallBar:document.getElementById('upOverallBar'), upSpeed:document.getElementById('upSpeed'), upEta:document.getElementById('upEta'),
            queue:document.getElementById('queue'),
            
            ctx:document.getElementById('ctxMenu')
        };
        
        let curDir='/';
        let dirCache = {};
        let originalPlaylist = [];
        let playlist = [];
        let playIndex = -1;
        let isShuffle = false;
        let repeatMode = 0; // 0=no, 1=all, 2=one
        let currentCtxItem = null;
        let isUploading = false;
        let queueItems = [];
        let queueId = 0;
        let currentEntries = [];

        const defaultCover = "data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'><rect width='100' height='100' fill='%23333'/><text x='50' y='55' font-family='sans-serif' font-size='40' text-anchor='middle' fill='%23777'>🎵</text></svg>";

        function fmtSize(b){ return b<1024?b+' B':b<1048576?(b/1024).toFixed(1)+' KB':(b/1048576).toFixed(1)+' MB'; }
        function fmtTime(s) { if(isNaN(s)) return '0:00'; let m=Math.floor(s/60); let ss=Math.floor(s%60); return m+':'+(ss<10?'0':'')+ss; }
        function fmtEta(s){if(s<0||!isFinite(s))return '';if(s>=60)return '~'+Math.floor(s/60)+'m'+String(Math.round(s%60)).padStart(2,'0')+'s';return '~'+Math.round(s)+'s';}
        function joinPath(a,b){ return a==='/' ? '/'+b : a+'/'+b; }

        // --- 1. INDEXEDDB CACHE (OFFLOAD ESP32) ---
        let dbPromise = new Promise((resolve) => {
            let req = indexedDB.open('mps3_meta_cache', 1);
            req.onupgradeneeded = (e) => { e.target.result.createObjectStore('metadata', { keyPath: 'path' }); };
            req.onsuccess = (e) => resolve(e.target.result);
            req.onerror = () => resolve(null);
        });

        async function getCachedMetadata(path) {
            let db = await dbPromise;
            if (!db) return null;
            return new Promise((res) => {
                let tx = db.transaction('metadata', 'readonly');
                let store = tx.objectStore('metadata');
                let req = store.get(path);
                req.onsuccess = () => res(req.result);
                req.onerror = () => res(null);
            });
        }

        async function setCachedMetadata(path, tags) {
            let db = await dbPromise;
            if (!db) return;
            let tx = db.transaction('metadata', 'readwrite');
            tx.objectStore('metadata').put({ path: path, ...tags });
        }

        // --- 2. WEB AUDIO API EQUALIZER & VISUALIZER ---
        let audioCtx, analyser, bassFilter, midFilter, trebleFilter, isAudioInitialized = false;

        function initWebAudio() {
            if (isAudioInitialized) return;
            try {
                audioCtx = new (window.AudioContext || window.webkitAudioContext)();
                let source = audioCtx.createMediaElementSource(el.audio);
                
                analyser = audioCtx.createAnalyser();
                analyser.fftSize = 64;

                bassFilter = audioCtx.createBiquadFilter();
                bassFilter.type = 'lowshelf';
                bassFilter.frequency.value = 250;

                midFilter = audioCtx.createBiquadFilter();
                midFilter.type = 'peaking';
                midFilter.frequency.value = 1500;
                midFilter.Q.value = 1;

                trebleFilter = audioCtx.createBiquadFilter();
                trebleFilter.type = 'highshelf';
                trebleFilter.frequency.value = 4000;

                source.connect(bassFilter);
                bassFilter.connect(midFilter);
                midFilter.connect(trebleFilter);
                trebleFilter.connect(analyser);
                analyser.connect(audioCtx.destination);

                el.eqBass.oninput = (e) => { bassFilter.gain.value = e.target.value; };
                el.eqMid.oninput = (e) => { midFilter.gain.value = e.target.value; };
                el.eqTreble.oninput = (e) => { trebleFilter.gain.value = e.target.value; };

                isAudioInitialized = true;
                drawVisualizer();
            } catch(e) {}
        }

        function drawVisualizer() {
            requestAnimationFrame(drawVisualizer);
            if (!analyser || !el.fs.classList.contains('active')) return;
            
            let canvas = el.canvas, ctx = canvas.getContext('2d');
            canvas.width = canvas.clientWidth * window.devicePixelRatio;
            canvas.height = canvas.clientHeight * window.devicePixelRatio;
            
            let bufferLength = analyser.frequencyBinCount;
            let dataArray = new Uint8Array(bufferLength);
            analyser.getByteFrequencyData(dataArray);
            
            ctx.clearRect(0, 0, canvas.width, canvas.height);
            let barWidth = (canvas.width / bufferLength) * 1.5;
            let x = 0;
            
            for (let i = 0; i < bufferLength; i++) {
                let barHeight = (dataArray[i] / 255) * canvas.height;
                ctx.fillStyle = `rgba(29, 185, 84, ${0.4 + (dataArray[i]/255)*0.6})`;
                ctx.fillRect(x, canvas.height - barHeight, barWidth - 2, barHeight);
                x += barWidth;
            }
        }

        // --- 3. MEDIASESSION API (LOCK SCREEN & BLUETOOTH KEYS) ---
        function updateMediaSession(title, artist, coverUrl) {
            if ('mediaSession' in navigator) {
                navigator.mediaSession.metadata = new MediaMetadata({
                    title: title,
                    artist: artist || 'mps3 Player',
                    album: 'Cartão SD',
                    artwork: coverUrl ? [{ src: coverUrl, sizes: '512x512', type: 'image/png' }] : []
                });
                navigator.mediaSession.setActionHandler('play', () => togglePlay());
                navigator.mediaSession.setActionHandler('pause', () => togglePlay());
                navigator.mediaSession.setActionHandler('previoustrack', () => prevTrack());
                navigator.mediaSession.setActionHandler('nexttrack', () => nextTrack());
                navigator.mediaSession.setActionHandler('seekto', (details) => {
                    if (details.seekTime) el.audio.currentTime = details.seekTime;
                });
            }
        }

        // --- SYSTEM STATUS & DIRECTORY RENDER ---
        async function fetchStatus(){
            try{
                let r=await fetch('/api/status'), j=await r.json();
                el.sdInfoText.textContent = 'SD: '+fmtSize(j.total-j.free)+' / '+fmtSize(j.total);
                if (j.total > 0) el.sdBar.style.width = ((j.total-j.free)/j.total*100) + '%';
                el.batInfo.innerHTML = '🔋 ' + j.battery_percent + '% <span style="color:#888;font-size:0.85em;font-weight:normal;">(' + (j.battery_mv/1000).toFixed(2) + 'V)</span>' + (j.battery_time_left>0?' <span style="font-size:0.8em;color:#666;font-weight:normal">~'+Math.floor(j.battery_time_left/60)+'h '+j.battery_time_left%60+'m</span>':'');
            }catch(e){}
        }

        function renderTable(entries) {
            let filter = el.filter.value.toLowerCase().trim();
            el.tbody.innerHTML='';
            if(curDir!=='/'){
                let tr=document.createElement('tr'); tr.innerHTML='<td colspan="3" class="name dir"><span>📁 ..</span></td>';
                tr.querySelector('span').onclick=()=>{ curDir=curDir.split('/').slice(0,-1).join('/')||'/'; load(); };
                el.tbody.appendChild(tr);
            }
            let filtered = entries.filter(e => e.name.toLowerCase().includes(filter));
            if (!filtered.length) {
                el.tbody.innerHTML += '<tr><td colspan="3" style="text-align:center; color:#888; padding:1.5em;">Nenhum arquivo encontrado</td></tr>';
                return;
            }
            filtered.forEach(e=>{
                let tr=document.createElement('tr');
                let isAudio = !e.dir && ['mp3','flac','wav','m4a'].includes(e.name.toLowerCase().split('.').pop());
                
                tr.innerHTML = `
                    <td class="name ${e.dir?'dir':'file'}">
                        <span>${e.dir?'📁 ':'🎵 '}${e.name}</span>
                        ${!e.dir ? '<div class="actions">⋮</div>' : ''}
                    </td>
                    <td class="size">${e.dir?'':fmtSize(e.size)}</td>
                `;
                
                let span = tr.querySelector('span');
                if(e.dir) span.onclick = ()=>{ curDir=joinPath(curDir,e.name); el.filter.value=''; load(); };
                else if(isAudio) span.onclick = ()=>playFromFolder(e.name);
                
                let act = tr.querySelector('.actions');
                if(act) {
                    act.onclick = (ev)=>{
                        ev.stopPropagation();
                        currentCtxItem = { name: e.name, path: joinPath(curDir, e.name), isAudio: isAudio };
                        el.ctx.style.left = Math.min(ev.pageX - 120, window.innerWidth - 160) + 'px';
                        el.ctx.style.top = ev.pageY + 'px';
                        el.ctx.classList.add('active');
                    };
                }
                el.tbody.appendChild(tr);
            });
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
                el.tbody.innerHTML = '<tr><td colspan="3" style="text-align:center; padding: 2em; color: #888;"><div style="display:inline-block; animation: spin 1s linear infinite;">⏳</div> Carregando pasta...</td></tr>';
            }
            
            try{
                let r=await fetch('/api/list?path='+encodeURIComponent(curDir));
                let files=(await r.json()).entries || [];
                files.sort((a,b)=>a.dir!==b.dir ? (a.dir?-1:1) : a.name.localeCompare(b.name));
                dirCache[curDir] = files;
                currentEntries = files;
                renderTable(currentEntries);
            }catch(e){ 
                if (!dirCache[curDir]) el.tbody.innerHTML='<tr><td colspan=3>Erro ao ler pasta</td></tr>'; 
            }
        }

        document.onclick = ()=>el.ctx.classList.remove('active');

        // --- PLAYLIST LOGIC & MUTEX ---
        function getFolderAudios() {
            let items = [];
            currentEntries.forEach(e => {
                if(!e.dir && ['mp3','flac','wav','m4a'].includes(e.name.toLowerCase().split('.').pop())) {
                    items.push({ name: e.name, path: joinPath(curDir, e.name) });
                }
            });
            return items;
        }

        function playFromFolder(targetName) {
            if (isUploading) {
                alert('Upload em andamento! Aguarde o envio terminar.');
                return;
            }
            initWebAudio();
            if (audioCtx && audioCtx.state === 'suspended') audioCtx.resume();

            let items = getFolderAudios();
            originalPlaylist = [...items];
            playlist = [...items];
            playIndex = playlist.findIndex(x=>x.name === targetName);
            
            if(isShuffle) {
                let curr = playlist[playIndex];
                playlist.sort(()=>Math.random()-0.5);
                playIndex = playlist.indexOf(curr);
            }
            loadTrack();
        }

        async function loadTrack() {
        if (isUploading) {
            if (el.audio) el.audio.pause();
            return;
        }
        if(playIndex < 0 || playIndex >= playlist.length) return;
        let trk = playlist[playIndex];
        
        // --- 1. UI DE CARREGAMENTO ANIMADO ---
        el.playerBar.classList.add('active');
        el.title.textContent = el.fsTitle.textContent = "⏳ Extraindo Capa...";
        el.artist.textContent = el.fsArtist.textContent = trk.name;
        el.art.src = el.fsArt.src = defaultCover;
        el.fsBg.style.backgroundImage = 'none';
        
        el.art.classList.add('loading-spin');
        el.fsArt.classList.add('loading-spin');

        let url = '/api/download?path=' + encodeURIComponent(trk.path);
        
        // --- 2. EXTRAIR METADATA E CAPA *PRIMEIRO* ---
        let cached = await getCachedMetadata(trk.path);
        let title = trk.name;
        let artist = 'Desconhecido';
        let picture = null;
        
        if (cached) {
            title = cached.title || trk.name;
            artist = cached.artist || 'Desconhecido';
            picture = cached.picture;
        } else {
            try {
                let t = await extractMetadata(url);
                title = t.title || trk.name;
                artist = t.artist || 'Desconhecido';
                picture = t.picture;
                setCachedMetadata(trk.path, { title: title, artist: artist, picture: picture });
            } catch(e) {}
        }
        
        // --- 3. ATUALIZAR UI COM OS DADOS ---
        el.title.textContent = el.fsTitle.textContent = title;
        el.artist.textContent = el.fsArtist.textContent = artist;
        if (picture) {
            el.art.src = el.fsArt.src = picture;
            el.fsBg.style.backgroundImage = 'url("'+picture+'")';
        }
        updateMediaSession(title, artist, picture);
        
        el.art.classList.remove('loading-spin');
        el.fsArt.classList.remove('loading-spin');
        
        // --- 4. TOCAR AUDIO DEPOIS QUE A REDE ESTA LIVRE ---
        el.audio.src = url;
        el.audio.play().catch(()=>{});
        updatePlayBtn(true);
    }function togglePlay() {
            if (isUploading) {
                alert('Upload em andamento! Aguarde o envio terminar.');
                return;
            }
            initWebAudio();
            if (audioCtx && audioCtx.state === 'suspended') audioCtx.resume();
            if(el.audio.paused) el.audio.play();
            else el.audio.pause();
        }
        const svgPlay = '<svg fill="currentColor" viewBox="0 0 24 24" width="20" height="20"><path d="M8 5v14l11-7z"/></svg>';
        const svgPause = '<svg fill="currentColor" viewBox="0 0 24 24" width="20" height="20"><path d="M6 19h4V5H6v14zm8-14v14h4V5h-4z"/></svg>';
        const svgPlayFs = '<svg fill="currentColor" viewBox="0 0 24 24" width="30" height="30"><path d="M8 5v14l11-7z"/></svg>';
        const svgPauseFs = '<svg fill="currentColor" viewBox="0 0 24 24" width="30" height="30"><path d="M6 19h4V5H6v14zm8-14v14h4V5h-4z"/></svg>';
        function updatePlayBtn(playing) {
            el.playBtn.innerHTML = playing ? svgPause : svgPlay;
            el.fsPlay.innerHTML = playing ? svgPauseFs : svgPlayFs;
        }
        el.audio.onplay = ()=>updatePlayBtn(true);
        el.audio.onpause = ()=>updatePlayBtn(false);
        el.audio.onended = ()=>nextTrack();
        
        el.audio.ontimeupdate = ()=>{
            el.cur.textContent = el.fsCur.textContent = fmtTime(el.audio.currentTime);
            el.tot.textContent = el.fsTot.textContent = fmtTime(el.audio.duration);
            let p = (el.audio.currentTime / el.audio.duration)*100 || 0;
            el.prog.style.width = el.fsProg.style.width = p+'%';
        };

        function seek(ev, cont) {
            let rect = cont.getBoundingClientRect();
            el.audio.currentTime = ((ev.clientX - rect.left) / rect.width) * el.audio.duration;
        }

        function nextTrack() {
            if(repeatMode === 2) { el.audio.currentTime=0; el.audio.play(); return; }
            playIndex++;
            if(playIndex >= playlist.length) {
                if(repeatMode === 1) playIndex = 0;
                else { playIndex--; return; }
            }
            loadTrack();
        }
        function prevTrack() {
            if(el.audio.currentTime > 3) { el.audio.currentTime=0; return; }
            playIndex--;
            if(playIndex < 0) playIndex = playlist.length-1;
            loadTrack();
        }

        function toggleShuffle() {
            isShuffle = !isShuffle;
            el.btnShuf.classList.toggle('active', isShuffle);
            if(!playlist.length) return;
            let curr = playlist[playIndex];
            if(isShuffle) {
                playlist.sort(()=>Math.random()-0.5);
            } else {
                playlist = [...originalPlaylist];
            }
            playIndex = playlist.indexOf(curr);
        }
        function toggleRepeat() {
            repeatMode = (repeatMode + 1) % 3;
            el.btnRep.classList.toggle('active', repeatMode > 0);
            // using same SVG but changing opacity or color via CSS class 'active', or swapping icon.
            if (repeatMode === 2) {
                // Repeat ONE icon
                el.btnRep.innerHTML = '<svg fill="currentColor" viewBox="0 0 24 24" width="24" height="24"><path d="M7 7h10v3l4-4-4-4v3H5v6h2V7zm10 10H7v-3l-4 4 4 4v-3h12v-6h-2v4zm-4-2V9h-1l-2 1v1h1.5v4L13 15z"/></svg>';
            } else {
                // Repeat ALL icon
                el.btnRep.innerHTML = '<svg fill="currentColor" viewBox="0 0 24 24" width="24" height="24"><path d="M7 7h10v3l4-4-4-4v3H5v6h2V7zm10 10H7v-3l-4 4 4 4v-3h12v-6h-2v4z"/></svg>';
            }
        }
        function toggleFullscreen() {
            el.fs.classList.toggle('active');
            document.body.classList.toggle('fullscreen-active');
            if (el.fs.classList.contains('active')) {
                initWebAudio();
                if (audioCtx && audioCtx.state === 'suspended') audioCtx.resume();
            }
        }

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
        document.getElementById('ctxDel').onclick = async ()=>{
            if(!currentCtxItem) return;
            if(confirm('Excluir '+currentCtxItem.name+'?')) {
                await fetch('/api/delete?path='+encodeURIComponent(currentCtxItem.path), {method:'DELETE'});
                load();
            }
        };

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
                const xhr = new XMLHttpRequest();
                const url = '/api/upload?path=' + encodeURIComponent(joinPath(curDir, relPath)) +
                    '&idx=' + idx + '&count=' + count + '&batchTotal=' + batchTotal;
                xhr.open('PUT', url, true);
                xhr.timeout = 300000;
                let t0 = performance.now(), lastProgress = t0, lastBytes = 0;

                xhr.upload.onprogress = (ev) => {
                    if (!ev.lengthComputable) return;
                    const now = performance.now();
                    const pct = Math.round(ev.loaded * 100 / ev.total);
                    const deltaTime = (now - lastProgress) / 1000;
                    const deltaBytes = ev.loaded - lastBytes;
                    const kbps = deltaTime > 0.05 ? (deltaBytes / 1024) / deltaTime : 0;
                    lastBytes = ev.loaded; lastProgress = now;

                    el.upFileBar.style.width = pct + '%';
                    el.upFilePct.textContent = pct + '%';
                    const etaSec = kbps > 0.05 ? ((ev.total - ev.loaded) / 1024) / kbps : -1;
                    el.upSpeed.textContent = kbps >= 1024 ? (kbps / 1024).toFixed(1) + ' MB/s' : Math.round(kbps) + ' KB/s';
                    el.upEta.textContent = etaSec >= 0 ? fmtEta(etaSec) : '';

                    if (count > 1) {
                        const overallDone = window.__batchDoneBefore + ev.loaded;
                        const opct = batchTotal > 0 ? Math.round(overallDone * 100 / batchTotal) : 0;
                        el.upOverallBar.style.width = opct + '%';
                        el.upOverallPct.textContent = opct + '%';
                        el.upOverallLabel.textContent = 'Geral (' + idx + '/' + count + ')';
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

            // MUTEX: Pausar reprodução imediatamente
            if (el.audio) el.audio.pause();
            isUploading = true;

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

                try {
                    await uploadOne(relPath, f, i + 1, count, batchTotal, q.id);
                } catch (e) {
                    errored = true;
                    alert('Falha ao enviar ' + relPath + ': ' + e.message);
                    break;
                }
                window.__batchDoneBefore += f.size;
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

        // --- METADATA EXTRACTION (SAFE NON-BLOCKING) ---
        async function extractMetadata(url) {
            try {
                let res = await fetch(url, { headers: { 'Range': 'bytes=0-2097151' } });
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

        

        const configBtn = document.getElementById('configBtn');
        const configModal = document.getElementById('configModal');
        const cfgSSID = document.getElementById('cfgSSID');
        const cfgPass = document.getElementById('cfgPass');
        const cfgStatus = document.getElementById('cfgStatus');
        const btnSaveCfg = document.getElementById('btnSaveCfg');
        const btnCloseCfg = document.getElementById('btnCloseCfg');
        const netList = document.getElementById('netList');
        const netEmpty = document.getElementById('netEmpty');

        function loadNetworks() {
            fetch('/api/wifi/networks')
            .then(r => r.json())
            .then(d => renderNetworks((d.networks || []).map(n => n.ssid)))
            .catch(() => renderNetworks([]));
        }

        async function deleteNetwork(ssid) {
            try {
                const r = await fetch('/api/wifi/networks', {
                    method: 'DELETE',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ ssid })
                });
                if (r.ok) loadNetworks();
                else cfgStatus.textContent = 'Erro ao remover.';
            } catch(e) {
                cfgStatus.textContent = 'Erro ao remover.';
            }
        }

        function renderNetworks(arr) {
            netList.innerHTML = '';
            if (arr.length === 0) {
                netEmpty.style.display = 'block';
            } else {
                netEmpty.style.display = 'none';
                arr.forEach(s => {
                    let li = document.createElement('li');
                    li.style.display = 'flex'; li.style.justifyContent = 'space-between'; li.style.padding = '4px 6px'; li.style.borderBottom = '1px solid #eee';
                    li.textContent = s;
                    let del = document.createElement('span');
                    del.textContent = '✕';
                    del.style.color = '#c33'; del.style.cursor = 'pointer';
                    del.onclick = () => deleteNetwork(s);
                    li.appendChild(del);
                    netList.appendChild(li);
                });
            }
        }

        configBtn.onclick = () => {
            configModal.style.display = 'block';
            loadNetworks();
        };

        btnCloseCfg.onclick = () => { configModal.style.display = 'none'; cfgStatus.textContent = ''; };

        btnSaveCfg.onclick = async () => {
            const ssid = cfgSSID.value.trim();
            const pass = cfgPass.value;
            if (!ssid) { cfgStatus.textContent = 'SSID não pode ficar vazio.'; return; }
            try {
                const r = await fetch('/api/wifi/networks', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ ssid, pass })
                });
                if (r.ok) {
                    cfgStatus.textContent = 'Rede salva! Conectando...';
                    cfgSSID.value = '';
                    cfgPass.value = '';
                    loadNetworks();
                    setTimeout(() => { cfgStatus.textContent = ''; }, 2500);
                } else {
                    cfgStatus.textContent = 'Erro ao salvar.';
                }
            } catch(e) {
                cfgStatus.textContent = 'Erro de conexão.';
            }
        };

        fetchStatus(); setInterval(fetchStatus, 8000); load();
    