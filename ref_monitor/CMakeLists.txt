cmake_minimum_required(VERSION 3.13)

# Pico W を使うので、SDK取り込みより先にボードを指定する。
set(PICO_BOARD pico_w)

include(pico_sdk_import.cmake)

project(ref_monitor C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# ------------------------------------------------------------------
# ビルドの切り替え
#
#   通常 (main.c から部品として使う):
#       cmake -B build
#
#   フォトリフレクタ単体の動作確認:
#       cmake -B build -DPHOTOREFLECTOR_TEST=ON
#
# 切り替えたら build ディレクトリを作り直すか、同じコマンドを
# 再実行してキャッシュを更新すること。
# ------------------------------------------------------------------
option(PHOTOREFLECTOR_TEST "フォトリフレクタ単体の動作確認ビルド" OFF)

if(PHOTOREFLECTOR_TEST)
    # photoreflector.c の中のテスト用 main() を使う。
    # main.c はビルドに含めない (main が二重定義になるため)。
    add_executable(ref_monitor
        photoreflector.c
    )
    target_compile_definitions(ref_monitor PRIVATE PHOTOREFLECTOR_TEST_MAIN)
else()
    # 追加した .c は必ずここに並べる。
    # ヘッダを include しただけでは実体はリンクされず、
    # 「undefined reference」のリンクエラーになる。
    add_executable(ref_monitor
        main.c
        photoreflector.c
    )
endif()

target_include_directories(ref_monitor PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(ref_monitor
    pico_stdlib
    hardware_spi
)

# 出力はUSBシリアルへ。UARTは使わない。
pico_enable_stdio_usb(ref_monitor 1)
pico_enable_stdio_uart(ref_monitor 0)

# .uf2 などを生成する
pico_add_extra_outputs(ref_monitor)
