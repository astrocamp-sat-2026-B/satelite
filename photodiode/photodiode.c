#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"

#define ADC_SPI spi0
#define PIN_MISO 16
#define PIN_CS 17
#define PIN_SCK 18
#define PIN_MOSI 19


void mcp3008_init(void) {
    spi_init(ADC_SPI, 500 * 1000); // 推奨初期値: 500 kHz
    spi_set_format(ADC_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_init(PIN_CS);
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_put(PIN_CS, 1); // 非選択
}

uint16_t mcp3008_read(uint8_t channel) {
    if (channel > 7) return 0xffff; // 範囲外をエラー値にする

    uint8_t tx[3] = {0x01, (uint8_t)((0x08 | channel) << 4), 0x00};
    uint8_t rx[3] = {0};

    gpio_put(PIN_CS, 0);
    spi_write_read_blocking(ADC_SPI, tx, rx, 3);
    gpio_put(PIN_CS, 1);
    return (uint16_t)(((rx[1] & 0x03) << 8) | rx[2]);
}

float code_to_volts(uint16_t code) {
    return (float)code * 3.3f / 1024.0f;
}


int main()
{

    stdio_init_all();
    // PCがUSBシリアルを認識するまで少し待っておこ
    sleep_ms(2000);
    mcp3008_init();

    while (true) {
        printf(
    "1:%.3f V\n"
    "2:%.3f V\n"
    "3:%.3f V\n"
    "4:%.3f V\n\n",
    (double)code_to_volts(mcp3008_read(0)),
    (double)code_to_volts(mcp3008_read(1)),
    (double)code_to_volts(mcp3008_read(2)),
    (double)code_to_volts(mcp3008_read(3))
);
        sleep_ms(1000);

    }
}
