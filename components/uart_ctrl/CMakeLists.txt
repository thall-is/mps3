# Componente uart_ctrl
#
# Protocolo de controle com a placa companheira de Bluetooth (parear,
# conectar, status, taxa de amostragem) com quadros enquadrados e CRC8/Maxim.
idf_component_register(
    SRCS "uart_ctrl.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_uart
)

