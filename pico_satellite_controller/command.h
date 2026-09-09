#ifndef COMMAND_H
#define COMMAND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int32_t value;
} command_state_t;

void command_init(command_state_t *state);
int32_t command_get_value(const command_state_t *state);
bool command_handle_line(command_state_t *state, const char *line,
                         char *reply, size_t reply_size);

#endif
