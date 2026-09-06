# Componente bt_link
#
# Ponte entre esta placa e o companheiro de Bluetooth (projeto
# bt_companion, ESP32 classico separado) - fala o protocolo de
# uart_ctrl e se registra no menu principal. Ver include/bt_link.h.
idf_component_register(
    SRCS "bt_link.c"
    INCLUDE_DIRS "include"
    REQUIRES uart_ctrl menu oled_display board_io driver esp_driver_gpio
)

