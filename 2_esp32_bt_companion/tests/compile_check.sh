#!/usr/bin/env bash
# Compila (contra stubs fieis ao ESP-IDF, ver tests/esp_idf_stubs/README.md)
# os arquivos deste projeto que dependem do ESP-IDF e por isso NAO
# entram em tests/run_host_tests.sh (que so' cobre logica pura). Isso
# NAO substitui compilar com o toolchain Xtensa real nem testar contra
# hardware - so' pega uma classe real de erro (sintaxe, tipo, campo
# inexistente) antes disso.
#
# Uso: ./tests/compile_check.sh

set -e
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
STUBS="$ROOT/tests/esp_idf_stubs/include"
FAIL=0

check() {
    local name="$1"; shift
    if gcc -c -I"$STUBS" -Wall -Wextra -Wno-unused-parameter "$@" -o "/tmp/cc_$name.o" 2>"/tmp/cc_$name.log"; then
        local warns
        warns=$(grep -c "warning:" "/tmp/cc_$name.log" || true)
        echo "OK    $name  ($warns warning(s))"
    else
        echo "FALHOU $name"
        cat "/tmp/cc_$name.log"
        FAIL=1
    fi
}

echo "=== compile-check (ESP-IDF stub) ==="

check uart_ctrl \
    -I"$ROOT/components/uart_ctrl/include" \
    "$ROOT/components/uart_ctrl/uart_ctrl.c"

check i2s_input \
    -I"$ROOT/components/i2s_input/include" \
    "$ROOT/components/i2s_input/i2s_input.c"

check bt_source \
    -I"$ROOT/components/bt_source/include" \
    "$ROOT/components/bt_source/bt_source.c"

check main \
    -I"$ROOT/components/uart_ctrl/include" \
    -I"$ROOT/components/i2s_input/include" \
    -I"$ROOT/components/bt_source/include" \
    -I"$ROOT/components/ldac_enc/include" \
    -I"$ROOT/components/sbc_enc/include" \
    -I"$ROOT/components/aptx_enc/include" \
    -I"$ROOT/components/aptx_hd_enc/include" \
    -I"$ROOT/components/audio_pipeline/include" \
    -I"$ROOT/components/bitrate_abr/include" \
    "$ROOT/main/main.c"

echo ""
if [ "$FAIL" -eq 0 ]; then
    echo "TODOS COMPILARAM (contra stub - ver ressalvas em tests/esp_idf_stubs/README.md)"
else
    echo "ALGUM FALHOU - ver acima"
    exit 1
fi
