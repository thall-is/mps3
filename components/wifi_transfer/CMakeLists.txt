idf_component_register(
    SRCS
        "wifi_transfer.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        audio_player
        sd_card
        oled_display
        menu
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        nvs_flash
        mdns
        esp_ringbuf
        eq

)

