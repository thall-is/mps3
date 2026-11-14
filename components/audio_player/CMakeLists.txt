idf_component_register(
    SRCS
        "audio_player.cpp"
        "metadata_extract.c"
        "webradio.cpp"
        "fs_browser.cpp"
        "user_metrics.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        board_io
        heap
        nvs_flash
        esp_audio_codec
        eq
        esp_http_client
        audio_meter
        sd_card
)

target_compile_options(${COMPONENT_LIB} PRIVATE -O3)



