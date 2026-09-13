idf_component_register(
    SRCS "usb_manager.c" "usb_descriptors.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer sdmmc fatfs audio_player board_io freertos touch_input sd_card eq
)
