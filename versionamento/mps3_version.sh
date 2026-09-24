#!/usr/bin/env bash
# ==============================================================================
# mps3_version.sh — Gerenciador de Builds e Flashing de Versões do MPS3 (ESP32-S3)
# ==============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
ESPIDF_DIR="$PROJECT_ROOT/espidf"
BINARIOS_DIR="$SCRIPT_DIR/binarios"
DEFAULT_PORT="COM7"

# Garante que a pasta de binarios exista
mkdir -p "$BINARIOS_DIR"

# Função para garantir que o ambiente ESP-IDF esteja carregado
ensure_idf_env() {
    if ! command -v idf.py &> /dev/null; then
        echo "[*] Ambiente ESP-IDF nao detectado no PATH."
        if [ -f "/c/esp/v6.0.1/esp-idf/export.sh" ]; then
            echo "[*] Carregando /c/esp/v6.0.1/esp-idf/export.sh..."
            # shellcheck disable=SC1091
            . "/c/esp/v6.0.1/esp-idf/export.sh"
        elif [ -f "$IDF_PATH/export.sh" ]; then
            echo "[*] Carregando $IDF_PATH/export.sh..."
            # shellcheck disable=SC1091
            . "$IDF_PATH/export.sh"
        else
            echo "[!] ERRO: Nao foi possivel encontrar o export.sh do ESP-IDF."
            echo "[!] Certifique-se de que o ESP-IDF v6.0.1 esta instalado."
            exit 1
        fi
    fi
}

# Localiza o comando esptool
get_esptool_cmd() {
    if command -v esptool.py &> /dev/null; then
        echo "esptool.py"
    elif [ -f "/c/esp/v6.0.1/esp-idf/components/esptool_py/esptool/esptool.py" ]; then
        echo "python /c/esp/v6.0.1/esp-idf/components/esptool_py/esptool/esptool.py"
    else
        echo "python -m esptool"
    fi
}

# Ação: Compilar e Salvar Nova Versão
cmd_build() {
    ensure_idf_env
    echo "=================================================="
    echo "  COMPILANDO NOVA VERSAO DO MPS3 (ESP32-S3)"
    echo "=================================================="
    
    (
        cd "$ESPIDF_DIR"
        idf.py build
    )

    # Gerar nome da versao: mps3_AAAAMMDD_HHMMSS
    TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
    VERSION_NAME="mps3_${TIMESTAMP}"
    TARGET_DIR="$BINARIOS_DIR/$VERSION_NAME"

    echo "[*] Salvando binarios em: $TARGET_DIR"
    mkdir -p "$TARGET_DIR"

    cp "$ESPIDF_DIR/build/mps3.bin" "$TARGET_DIR/mps3.bin"
    cp "$ESPIDF_DIR/build/bootloader/bootloader.bin" "$TARGET_DIR/bootloader.bin"
    cp "$ESPIDF_DIR/build/partition_table/partition-table.bin" "$TARGET_DIR/partition-table.bin"
    cp "$ESPIDF_DIR/build/flasher_args.json" "$TARGET_DIR/flasher_args.json"
    if [ -f "$ESPIDF_DIR/build/ota_data_initial.bin" ]; then
        cp "$ESPIDF_DIR/build/ota_data_initial.bin" "$TARGET_DIR/ota_data_initial.bin"
    fi

    # Salvar metadados
    COMMIT_HASH="$(git rev-parse --short HEAD 2>/dev/null || echo 'desconhecido')"
    BRANCH_NAME="$(git branch --show-current 2>/dev/null || echo 'desconhecido')"
    cat <<EOF > "$TARGET_DIR/info.txt"
Versao: $VERSION_NAME
Data: $(date '+%Y-%m-%d %H:%M:%S')
Branch: $BRANCH_NAME
Commit: $COMMIT_HASH
Tamanho mps3.bin: $(stat -c%s "$TARGET_DIR/mps3.bin" 2>/dev/null || stat -f%z "$TARGET_DIR/mps3.bin" 2>/dev/null) bytes
EOF

    echo "=================================================="
    echo "  SUCESSO! Versao criada: $VERSION_NAME"
    echo "  Arquivos salvos em: versionamento/binarios/$VERSION_NAME"
    echo "=================================================="
}

