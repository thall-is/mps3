/* ==========================================================================
 * MÓDULO JS 08: Atualização de Firmware OTA via Navegador (/api/ota)
 * ========================================================================== */
        // ================= ATUALIZAÇÃO DE FIRMWARE (OTA) =================
        let otaFile = null;
        let otaXhr = null;

        async function openOtaModal() {
            document.getElementById('otaModal').style.display = 'flex';
            document.getElementById('otaFileSection').style.display = 'block';
            document.getElementById('otaProgressBox').style.display = 'none';
            document.getElementById('otaCountdownBox').style.display = 'none';
            document.getElementById('otaFileInput').value = '';
            document.getElementById('otaFileInput').disabled = false;
            document.getElementById('btnStartOta').disabled = true;
            document.getElementById('btnCancelOta').disabled = false;
            document.getElementById('btnCloseOta').disabled = false;
            document.getElementById('otaFileErr').style.display = 'none';
            otaFile = null;

            document.getElementById('otaRunningVer').textContent = 'Consultando...';
            document.getElementById('otaRunningPart').textContent = '--';
            document.getElementById('otaNextPart').textContent = '--';
            document.getElementById('otaBatteryInfo').textContent = '--';

            try {
                const res = await fetch('/api/ota');
                if (res.ok) {
                    const data = await res.json();
                    document.getElementById('otaRunningVer').textContent = data.running_version || 'Desconhecida';
                    document.getElementById('otaRunningPart').textContent = data.running_partition || 'ota_0';
                    document.getElementById('otaNextPart').textContent = data.next_partition || 'ota_1';
                    
                    const batPct = data.battery_percent !== undefined ? data.battery_percent : '--';
                    const isCharging = data.battery_charging;
                    document.getElementById('otaBatteryInfo').textContent = `${batPct}% ${isCharging ? '⚡ (Carregando)' : ''}`;

                    if (data.battery_percent < 15 && !data.battery_charging) {
                        const err = document.getElementById('otaFileErr');
                        err.textContent = 'Bateria muito baixa (< 15%). Conecte o carregador antes de atualizar!';
                        err.style.display = 'block';
                    }
                }
            } catch(e) {
                document.getElementById('otaRunningVer').textContent = 'Erro ao consultar';
            }
        }

        function closeOtaModal() {
            if (otaXhr) {
                if (!confirm("Atualização em andamento! Deseja cancelar?")) return;
                otaXhr.abort();
                otaXhr = null;
            }
            document.getElementById('otaModal').style.display = 'none';
        }

        document.getElementById('otaBtn').onclick = openOtaModal;
        document.getElementById('btnCloseOta').onclick = closeOtaModal;
        document.getElementById('btnCancelOta').onclick = closeOtaModal;

        document.getElementById('otaFileInput').onchange = (e) => {
            const files = e.target.files;
            const err = document.getElementById('otaFileErr');
            err.style.display = 'none';
            if (files && files.length > 0) {
                const file = files[0];
                if (!file.name.toLowerCase().endsWith('.bin')) {
                    err.textContent = 'Selecione um arquivo de firmware com extensão .bin';
                    err.style.display = 'block';
                    document.getElementById('btnStartOta').disabled = true;
                    otaFile = null;
                    return;
                }
                otaFile = file;
                document.getElementById('btnStartOta').disabled = false;
            } else {
                document.getElementById('btnStartOta').disabled = true;
                otaFile = null;
            }
        };

        document.getElementById('btnStartOta').onclick = () => {
            if (!otaFile) return;

            if (!confirm(`Confirmar gravação do firmware "${otaFile.name}" (${(otaFile.size / 1024 / 1024).toFixed(2)} MB) no MPS3?`)) {
                return;
            }

            document.getElementById('btnStartOta').disabled = true;
            document.getElementById('btnCancelOta').disabled = true;
            document.getElementById('btnCloseOta').disabled = true;
            document.getElementById('otaFileInput').disabled = true;
            document.getElementById('otaProgressBox').style.display = 'block';
            document.getElementById('otaProgressBar').style.width = '0%';
            document.getElementById('otaPctText').textContent = '0%';
            document.getElementById('otaStatusText').textContent = 'Gravando Flash SPI...';

            otaXhr = new XMLHttpRequest();
            otaXhr.open('POST', '/api/ota');
            otaXhr.setRequestHeader('Content-Type', 'application/octet-stream');

            otaXhr.upload.onprogress = (evt) => {
                if (evt.lengthComputable) {
                    const pct = Math.round((evt.loaded / evt.total) * 100);
                    document.getElementById('otaProgressBar').style.width = `${pct}%`;
                    document.getElementById('otaPctText').textContent = `${pct}%`;
                    if (pct >= 100) {
                        document.getElementById('otaStatusText').textContent = 'Finalizando gravação na partição flash...';
                    }
                }
            };

            otaXhr.onreadystatechange = () => {
                if (otaXhr && otaXhr.readyState === 4) {
                    if (otaXhr.status === 200) {
                        document.getElementById('otaProgressBar').style.width = '100%';
                        document.getElementById('otaPctText').textContent = '100%';
                        document.getElementById('otaProgressBox').style.display = 'none';
                        document.getElementById('otaFileSection').style.display = 'none';
                        document.getElementById('otaCountdownBox').style.display = 'block';

                        let sec = 10;
                        document.getElementById('otaCountdownSec').textContent = sec;
                        const timer = setInterval(() => {
                            sec--;
                            document.getElementById('otaCountdownSec').textContent = sec;
                            if (sec <= 0) {
                                clearInterval(timer);
                                location.reload();
                            }
                        }, 1000);
                    } else {
                        let errMsg = 'Erro na atualização.';
                        try {
                            const errResp = JSON.parse(otaXhr.responseText);
                            if (errResp.error) errMsg = errResp.error;
                        } catch(e) {}
                        alert(`Falha na atualização OTA (${otaXhr.status}):\n${errMsg}`);
                        document.getElementById('btnStartOta').disabled = false;
                        document.getElementById('btnCancelOta').disabled = false;
                        document.getElementById('btnCloseOta').disabled = false;
                        document.getElementById('otaFileInput').disabled = false;
                        document.getElementById('otaProgressBox').style.display = 'none';
                    }
                    otaXhr = null;
                }
            };

            otaXhr.onerror = () => {
                alert('Erro de conexão durante o upload OTA.');
                document.getElementById('btnStartOta').disabled = false;
                document.getElementById('btnCancelOta').disabled = false;
                document.getElementById('btnCloseOta').disabled = false;
                document.getElementById('otaFileInput').disabled = false;
                document.getElementById('otaProgressBox').style.display = 'none';
                otaXhr = null;
            };

            otaXhr.send(otaFile);
        };
