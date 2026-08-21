cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(plutopack_mcu C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# PlutoPack MCU-interface firmware: SPI0-slave link to the Zynq + display
# control lines + local UI (buttons/encoder) and sensors (fuel gauge, touch).
add_executable(plutopack_mcu
    plutopack_mcu.c
)
target_link_libraries(plutopack_mcu
    pico_stdlib
    hardware_i2c
    hardware_adc
    hardware_spi
    hardware_dma
    hardware_pio
)
pico_enable_stdio_usb(plutopack_mcu 1)
pico_enable_stdio_uart(plutopack_mcu 0)
pico_add_extra_outputs(plutopack_mcu)
