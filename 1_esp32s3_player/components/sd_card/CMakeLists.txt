idf_component_register(
    SRCS
        "sd_card.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        board_io
        driver
        fatfs
        sdmmc
        vfs
        esp_driver_sdmmc
      
)