# Componente audio_pipeline
#
# Acumulador de PCM + extracao de 16 bits - logica pura, sem NENHUMA
# dependencia de ESP-IDF (testavel com gcc puro no host, ver
# test_audio_pipeline_host.c e tests/run_host_tests.sh na raiz do
# projeto). Codigo 100% novo deste projeto.
idf_component_register(
    SRCS "audio_pipeline.c"
    INCLUDE_DIRS "include"
)