# Ação: Gravar pasta específica de binários no ESP32-S3
flash_dir() {
    local DIR_TO_FLASH="$1"
    local PORT="${2:-$DEFAULT_PORT}"

    if [ ! -d "$DIR_TO_FLASH" ]; then
        echo "[!] ERRO: Diretorio nao encontrado: $DIR_TO_FLASH"
        exit 1
    fi

    if [ ! -f "$DIR_TO_FLASH/mps3.bin" ] || [ ! -f "$DIR_TO_FLASH/bootloader.bin" ] || [ ! -f "$DIR_TO_FLASH/partition-table.bin" ]; then
        echo "[!] ERRO: Arquivos de firmware incompletos em $DIR_TO_FLASH"
        exit 1
    fi

    echo "=================================================="
    echo "  GRAVANDO NO ESP32-S3 VIA ESPTOOL"
    echo "  Origem: $(basename "$DIR_TO_FLASH")"
    echo "  Porta:  $PORT | Baud: 460800"
    echo "=================================================="

    ESPTOOL="$(get_esptool_cmd)"

    APP_OFFSET="0x20000"
    OTADATA_ARG=""
    if [ -f "$DIR_TO_FLASH/flasher_args.json" ]; then
        OFFSET_READ=$(python3 -c "import json; print(json.load(open('$DIR_TO_FLASH/flasher_args.json'))['app']['offset'])" 2>/dev/null || python -c "import json; print(json.load(open('$DIR_TO_FLASH/flasher_args.json'))['app']['offset'])" 2>/dev/null)
        if [ -n "$OFFSET_READ" ]; then
            APP_OFFSET="$OFFSET_READ"
        fi
    fi

    if [ -f "$DIR_TO_FLASH/ota_data_initial.bin" ]; then
        OTADATA_ARG="0x10000 $DIR_TO_FLASH/ota_data_initial.bin"
    elif [ -f "$ESPIDF_DIR/build/ota_data_initial.bin" ]; then
        OTADATA_ARG="0x10000 $ESPIDF_DIR/build/ota_data_initial.bin"
    fi

    $ESPTOOL --chip esp32s3 -p "$PORT" -b 460800 \
        --before default-reset --after hard-reset write_flash \
        --flash_mode dio --flash_freq 80m --flash_size 16MB \
        0x0 "$DIR_TO_FLASH/bootloader.bin" \
        0x8000 "$DIR_TO_FLASH/partition-table.bin" \
        $OTADATA_ARG \
        "$APP_OFFSET" "$DIR_TO_FLASH/mps3.bin"

    echo "=================================================="
    echo "  GRAVACAO CONCLUIDA COM SUCESSO!"
    echo "=================================================="
}

# Ação: Listar versões salvas
cmd_list() {
    echo "=================================================="
    echo "  VERSOES SALVAS DO FIRMWARE MPS3"
    echo "=================================================="
    
    local COUNT=0
    for d in $(find "$BINARIOS_DIR" -mindepth 1 -maxdepth 1 -type d | sort); do
        COUNT=$((COUNT + 1))
        local VNAME
        VNAME="$(basename "$d")"
        local DATE_STR="-"
        local COMMIT_STR="-"
        if [ -f "$d/info.txt" ]; then
            DATE_STR="$(grep '^Data:' "$d/info.txt" | cut -d':' -f2- | xargs)"
            COMMIT_STR="$(grep '^Commit:' "$d/info.txt" | cut -d':' -f2- | xargs)"
        fi
        local SZ="-"
        if [ -f "$d/mps3.bin" ]; then
            local BYTES
            BYTES="$(stat -c%s "$d/mps3.bin" 2>/dev/null || stat -f%z "$d/mps3.bin" 2>/dev/null || echo 0)"
            SZ="$((BYTES / 1024)) KB"
        fi
        printf " [%02d] %-25s | Data: %-19s | Commit: %-8s | Tam: %s\n" "$COUNT" "$VNAME" "$DATE_STR" "$COMMIT_STR" "$SZ"
    done

    if [ "$COUNT" -eq 0 ]; then
        echo "  Nenhuma versao encontrada em versionamento/binarios/"
    fi
    echo "=================================================="
}

