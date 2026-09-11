#include "photodiode.h"

#include "hardware/spi.h"
#include "pico/stdlib.h"

#define PHOTODIODE_SPI spi0
#define PHOTODIODE_PIN_MISO 16
#define PHOTODIODE_PIN_CS   17
#define PHOTODIODE_PIN_SCK  18
#define PHOTODIODE_PIN_MOSI 19
#define MCP3008_SPI_BAUDRATE 500000

static uint16_t mcp3008_read(uint8_t channel) {
    uint8_t tx[3] = {0x01, (uint8_t)((0x08 | channel) << 4), 0x00};
    uint8_t rx[3] = {0};

    gpio_put(PHOTODIODE_PIN_CS, false);
    spi_write_read_blocking(PHOTODIODE_SPI, tx, rx, sizeof(tx));
    gpio_put(PHOTODIODE_PIN_CS, true);

    return (uint16_t)(((rx[1] & 0x03) << 8) | rx[2]);
}

void photodiode_init(void) {
    spi_init(PHOTODIODE_SPI, MCP3008_SPI_BAUDRATE);
    spi_set_format(PHOTODIODE_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(PHOTODIODE_PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PHOTODIODE_PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PHOTODIODE_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_init(PHOTODIODE_PIN_CS);
    gpio_set_dir(PHOTODIODE_PIN_CS, GPIO_OUT);
    gpio_put(PHOTODIODE_PIN_CS, true);
}

void photodiode_read_all(uint16_t values[PHOTODIODE_CHANNEL_COUNT]) {
    for (uint8_t channel = 0; channel < PHOTODIODE_CHANNEL_COUNT; ++channel) {
        values[channel] = mcp3008_read(channel);
    }
}
