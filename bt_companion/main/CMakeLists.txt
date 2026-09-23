idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES uart_ctrl i2s_input bt_source ldac_enc sbc_enc aptx_enc aptx_hd_enc audio_pipeline bitrate_abr nvs_flash driver esp_driver_uart esp_driver_i2s esp_driver_gpio
)
