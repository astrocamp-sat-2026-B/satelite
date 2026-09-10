#ifndef PHOTOREFLECTOR_H
#define PHOTOREFLECTOR_H

#include <stdint.h>

/* Valid MCP3008 readings are 0..1023. */
#define PHOTOREFLECTOR_INVALID 0xffffu

/* Initializes the LBR-127HLD input on the main-board MCP3008 channel 4. */
void photoreflector_init(void);

/* Returns the raw ADC code, or PHOTOREFLECTOR_INVALID on an invalid transfer. */
uint16_t photoreflector_read_raw(void);

#endif
