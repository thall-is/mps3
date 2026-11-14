/* ==========================================================================
 * MÓDULO JS 07: Gerenciador de Redes Wi-Fi (/api/wifi) e Equalizador DSP 10 Bandas
 * ========================================================================== */
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
            if (typeof window.updateCloudStatusUI === 'function') window.updateCloudStatusUI();
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

        
        // ================= EQUALIZADOR DE 10 BANDAS =================
        let eqConfig = null;
        let selectedPresetIdx = 0;
        const EQ_FREQS = ["31Hz", "62Hz", "125Hz", "250Hz", "500Hz", "1kHz", "2kHz", "4kHz", "8kHz", "16kHz"];
        const EQ_CURVES = {
            'flat': [0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
            'rock': [5, 4, 3, 1, -1, -1, 1, 2, 3, 4],
            'pop': [-1, -1, 2, 3, 4, 4, 2, 1, -1, -2],
            'dance': [6, 5, 2, 0, -1, -1, 0, 2, 4, 5],
            'jazz': [3, 2, 1, 2, -1, -1, 0, 1, 2, 3],
            'classica': [4, 3, 2, 1, 1, 1, 2, 3, 4, 4],
            'vocal': [-2, -1, 1, 3, 4, 4, 3, 1, 0, -1],
            'bass': [6, 5, 4, 2, 1, 0, 0, 0, 0, 0]
        };

        function renderEqBands() {
            const container = document.getElementById('eqBandsContainer');
            container.innerHTML = '';
            for(let b = 0; b < 10; b++) {
                const col = document.createElement('div');
                col.className = 'eq-fader-col';
                col.innerHTML = `
                    <span class="eq-fader-val" id="eqBandVal${b}">0.0 dB</span>
                    <div class="eq-fader-track-wrap">
                        <div class="eq-fader-zero-line"></div>
                        <input type="range" id="eqBandSlider${b}" class="v-fader" min="-15" max="15" step="0.5" value="0">
                    </div>
                    <span class="eq-fader-freq">${EQ_FREQS[b]}</span>
                `;
                container.appendChild(col);
                const slider = col.querySelector('input');
                slider.oninput = (e) => {
                    const val = parseFloat(e.target.value);
                    document.getElementById(`eqBandVal${b}`).textContent = (val > 0 ? '+' : '') + val.toFixed(1) + ' dB';
                    if(eqConfig && eqConfig.presets && eqConfig.presets[selectedPresetIdx]) {
                        eqConfig.presets[selectedPresetIdx].band_gains[b] = val;
                    }
                };
            }
        }

        async function loadEq() {
            try {
                const res = await fetch('/api/eq');
                if(res.ok) {
                    eqConfig = await res.json();
                    selectedPresetIdx = eqConfig.active_preset_idx || 0;
                    populatePresetSelect();
                    updateEqUI();
                }
            } catch(e) {
                console.error("Erro ao carregar EQ:", e);
            }
        }

        function populatePresetSelect() {
            const sel = document.getElementById('eqPresetSelect');
            sel.innerHTML = '';
            for(let p = 0; p < (eqConfig.presets ? eqConfig.presets.length : 10); p++) {
                const opt = document.createElement('option');
                opt.value = p;
                const name = (eqConfig.presets && eqConfig.presets[p] && eqConfig.presets[p].name) ? eqConfig.presets[p].name : `Preset ${p+1}`;
                opt.textContent = `${p+1}: ${name}`;
                if(p === selectedPresetIdx) opt.selected = true;
                sel.appendChild(opt);
            }
        }

        function updateEqUI() {
            if(!eqConfig || !eqConfig.presets) return;
            document.getElementById('eqMasterEnable').checked = !!eqConfig.enabled;
            const p = eqConfig.presets[selectedPresetIdx];
            if(!p) return;

            document.getElementById('eqPresetName').value = p.name || `Preset ${selectedPresetIdx+1}`;
            
            const ovr = p.overall_gain || 0;
            document.getElementById('eqOverallSlider').value = ovr;
            document.getElementById('eqOverallVal').textContent = (ovr > 0 ? '+' : '') + ovr.toFixed(1) + ' dB';

            for(let b = 0; b < 10; b++) {
                const g = (p.band_gains && p.band_gains[b] !== undefined) ? p.band_gains[b] : 0;
                const sl = document.getElementById(`eqBandSlider${b}`);
                if(sl) sl.value = g;
                const lbl = document.getElementById(`eqBandVal${b}`);
                if(lbl) lbl.textContent = (g > 0 ? '+' : '') + g.toFixed(1) + ' dB';
            }
        }

        function applyEqCurve(name) {
            const curve = EQ_CURVES[name];
            if(!curve || !eqConfig || !eqConfig.presets) return;
            const p = eqConfig.presets[selectedPresetIdx];
            if(!p) return;
            for(let b = 0; b < 10; b++) {
                p.band_gains[b] = curve[b];
            }
            updateEqUI();
        }

        async function saveEq() {
            if(!eqConfig) return;
            const nameInput = document.getElementById('eqPresetName').value.trim();
            if(nameInput && eqConfig.presets && eqConfig.presets[selectedPresetIdx]) {
                eqConfig.presets[selectedPresetIdx].name = nameInput;
            }
            eqConfig.enabled = document.getElementById('eqMasterEnable').checked;

            const st = document.getElementById('eqStatusMsg');
            st.textContent = 'Enviando...';
            try {
                const res = await fetch('/api/eq', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    body: JSON.stringify(eqConfig)
                });
                if(res.ok) {
                    st.textContent = 'Salvo no MPS3 com sucesso!';
                    populatePresetSelect();
                    setTimeout(()=>{ st.textContent = ''; }, 3000);
                } else {
                    st.textContent = 'Erro ao salvar.';
                }
            } catch(e) {
                st.textContent = 'Erro de rede.';
            }
        }

        async function activatePreset() {
            if(!eqConfig) return;
            eqConfig.active_preset_idx = selectedPresetIdx;
            eqConfig.enabled = true;
            document.getElementById('eqMasterEnable').checked = true;
            await saveEq();
        }

        document.getElementById('eqBtn').onclick = () => {
            renderEqBands();
            document.getElementById('eqModal').style.display = 'block';
            loadEq();
        };

        document.getElementById('btnCloseEq').onclick = () => {
            document.getElementById('eqModal').style.display = 'none';
            document.getElementById('eqStatusMsg').textContent = '';
        };

        document.getElementById('eqPresetSelect').onchange = (e) => {
            selectedPresetIdx = parseInt(e.target.value);
            updateEqUI();
        };

        document.getElementById('eqOverallSlider').oninput = (e) => {
            const val = parseFloat(e.target.value);
            document.getElementById('eqOverallVal').textContent = (val > 0 ? '+' : '') + val.toFixed(1) + ' dB';
            if(eqConfig && eqConfig.presets && eqConfig.presets[selectedPresetIdx]) {
                eqConfig.presets[selectedPresetIdx].overall_gain = val;
            }
        };

        document.getElementById('btnSaveEq').onclick = saveEq;
        document.getElementById('btnActivatePreset').onclick = activatePreset;
