/* ==========================================================================
 * MÓDULO JS 04: Web Audio API Player, Analisador de Espectro e Menu de Contexto
 * ========================================================================== */
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
            
            el.playerBar.classList.add('active');
            let url = '/api/download?path=' + encodeURIComponent(trk.path);
            
            // Dados preliminares inteligentes a partir do caminho e nome
            let parts = trk.path.split('/').filter(x => x);
            let defaultArtist = parts.length >= 2 ? parts[parts.length - 2] : 'Cartão SD';
            let title = trk.name.replace(/\.[^.]+$/, '');
            let artist = defaultArtist;
            let coverSrc = defaultCover;
            
            el.title.textContent = el.fsTitle.textContent = title;
            el.artist.textContent = el.fsArtist.textContent = artist;
            el.art.src = el.fsArt.src = defaultCover;
            el.fsBg.style.backgroundImage = 'none';

            // Busca metadados de texto e capa embutida (128KB Range request rápida) ANTES do stream contínuo
            // para não disputar o servidor httpd single-thread do ESP32
            try {
                let cached = await getCachedMetadata(trk.path);
                if (cached && (cached.title || cached.artist || cached.picture)) {
                    if (cached.title) title = el.title.textContent = el.fsTitle.textContent = cached.title;
                    if (cached.artist) artist = el.artist.textContent = el.fsArtist.textContent = cached.artist;
                    if (cached.picture) {
                        coverSrc = el.art.src = el.fsArt.src = cached.picture;
                        el.fsBg.style.backgroundImage = 'url("' + cached.picture + '")';
                    }
                } else {
                    let t = await extractMetadata(url);
                    if (t.title) title = el.title.textContent = el.fsTitle.textContent = t.title;
                    if (t.artist) artist = el.artist.textContent = el.fsArtist.textContent = t.artist;
                    if (t.picture) {
                        coverSrc = el.art.src = el.fsArt.src = t.picture;
                        el.fsBg.style.backgroundImage = 'url("' + t.picture + '")';
                    }
                    if (t.title || t.artist || t.picture) {
                        setCachedMetadata(trk.path, { title: title, artist: artist, picture: t.picture || null });
                    }
                }
            } catch(e) {}

            updateMediaSession(title, artist, coverSrc !== defaultCover ? coverSrc : '');

            if (isUploading) return;
            let streamUrl = '/api/download?path=' + encodeURIComponent(trk.path) + '&stream=1';
            if (el.buffer) el.buffer.style.width = '0%';
            if (el.fsBuffer) el.fsBuffer.style.width = '0%';
            if (el.playBtn) el.playBtn.classList.add('buffering');
            if (el.fsPlay) el.fsPlay.classList.add('buffering');
            el.audio.src = streamUrl;
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
        function updateBufferProgress() {
            if (!el.audio || !el.audio.duration || !el.audio.buffered || !el.audio.buffered.length) {
                return;
            }
            let dur = el.audio.duration;
            let cur = el.audio.currentTime || 0;
            let bufEnd = 0;
            for (let i = 0; i < el.audio.buffered.length; i++) {
                let s = el.audio.buffered.start(i);
                let e = el.audio.buffered.end(i);
                if (s <= cur && cur <= e) {
                    bufEnd = e;
                    break;
                }
            }
            if (!bufEnd && el.audio.buffered.length > 0) {
                bufEnd = el.audio.buffered.end(el.audio.buffered.length - 1);
            }
            let pct = Math.min(100, Math.max(0, (bufEnd / dur) * 100));
            let pctStr = pct.toFixed(1) + '%';
            if (el.buffer) el.buffer.style.width = pctStr;
            if (el.fsBuffer) el.fsBuffer.style.width = pctStr;
        }

        function updatePlayBtn(playing) {
            el.playBtn.innerHTML = playing ? svgPause : svgPlay;
            el.fsPlay.innerHTML = playing ? svgPauseFs : svgPlayFs;
        }
        el.audio.onplay = ()=>{
            updatePlayBtn(true);
            if (el.playBtn) el.playBtn.classList.remove('buffering');
            if (el.fsPlay) el.fsPlay.classList.remove('buffering');
        };
        el.audio.onpause = ()=>{
            updatePlayBtn(false);
            if (el.playBtn) el.playBtn.classList.remove('buffering');
            if (el.fsPlay) el.fsPlay.classList.remove('buffering');
        };
        el.audio.onended = ()=>nextTrack();
        el.audio.onwaiting = ()=>{
            if (el.playBtn) el.playBtn.classList.add('buffering');
            if (el.fsPlay) el.fsPlay.classList.add('buffering');
        };
        el.audio.onplaying = ()=>{
            if (el.playBtn) el.playBtn.classList.remove('buffering');
            if (el.fsPlay) el.fsPlay.classList.remove('buffering');
        };
        el.audio.oncanplay = ()=>{
            if (el.playBtn) el.playBtn.classList.remove('buffering');
            if (el.fsPlay) el.fsPlay.classList.remove('buffering');
            updateBufferProgress();
        };
        el.audio.onprogress = ()=>updateBufferProgress();
        el.audio.onerror = ()=>{
            if (isUploading) {
                alert('Upload em andamento na velocidade maxima. SD temporariamente reservado.');
            }
            if (el.playBtn) el.playBtn.classList.remove('buffering');
            if (el.fsPlay) el.fsPlay.classList.remove('buffering');
            updatePlayBtn(false);
        };
        
        el.audio.ontimeupdate = ()=>{
            el.cur.textContent = el.fsCur.textContent = fmtTime(el.audio.currentTime);
            el.tot.textContent = el.fsTot.textContent = fmtTime(el.audio.duration);
            let p = (el.audio.currentTime / el.audio.duration)*100 || 0;
            el.prog.style.width = el.fsProg.style.width = p+'%';
            updateBufferProgress();
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
