#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "telemetry.h"

#define PROTOCOL_MAX_MESSAGE_LENGTH 384

typedef void (*protocol_line_handler_t)(const char *line, void *context);

typedef struct {
    char line[PROTOCOL_MAX_MESSAGE_LENGTH];
    size_t length;
} protocol_receiver_t;

void protocol_receiver_init(protocol_receiver_t *receiver);
void protocol_receiver_feed(protocol_receiver_t *receiver, const uint8_t *data,
                            size_t length, protocol_line_handler_t handler,
                            void *context);
bool protocol_encode_telemetry(const telemetry_data_t *telemetry, char *message,
                               size_t message_size);

#endif
