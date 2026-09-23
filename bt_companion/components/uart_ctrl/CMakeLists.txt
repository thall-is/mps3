# Componente uart_ctrl
#
# Protocolo de controle com a placa principal (parear, conectar,
# status, qualidade, taxa de amostragem) - ver include/uart_ctrl.h pro
# formato do quadro. Codigo 100% novo deste projeto (nao vendorizado).
idf_component_register(
    SRCS "uart_ctrl.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_uart
)
