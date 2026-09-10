#ifndef PHOTODIODE_H
#define PHOTODIODE_H

#include <stdint.h>

#define PHOTODIODE_CHANNEL_COUNT 4

/* Initializes SPI0 and the MCP3008 used by the photodiodes. */
void photodiode_init(void);

/* Reads MCP3008 channel 0 through 3 into 10-bit ADC codes. */
void photodiode_read_all(uint16_t values[PHOTODIODE_CHANNEL_COUNT]);

#endif
