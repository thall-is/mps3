# Componente ldac_enc
#
# vendor/ e' o codigo-fonte REAL do encoder LDAC da Sony, baixado sem
# modificacoes de https://github.com/wdv4758h/libldac (mirror nao-oficial
# de https://android.googlesource.com/platform/external/libldac).
# Licenca Apache 2.0 - ver vendor/LICENSE e vendor/NOTICE.
#
# So' entram no build vendor/src/ldaclib.c e vendor/src/ldacBT.c: o
# proprio ldaclib.c faz #include de todos os outros .c do diretorio (e'
# um "unity build" de proposito do projeto original - ver o arquivo pra
# confirmar). NAO adicione os outros .c da pasta vendor/src como fontes
# separadas do componente, ou vai dar multiplas definicoes no linker.
#
# _32BIT_FIXED_POINT: usa o caminho de ponto fixo do encoder (as versoes
# _fixp_ dos arquivos internos), em vez do caminho float. E' o modo que a
# propria Sony recomenda pra CPUs sem unidade de ponto flutuante dedicada
# (ver comentario no Android.bp original) - o caso do nucleo Xtensa LX6
# do ESP32 classico.
idf_component_register(
    SRCS "vendor/src/ldaclib.c" "vendor/src/ldacBT.c" "ldac_enc.c"
    INCLUDE_DIRS "vendor/inc" "include"
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE _32BIT_FIXED_POINT)

# O encoder real da Sony compila limpo com -Wall, mas nao necessariamente
# com -Werror (nao foi escrito visando os warnings especificos do
# toolchain do ESP-IDF) - relaxa so' pra esses arquivos vendorizados, sem
# afetar o resto do projeto.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-unused-parameter)
