cmake_minimum_required(VERSION 3.22)

# Build flags migrados de platformio.ini build_flags
# (necessários para o USB Audio Class do TinyUSB)
add_compile_definitions(
    ESP_ES_PARSER_SEARCH_LIMIT=16777216
    CFG_TUD_AUDIO=1
    CFG_TUD_AUDIO_ENABLE_EP_OUT=1
    CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX=384
    CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ=2048
    CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP=0
    CFG_TUD_MSC_EP_BUFSIZE=8192
)

# Componentes customizados vivem na pasta irmã (../components)
set(EXTRA_COMPONENT_DIRS "../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mps3)

