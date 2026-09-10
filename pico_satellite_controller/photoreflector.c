#include "photoreflector.h"

#include <stdbool.h>

#include "hardware/spi.h"
#include "pico/stdlib.h"

#define PHOTOREFLECTOR_SPI spi0
#define PHOTOREFLECTOR_PIN_MISO 16
#define PHOTOREFLECTOR_PIN_CS   17
#define PHOTOREFLECTOR_PIN_SCK  18
#define PHOTOREFLECTOR_PIN_MOSI 19
#define PHOTOREFLECTOR_SPI_BAUDRATE 1000000
#define PHOTOREFLECTOR_ADC_CHANNEL 4

static bool initialized;

void photoreflector_init(void) {
    spi_init(PHOTOREFLECTOR_SPI, PHOTOREFLECTOR_SPI_BAUDRATE);
    spi_set_format(PHOTOREFLECTOR_SPI, 8, SPI_CPOL_0, SPI_CPHA_0,
                   SPI_MSB_FIRST);
    gpio_set_function(PHOTOREFLECTOR_PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PHOTOREFLECTOR_PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PHOTOREFLECTOR_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_init(PHOTOREFLECTOR_PIN_CS);
    gpio_set_dir(PHOTOREFLECTOR_PIN_CS, GPIO_OUT);
    gpio_put(PHOTOREFLECTOR_PIN_CS, true);
    initialized = true;
}

uint16_t photoreflector_read_raw(void) {
    if (!initialized) return PHOTOREFLECTOR_INVALID;

    uint8_t tx[3] = {
        0x01,
        (uint8_t)(0x80 | (PHOTOREFLECTOR_ADC_CHANNEL << 4)),
        0x00,
    };
    uint8_t rx[3] = {0};

    gpio_put(PHOTOREFLECTOR_PIN_CS, false);
    spi_write_read_blocking(PHOTOREFLECTOR_SPI, tx, rx, sizeof(tx));
    gpio_put(PHOTOREFLECTOR_PIN_CS, true);

    /* The MCP3008 null bit must be zero; a one indicates bad framing. */
    if (rx[1] & 0x04) return PHOTOREFLECTOR_INVALID;
    return (uint16_t)(((rx[1] & 0x03) << 8) | rx[2]);
}
