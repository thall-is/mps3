# Componente sbc_enc
#
# vendor/ e' o codigo-fonte REAL do encoder SBC do Google
# (https://github.com/google/libsbc), baixado sem modificacoes.
# Apache 2.0 - ver vendor/LICENSE.
idf_component_register(
    SRCS "vendor/src/sbc.c" "vendor/src/bits.c" "sbc_enc.c"
    INCLUDE_DIRS "vendor/inc" "include"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-unused-parameter)
