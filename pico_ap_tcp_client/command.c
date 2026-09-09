#include "command.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void command_init(command_state_t *state) {
    state->value = 0;
}

int32_t command_get_value(const command_state_t *state) {
    return state->value;
}

bool command_handle_line(command_state_t *state, const char *line,
                         char *reply, size_t reply_size) {
    static const char set_value_prefix[] = "SET_VALUE,";
    int length;

    if (strncmp(line, set_value_prefix, sizeof(set_value_prefix) - 1) == 0) {
        char *end;
        long value = strtol(line + sizeof(set_value_prefix) - 1, &end, 10);

        if (*end == '\0') {
            state->value = (int32_t)value;
            length = snprintf(reply, reply_size, "ACK,SET_VALUE,%ld\n", value);
        } else {
            length = snprintf(reply, reply_size, "ERROR,INVALID_VALUE\n");
        }
    } else if (strcmp(line, "GET_VALUE") == 0) {
        length = snprintf(reply, reply_size, "VALUE,%ld\n", (long)state->value);
    } else {
        /* Compatibility with the original echo demonstration. */
        length = snprintf(reply, reply_size, "PICO_REPLY: %s\n", line);
    }

    return length >= 0 && (size_t)length < reply_size;
}
