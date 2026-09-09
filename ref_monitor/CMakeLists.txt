cmake_minimum_required(VERSION 3.13)

# Raspberry Pi Pico W をターゲットにする
set(PICO_BOARD pico_w CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(ref_monitor C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(ref_monitor
    ref_monitor.c
)

pico_set_program_name(ref_monitor "ref_monitor")
pico_set_program_version(ref_monitor "0.1")

# 出力先はUSBシリアルのみ(stdio_usb)。UART出力は使わない。
pico_enable_stdio_usb(ref_monitor 1)
pico_enable_stdio_uart(ref_monitor 0)

pico_add_extra_outputs(ref_monitor)

target_link_libraries(ref_monitor
    pico_stdlib
    hardware_spi
)
