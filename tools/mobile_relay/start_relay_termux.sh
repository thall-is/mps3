#!/data/data/com.termux/files/usr/bin/bash
# Script de 1 clique para iniciar o MPS3 Relay no Termux

# Garante python instalado
if ! command -v python &> /dev/null; then
    echo "Instalando Python no Termux..."
    pkg update -y && pkg install python -y
fi

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
echo "Iniciando MPS3 Relay..."
python "$DIR/mps3_relay.py"

