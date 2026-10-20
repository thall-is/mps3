idf_component_register(SRCS "audio_meter.c"
                       INCLUDE_DIRS "include"
                       REQUIRES "esp_timer" "nvs_flash" "esp_system")

target_compile_options(${COMPONENT_LIB} PRIVATE -O3)


