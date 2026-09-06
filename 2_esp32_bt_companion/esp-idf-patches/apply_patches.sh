#!/bin/bash
# Script para aplicar patches necessarios no ESP-IDF para suporte a LDAC e Multi-SEP
IDF_TARGET="${IDF_PATH:-$1}"

if [ -z "$IDF_TARGET" ]; then
    echo "Erro: IDF_PATH nao definido. Carregue o ambiente ESP-IDF primeiro ou passe o caminho como argumento."
    exit 1
fi

echo "Aplicando patches em: $IDF_TARGET"
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

for patch in $(ls "$SCRIPT_DIR"/*.patch | sort); do
    echo "Aplicando $(basename "$patch")..."
    git -C "$IDF_TARGET" apply "$patch"
done
echo "Concluido!"
