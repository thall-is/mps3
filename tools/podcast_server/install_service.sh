#!/usr/bin/env bash
# Script de instalacao do MPS3 Podcast Sync Server na SBC Radxa Rock 3E
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=================================================="
echo "  Instalando MPS3 Podcast Sync Server"
echo "=================================================="

sudo mkdir -p /opt/mps3-sync
sudo cp "${SCRIPT_DIR}/mps3_podcast_server.py" /opt/mps3-sync/
sudo chmod +x /opt/mps3-sync/mps3_podcast_server.py
sudo cp "${SCRIPT_DIR}/mps3-sync.service" /etc/systemd/system/

sudo systemctl daemon-reload
sudo systemctl enable mps3-sync.service
sudo systemctl restart mps3-sync.service

echo ""
echo "[+] Servico ativado e iniciado com sucesso!"
sudo systemctl status mps3-sync.service --no-pager

