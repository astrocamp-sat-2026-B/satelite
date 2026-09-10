#include "command.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool parse_speed(const char *text, int32_t *speed) {
    char *end;
    long value = strtol(text, &end, 10);

    if (*text == '\0' || *end != '\0' || value < -100 || value > 100) {
        return false;
    }

    *speed = (int32_t)value;
    return true;
}

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

    if (strncmp(line, set_value_prefix, sizeof(set_value_prefix) - 1) == 0 ||
        *line == '-' || (*line >= '0' && *line <= '9')) {
        const char *speed_text = line;
        int32_t speed;

        if (strncmp(line, set_value_prefix, sizeof(set_value_prefix) - 1) == 0) {
            speed_text = line + sizeof(set_value_prefix) - 1;
        }

        if (parse_speed(speed_text, &speed)) {
            state->value = speed;
            length = snprintf(reply, reply_size, "ACK,SET_VALUE,%ld\n", (long)speed);
        } else {
            length = snprintf(reply, reply_size,
                              "ERROR,INVALID_SPEED,-100_TO_100\n");
        }
    } else if (strcmp(line, "GET_VALUE") == 0) {
        length = snprintf(reply, reply_size, "VALUE,%ld\n", (long)state->value);
    } else {
        /* Compatibility with the original echo demonstration. */
        length = snprintf(reply, reply_size, "PICO_REPLY: %s\n", line);
    }

    return length >= 0 && (size_t)length < reply_size;
}
