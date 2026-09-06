# Componente i2s_input
#
# Recepcao I2S como slave (o clock vem da placa principal) - ver
# include/i2s_input.h pro formato exato esperado no barramento. Codigo
# 100% novo deste projeto (nao vendorizado).
idf_component_register(
    SRCS "i2s_input.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_i2s
)
