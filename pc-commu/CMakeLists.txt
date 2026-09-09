cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico_w CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(pc_commu C CXX ASM)

pico_sdk_init()

add_executable(pc_commu
    pico_ap.c
)

target_include_directories(pc_commu PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(pc_commu
    pico_stdlib
    pico_cyw43_arch_lwip_poll
)

pico_enable_stdio_usb(pc_commu 1)
pico_enable_stdio_uart(pc_commu 0)

pico_add_extra_outputs(pc_commu)
