# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.1)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico_w CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(blink C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Reusable camera component. Other executables can link camera_component.
add_library(camera_component STATIC camera.c)
pico_generate_pio_header(camera_component ${CMAKE_CURRENT_LIST_DIR}/camera_capture.pio)
target_include_directories(camera_component PUBLIC ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(camera_component PUBLIC
    pico_stdlib
    hardware_i2c
    hardware_pwm
    hardware_pio
    hardware_dma
)
target_compile_options(camera_component PRIVATE -Wall -Wextra)

add_executable(camera camera_app.c)
target_link_libraries(camera PRIVATE camera_component hardware_clocks)
pico_enable_stdio_usb(camera 1)
pico_enable_stdio_uart(camera 0)
target_compile_options(camera PRIVATE -Wall -Wextra)
pico_add_extra_outputs(camera)

# add url via pico_set_program_url
