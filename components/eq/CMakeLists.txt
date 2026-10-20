idf_component_register(
    SRCS "eq.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        nvs_flash
        heap
        esp_timer
)

target_compile_options(${COMPONENT_LIB} PRIVATE -O3 -ffast-math)

