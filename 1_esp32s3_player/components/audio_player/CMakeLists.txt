idf_component_register(
    SRCS
        "audio_player.cpp"
        "metadata_extract.c"
        "webradio.cpp"
        "fs_browser.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        board_io
        heap
        nvs_flash
        esp_audio_codec
        eq
        esp_http_client
)


