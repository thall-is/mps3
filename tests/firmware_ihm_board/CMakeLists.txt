cmake_minimum_required(VERSION 3.22)

# Apenas u8g2 como componente externo; o restante dos drivers I2C compila direto na main
set(EXTRA_COMPONENT_DIRS "../../components/u8g2")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(firmware_ihm_board)
