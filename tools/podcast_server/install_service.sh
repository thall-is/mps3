#!/usr/bin/env bash
# Script de instalacao do MPS3 Podcast Sync Service (Push Sync v2.0 + Server) na SBC Radxa Rock 3E
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PODCASTS_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

echo "=================================================="
echo "  Instalando MPS3 Podcast Sync v2.0 (Push + HTTP)"
echo "=================================================="

sudo mkdir -p /opt/mps3-sync
sudo cp "${SCRIPT_DIR}/mps3_podcast_server.py" /opt/mps3-sync/
sudo chmod +x /opt/mps3-sync/mps3_podcast_server.py

if [ -d "${PODCASTS_ROOT}/mps3_sync/mps3_sync" ]; then
    sudo rm -rf /opt/mps3-sync/mps3_sync
    sudo cp -r "${PODCASTS_ROOT}/mps3_sync/mps3_sync" /opt/mps3-sync/
    echo "[+] Pacote mps3_sync copiado para /opt/mps3-sync/mps3_sync"
fi

sudo cp "${SCRIPT_DIR}/mps3-sync.service" /etc/systemd/system/

sudo systemctl daemon-reload
sudo systemctl enable mps3-sync.service
sudo systemctl restart mps3-sync.service

echo ""
echo "[+] Servico ativado e iniciado com sucesso!"
sudo systemctl status mps3-sync.service --no-pager