# Ação: Gravar a versão mais recente
cmd_flash_latest() {
    local PORT="${1:-$DEFAULT_PORT}"
    local LATEST
    LATEST="$(find "$BINARIOS_DIR" -mindepth 1 -maxdepth 1 -type d | sort | tail -n 1)"
    if [ -z "$LATEST" ]; then
        echo "[!] Nenhuma versao salva encontrada para gravar."
        exit 1
    fi
    echo "[*] Versao mais recente identificada: $(basename "$LATEST")"
    flash_dir "$LATEST" "$PORT"
}

# Ação: Gravar a versão anterior (penúltima)
cmd_flash_prev() {
    local PORT="${1:-$DEFAULT_PORT}"
    local PREV
    PREV="$(find "$BINARIOS_DIR" -mindepth 1 -maxdepth 1 -type d | sort | tail -n 2 | head -n 1)"
    if [ -z "$PREV" ]; then
        echo "[!] Nao ha versao anterior salva."
        exit 1
    fi
    echo "[*] Versao anterior identificada: $(basename "$PREV")"
    flash_dir "$PREV" "$PORT"
}

# Ação: Gravar uma versão específica por nome
cmd_flash_version() {
    local VNAME="$1"
    local PORT="${2:-$DEFAULT_PORT}"

    if [ -z "$VNAME" ]; then
        echo "[!] ERRO: Especifique o nome da versao."
        echo "    Exemplo: $0 flash-ver mps3_20260913_011500"
        exit 1
    fi

    local TARGET="$BINARIOS_DIR/$VNAME"
    if [ ! -d "$TARGET" ]; then
        echo "[!] ERRO: Versao '$VNAME' nao encontrada em $BINARIOS_DIR."
        echo "[*] Use '$0 list' para ver as versoes disponiveis."
        exit 1
    fi

    flash_dir "$TARGET" "$PORT"
}

# Ação: Executar upload OTA chamando versionamento/ota_upload.py
ota_upload() {
    local BINARY_PATH="$1"
    local TARGET_IP="$2"

    if [ ! -f "$BINARY_PATH" ]; then
        echo "[!] ERRO: Arquivo de firmware nao encontrado: $BINARY_PATH"
        exit 1
    fi

    local OTA_PY="$SCRIPT_DIR/ota_upload.py"
    if [ ! -f "$OTA_PY" ]; then
        echo "[!] ERRO: Script $OTA_PY nao encontrado!"
        exit 1
    fi

    echo "=================================================="
    echo "  TRANSMISSAO OTA DO FIRMWARE (ESP32-S3)"
    echo "  Arquivo: $(basename "$BINARY_PATH")"
    if [ -n "$TARGET_IP" ]; then
        echo "  Destino: $TARGET_IP"
    else
        echo "  Destino: Auto-descoberta por MAC (28:84:85:52:35:84)"
    fi
    echo "=================================================="

    local PY_CMD="python"
    if ! command -v python &> /dev/null; then
        PY_CMD="python3"
    fi

    if [ -n "$TARGET_IP" ]; then
        $PY_CMD "$OTA_PY" "$BINARY_PATH" "$TARGET_IP"
    else
        $PY_CMD "$OTA_PY" "$BINARY_PATH"
    fi
}

