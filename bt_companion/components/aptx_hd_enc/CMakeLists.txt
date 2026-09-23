# Componente aptx_hd_enc
#
# vendor/ e' o codigo-fonte REAL do encoder aptX HD, liberado pela
# Qualcomm como parte do AOSP em 2023 (ver vendor/PROVENANCE.md).
idf_component_register(
    SRCS "vendor/src/aptXHDbtenc.c" "vendor/src/ProcessSubband.c"
         "vendor/src/QmfConv.c" "vendor/src/QuantiseDifference.c"
         "aptx_hd_enc.c"
    INCLUDE_DIRS "vendor/inc" "include"
)

# Ver a nota identica em components/aptx_enc/CMakeLists.txt - mesmo
# codigo-base, mesma exigencia de -O3 confirmada no host.
target_compile_options(${COMPONENT_LIB} PRIVATE -O3 -Wno-error -Wno-unused-parameter)
