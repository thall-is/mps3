# Componente bitrate_abr
#
# Adaptacao automatica de qualidade do LDAC (HQ->SQ->MQ conforme o link
# aguenta ou nao) - logica pura, sem NENHUMA dependencia de ESP-IDF
# (testavel com gcc puro no host, ver test_bitrate_abr_host.c e
# tests/run_host_tests.sh na raiz do projeto). Codigo 100% novo deste
# projeto.
idf_component_register(
    SRCS "bitrate_abr.c"
    INCLUDE_DIRS "include"
)