# Ação: Upload OTA da versão mais recente
cmd_ota_latest() {
    local TARGET_IP="$1"
    local LATEST
    LATEST="$(find "$BINARIOS_DIR" -mindepth 1 -maxdepth 1 -type d | sort | tail -n 1)"
    if [ -z "$LATEST" ]; then
        echo "[!] Nenhuma versao salva encontrada para upload OTA."
        exit 1
    fi
    echo "[*] Versao mais recente identificada para OTA: $(basename "$LATEST")"
    ota_upload "$LATEST/mps3.bin" "$TARGET_IP"
}

# Ação: Upload OTA de uma versão específica
cmd_ota_version() {
    local VNAME="$1"
    local TARGET_IP="$2"

    if [ -z "$VNAME" ]; then
        echo "[!] ERRO: Especifique o nome da versao."
        echo "    Exemplo: $0 ota-ver mps3_20260924_140150 192.168.1.100"
        exit 1
    fi

    local TARGET="$BINARIOS_DIR/$VNAME/mps3.bin"
    if [ ! -f "$TARGET" ]; then
        echo "[!] ERRO: Binario '$TARGET' nao encontrado."
        echo "[*] Use '$0 list' para ver as versoes disponiveis."
        exit 1
    fi

    ota_upload "$TARGET" "$TARGET_IP"
}

# Ação: Compilar nova versão e enviar via OTA
cmd_build_ota() {
    local TARGET_IP="$1"
    cmd_build
    local LATEST
    LATEST="$(find "$BINARIOS_DIR" -mindepth 1 -maxdepth 1 -type d | sort | tail -n 1)"
    if [ -z "$LATEST" ]; then
        echo "[!] Nao foi possivel localizar a versao recem-compilada."
        exit 1
    fi
    ota_upload "$LATEST/mps3.bin" "$TARGET_IP"
}

