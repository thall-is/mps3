#!/usr/bin/env bash
# Roda todo teste deste projeto que NAO precisa do toolchain ESP-IDF nem
# de hardware real - so' precisa de gcc. Cobre: os 4 encoders de verdade
# (LDAC/Sony, SBC/Google, aptX e aptX HD/Qualcomm - todos vendorizados,
# nenhum reescrito), o empacotador de payload A2DP (incluindo a
# variante RTP-only do aptX HD), o acumulador/extracao de PCM, e um
# teste de integracao que liga os pedacos.
#
# O que isso NAO cobre (precisa de hardware real - ver README, secao
# "O que esta escrito mas NAO testado"): uart_ctrl, i2s_input,
# bt_source.c, main.c - esses usam driver/uart.h, driver/i2s_std.h,
# esp_bt*.h, que so' existem dentro do ESP-IDF.
#
# Uso: ./tests/run_host_tests.sh

set -e
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
BIN=/tmp/bt_companion_host_tests
mkdir -p "$BIN"

CFLAGS="-Wall -Wextra -Wno-unused-parameter"
FAIL=0

run_one() {
    local name="$1"
    echo ""
    echo "=== $name ==="
    if "$BIN/$name"; then
        echo "--- $name: PASSOU ---"
    else
        echo "--- $name: FALHOU ---"
        FAIL=1
    fi
}

echo "Compilando..."

gcc $CFLAGS \
    -I"$ROOT/components/bitrate_abr/include" \
    "$ROOT/components/bitrate_abr/bitrate_abr.c" \
    "$ROOT/components/bitrate_abr/test_bitrate_abr_host.c" \
    -o "$BIN/bitrate_abr_test"

gcc $CFLAGS -D_32BIT_FIXED_POINT \
    -I"$ROOT/components/ldac_enc/vendor/inc" \
    -I"$ROOT/components/ldac_enc/vendor/src" \
    -I"$ROOT/components/ldac_enc/include" \
    "$ROOT/components/ldac_enc/vendor/src/ldaclib.c" \
    "$ROOT/components/ldac_enc/vendor/src/ldacBT.c" \
    "$ROOT/components/ldac_enc/ldac_enc.c" \
    "$ROOT/components/ldac_enc/test_host.c" \
    -lm -o "$BIN/ldac_enc_test"

gcc $CFLAGS \
    -I"$ROOT/components/bt_source/include" \
    "$ROOT/components/bt_source/a2dp_media_payload.c" \
    "$ROOT/components/bt_source/test_media_payload_host.c" \
    -o "$BIN/payload_test"

gcc $CFLAGS \
    -I"$ROOT/components/audio_pipeline/include" \
    "$ROOT/components/audio_pipeline/audio_pipeline.c" \
    "$ROOT/components/audio_pipeline/test_audio_pipeline_host.c" \
    -o "$BIN/audio_pipeline_test"

gcc $CFLAGS \
    -I"$ROOT/components/sbc_enc/vendor/inc" \
    -I"$ROOT/components/sbc_enc/vendor/src" \
    -I"$ROOT/components/sbc_enc/include" \
    "$ROOT/components/sbc_enc/vendor/src/sbc.c" \
    "$ROOT/components/sbc_enc/vendor/src/bits.c" \
    "$ROOT/components/sbc_enc/sbc_enc.c" \
    "$ROOT/components/sbc_enc/test_host.c" \
    -lm -o "$BIN/sbc_enc_test"

gcc -O3 -Wall -Wextra -Wno-unused-parameter \
    -I"$ROOT/components/aptx_enc/vendor/inc" \
    -I"$ROOT/components/aptx_enc/vendor/src" \
    -I"$ROOT/components/aptx_enc/include" \
    "$ROOT/components/aptx_enc/vendor/src/aptXbtenc.c" \
    "$ROOT/components/aptx_enc/vendor/src/ProcessSubband.c" \
    "$ROOT/components/aptx_enc/vendor/src/QmfConv.c" \
    "$ROOT/components/aptx_enc/vendor/src/QuantiseDifference.c" \
    "$ROOT/components/aptx_enc/aptx_enc.c" \
    "$ROOT/components/aptx_enc/test_host.c" \
    -lm -o "$BIN/aptx_enc_test"

gcc -O3 -Wall -Wextra -Wno-unused-parameter \
    -I"$ROOT/components/aptx_hd_enc/vendor/inc" \
    -I"$ROOT/components/aptx_hd_enc/vendor/src" \
    -I"$ROOT/components/aptx_hd_enc/include" \
    "$ROOT/components/aptx_hd_enc/vendor/src/aptXHDbtenc.c" \
    "$ROOT/components/aptx_hd_enc/vendor/src/ProcessSubband.c" \
    "$ROOT/components/aptx_hd_enc/vendor/src/QmfConv.c" \
    "$ROOT/components/aptx_hd_enc/vendor/src/QuantiseDifference.c" \
    "$ROOT/components/aptx_hd_enc/aptx_hd_enc.c" \
    "$ROOT/components/aptx_hd_enc/test_host.c" \
    -lm -o "$BIN/aptx_hd_enc_test"

gcc $CFLAGS -D_32BIT_FIXED_POINT \
    -I"$ROOT/components/ldac_enc/vendor/inc" \
    -I"$ROOT/components/ldac_enc/vendor/src" \
    -I"$ROOT/components/ldac_enc/include" \
    -I"$ROOT/components/bt_source/include" \
    -I"$ROOT/components/audio_pipeline/include" \
    "$ROOT/components/ldac_enc/vendor/src/ldaclib.c" \
    "$ROOT/components/ldac_enc/vendor/src/ldacBT.c" \
    "$ROOT/components/ldac_enc/ldac_enc.c" \
    "$ROOT/components/bt_source/a2dp_media_payload.c" \
    "$ROOT/components/audio_pipeline/audio_pipeline.c" \
    "$ROOT/tests/integration_pipeline_host.c" \
    -lm -o "$BIN/integration_pipeline_host"

echo "Compilado. Rodando..."

run_one ldac_enc_test
run_one payload_test
run_one audio_pipeline_test
run_one sbc_enc_test
run_one aptx_enc_test
run_one aptx_hd_enc_test
run_one bitrate_abr_test
run_one integration_pipeline_host

echo ""
if [ "$FAIL" -eq 0 ]; then
    echo "===================================="
    echo "TODOS OS TESTES DE HOST PASSARAM"
    echo "===================================="
else
    echo "===================================="
    echo "ALGUM TESTE FALHOU - ver acima"
    echo "===================================="
    exit 1
fi
