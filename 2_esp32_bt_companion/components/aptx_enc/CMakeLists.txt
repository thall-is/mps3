# Componente aptx_enc
#
# vendor/ e' o codigo-fonte REAL do encoder aptX classico, liberado pela
# Qualcomm como parte do AOSP em 2023 (ver vendor/PROVENANCE.md).
idf_component_register(
    SRCS "vendor/src/aptXbtenc.c" "vendor/src/ProcessSubband.c"
         "vendor/src/QmfConv.c" "vendor/src/QuantiseDifference.c"
         "aptx_enc.c"
    INCLUDE_DIRS "vendor/inc" "include"
)

# IMPORTANTE: este codigo usa varias funcoes "inline" (sem "static") que
# so' funcionam corretamente se o compilador de fato eliminar TODAS as
# chamadas via inlining - caso contrario da' erro de link
# ("undefined reference"). Testado no host: -O2 nao foi suficiente pra
# uma das funcoes (QmfAnalysisFilter, em Qmf.h) resolver assim sem
# gerar link error; -O3 resolveu. Se o toolchain Xtensa do ESP-IDF
# apresentar o mesmo problema com -O2 (o nivel que o CMAKE_BUILD_TYPE
# padrao do projeto usa), suba pra -O3 so' pra este componente como
# feito abaixo.
target_compile_options(${COMPONENT_LIB} PRIVATE -O3 -Wno-error -Wno-unused-parameter)