# Menu de Ajuda Completo
cmd_help() {
    cat << 'EOF'
================================================================================
  MPS3 (ESP32-S3) — GERENCIADOR DE VERSIONAMENTO E FLASHING DE BINARIOS
================================================================================

  ESTRUTURA DE DIRETORIOS:
    versionamento/
    ├── binarios/
    │   └── mps3_AAAAMMDD_HHMMSS/    (pasta da versao gerada)
    │       ├── bootloader.bin       (offset 0x0)
    │       ├── partition-table.bin  (offset 0x8000)
    │       ├── mps3.bin             (offset 0x10000 / 0x20000 - firmware principal)
    │       ├── flasher_args.json    (parametros completos do ESP-IDF)
    │       └── info.txt             (data, branch, commit e tamanho)
    ├── ota_upload.py                (script Python para transmissao OTA Wi-Fi)
    ├── mps3_version.sh              (script Bash para Git Bash / Linux)
    └── mps3_version.ps1             (script PowerShell nativo para Windows)

--------------------------------------------------------------------------------
  COMANDOS DISPONIVEIS:
--------------------------------------------------------------------------------
  list
      Lista todas as versoes salvas em binarios/, exibindo nome, data, commit
      Git e tamanho do arquivo mps3.bin.

  build
      Compila o firmware via ESP-IDF v6.0.1 em espidf/, cria uma nova pasta
      em binarios/mps3_AAAAMMDD_HHMMSS e salva todos os binarios gerados com
      metadados (commit, data, tamanho).

  flash-latest [PORTA]  (ou 'flash')
      Identifica a versao mais recente salva em binarios/ e grava diretamente
      no ESP32-S3 via esptool (cabo serial). Se a PORTA nao for informada, usa COM7.

  build-flash [PORTA]
      Executa 'build' (compila e salva versao) e em seguida grava no ESP32-S3 via serial.

  flash-prev [PORTA]  (ou 'flash-previous')
      Identifica a versao anterior (penultima versao salva) e faz o rollback
      gravando-a no ESP32-S3 via serial.

  flash-ver <NOME_DA_VERSAO> [PORTA]
      Grava uma versao especifica informada pelo nome da pasta via serial.
      Exemplo: ./mps3_version.sh flash-ver mps3_20260913_011500 COM7

  ota-latest [IP]  (ou 'ota')
      Envia a versao mais recente de mps3.bin para o ESP32-S3 via Wi-Fi (OTA).
      Se o IP nao for fornecido, localiza automaticamente o dispositivo na rede
      local atraves do endereco MAC de hardware (28:84:85:52:35:84).

  build-ota [IP]
      Compila nova versao com ESP-IDF e realiza imediatamente o upload OTA via Wi-Fi
      com auto-descoberta por MAC (se IP omitido).

  ota-ver <NOME_DA_VERSAO> [IP]
      Envia uma versao salva especifica via OTA para o IP indicado (ou busca por MAC se omitido).
      Exemplo: ./mps3_version.sh ota-ver mps3_20260924_140150 192.168.1.100

  help | -h | --help
      Exibe este menu de ajuda detalhado.

--------------------------------------------------------------------------------
  PARAMETROS DE GRAVACAO SERIAL:
    Chip:       ESP32-S3 (N16R8)
    Baud rate:  460800 bps
    Flash mode: DIO @ 80 MHz | Tamanho: 16 MB
    Offsets:    0x0 (bootloader) | 0x8000 (particoes) | 0x10000 (otadata) | 0x20000 (app mps3)
    Porta:      Padrao: COM7 (substitua passando como argumento se necessario)

--------------------------------------------------------------------------------
  PARAMETROS DE GRAVACAO OTA (WI-FI):
    Protocolo:  HTTP POST /api/ota (application/octet-stream)
    Endpoints:  GET /api/ota (status/bateria/mac) | POST /api/ota (upload firmware)
    MAC Alvo:   28:84:85:52:35:84 (STA) / 28:84:85:52:35:85 (AP)
    Destinos:   Auto-descoberta inteligente por MAC (ARP + ping sweep) ou IP direto

--------------------------------------------------------------------------------
  EXEMPLOS DE USO:
    # 1. Ver todas as versoes disponiveis:
    ./mps3_version.sh list

    # 2. Compilar e salvar nova versao com timestamp atual:
    ./mps3_version.sh build

    # 3. Compilar, salvar e ja gravar na porta COM7 via serial:
    ./mps3_version.sh build-flash COM7

    # 4. Gravar a ultima versao compilada via cabo serial:
    ./mps3_version.sh flash-latest COM7

    # 5. Voltar para a versao anterior (Rollback via serial):
    ./mps3_version.sh flash-prev COM7

    # 6. Gravar uma versao especifica via serial:
    ./mps3_version.sh flash-ver mps3_20260913_011500 COM7

    # 7. Atualizar a versao mais recente via Wi-Fi (OTA):
    ./mps3_version.sh ota
    ./mps3_version.sh ota 192.168.1.100
    ./mps3_version.sh ota-latest 192.168.1.100

    # 8. Compilar e enviar automaticamente via OTA:
    ./mps3_version.sh build-ota 192.168.1.100

    # 9. Enviar versao especifica via OTA:
    ./mps3_version.sh ota-ver mps3_20260924_140150 192.168.1.100
================================================================================
EOF
}

# Dispatcher de comandos
case "$1" in
    build)
        cmd_build
        ;;
    flash-latest|flash)
        cmd_flash_latest "$2"
        ;;
    build-flash)
        cmd_build
        cmd_flash_latest "$2"
        ;;
    flash-prev|flash-previous)
        cmd_flash_prev "$2"
        ;;
    flash-ver|flash-version)
        cmd_flash_version "$2" "$3"
        ;;
    ota-latest|ota)
        cmd_ota_latest "$2"
        ;;
    build-ota)
        cmd_build_ota "$2"
        ;;
    ota-ver|ota-version)
        cmd_ota_version "$2" "$3"
        ;;
    list)
        cmd_list
        ;;
    help|--help|-h|"")
        cmd_help
        ;;
    *)
        echo "[!] Comando desconhecido: $1"
        echo ""
        cmd_help
        exit 1
        ;;
esac


