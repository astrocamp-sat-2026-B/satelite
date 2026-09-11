#include "protocol.h"

#include <stdio.h>

void protocol_receiver_init(protocol_receiver_t *receiver) {
    receiver->length = 0;
}

void protocol_receiver_feed(protocol_receiver_t *receiver, const uint8_t *data,
                            size_t length, protocol_line_handler_t handler,
                            void *context) {
    for (size_t i = 0; i < length; ++i) {
        char character = (char)data[i];

        if (character == '\r') {
            continue;
        }

        if (character == '\n') {
            receiver->line[receiver->length] = '\0';
            handler(receiver->line, context);
            receiver->length = 0;
        } else if (receiver->length < sizeof(receiver->line) - 1) {
            receiver->line[receiver->length++] = character;
        } else {
            receiver->length = 0;
        }
    }
}

bool protocol_encode_telemetry(const telemetry_data_t *telemetry, char *message,
                               size_t message_size) {
    int fraction = telemetry->temperature_centi_c >= 0
        ? telemetry->temperature_centi_c % 100
        : (-telemetry->temperature_centi_c) % 100;
    int length;
    char gyro_text[24];
    char gyro_angle_text[24];
    char photoreflector_text[12];

    if (telemetry->gyro_z_valid) {
        int gyro_fraction = telemetry->gyro_z_centi_dps >= 0
            ? telemetry->gyro_z_centi_dps % 100
            : (-telemetry->gyro_z_centi_dps) % 100;
        snprintf(gyro_text, sizeof(gyro_text), "%ld.%02d",
                 (long)(telemetry->gyro_z_centi_dps / 100), gyro_fraction);
    } else {
        snprintf(gyro_text, sizeof(gyro_text), "NA");
    }

    if (telemetry->gyro_z_angle_valid) {
        int gyro_angle_fraction = telemetry->gyro_z_angle_centi_deg >= 0
            ? telemetry->gyro_z_angle_centi_deg % 100
            : (-telemetry->gyro_z_angle_centi_deg) % 100;
        snprintf(gyro_angle_text, sizeof(gyro_angle_text), "%ld.%02d",
                 (long)(telemetry->gyro_z_angle_centi_deg / 100), gyro_angle_fraction);
    } else {
        snprintf(gyro_angle_text, sizeof(gyro_angle_text), "NA");
    }

    if (telemetry->photoreflector_valid) {
        snprintf(photoreflector_text, sizeof(photoreflector_text), "%u",
                 (unsigned int)telemetry->photoreflector_adc);
    } else {
        snprintf(photoreflector_text, sizeof(photoreflector_text), "NA");
    }

    length = snprintf(
        message,
        message_size,
        "TELEMETRY,wifi_mode=AP,uptime_s=%lu,temp_c=%d.%02d,random=%lu,command_value=%ld,gyro_z_dps=%s,gyro_z_angle_deg=%s,photodiode_adc=%u|%u|%u|%u,photoreflector_adc=%s\n",
        (unsigned long)telemetry->uptime_s,
        telemetry->temperature_centi_c / 100,
        fraction,
        (unsigned long)telemetry->random_value,
        (long)telemetry->command_value,
        gyro_text,
        gyro_angle_text,
        (unsigned int)telemetry->photodiode_adc[0],
        (unsigned int)telemetry->photodiode_adc[1],
        (unsigned int)telemetry->photodiode_adc[2],
        (unsigned int)telemetry->photodiode_adc[3],
        photoreflector_text
    );

    return length >= 0 && (size_t)length < message_size;
}
