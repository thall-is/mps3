# Componente bt_source
#
# A2DP source via Bluedroid (codec externo) - registra os 4 SEPs
# (LDAC, aptX HD, aptX, SBC) e envia frames ja codificados. Depende dos
# patches em esp-idf-patches/ (raiz do projeto) pros SEPs vendor (LDAC,
# aptX, aptX HD) - SBC funciona sem patch. Codigo 100% novo deste
# projeto (o algoritmo dos codecs em si esta' nos outros componentes
# *_enc, vendorizado).
idf_component_register(
    SRCS "bt_source.c" "a2dp_media_payload.c"
    INCLUDE_DIRS "include"
    REQUIRES bt
)
