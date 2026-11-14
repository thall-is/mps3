/* ==========================================================================
 * MÓDULO JS 02: Autenticação Admin, Mapa DOM (el), Estado Global e Utilitários
 * ========================================================================== */
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
            prog:document.getElementById('progressFill'), buffer:document.getElementById('bufferFill'),
            
            fs:document.getElementById('fs-player'), fsBg:document.getElementById('fs-bg'),
            fsArt:document.getElementById('fsArt'), fsTitle:document.getElementById('fsTitle'), fsArtist:document.getElementById('fsArtist'),
            fsCur:document.getElementById('fsTimeCur'), fsTot:document.getElementById('fsTimeTot'),
            fsProg:document.getElementById('fsProgressFill'), fsBuffer:document.getElementById('fsBufferFill'), fsPlay:document.getElementById('fsPlayBtn'),
            btnShuf:document.getElementById('btnShuffle'), btnRep:document.getElementById('btnRepeat'),
            canvas:document.getElementById('visualizer'),
            eqBass:document.getElementById('eqBass'), eqMid:document.getElementById('eqMid'), eqTreble:document.getElementById('eqTreble'),
            
            dropzone:document.getElementById('dropzone'), inpFiles:document.getElementById('inpFiles'), inpFolder:document.getElementById('inpFolder'),
            upBox:document.getElementById('upload-status-box'), upFileName:document.getElementById('upFileName'), upFilePct:document.getElementById('upFilePct'),
            upFileBar:document.getElementById('upFileBar'), upOverallLabel:document.getElementById('upOverallLabel'), upOverallPct:document.getElementById('upOverallPct'),
            upOverallBar:document.getElementById('upOverallBar'), upSpeed:document.getElementById('upSpeed'), upEta:document.getElementById('upEta'),
            queue:document.getElementById('queue'),

            dlBox:document.getElementById('download-status-box'), dlFileName:document.getElementById('dlFileName'), dlFilePct:document.getElementById('dlFilePct'),
            dlFileBar:document.getElementById('dlFileBar'), dlSpeed:document.getElementById('dlSpeed'), dlEta:document.getElementById('dlEta'), dlSize:document.getElementById('dlSize'),
            
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
        let selectedItems = new Map(); // path -> { path, name, isDir }

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
