#include "protocol.h"

#include <stdio.h>

#include "attitude_control.h"

static void format_centi(int32_t value, char *text, size_t text_size) {
    if (value < 0) {
        uint32_t magnitude = (uint32_t)(-(int64_t)value);
        snprintf(text, text_size, "-%lu.%02lu",
                 (unsigned long)(magnitude / 100u),
                 (unsigned long)(magnitude % 100u));
    } else {
        snprintf(text, text_size, "%lu.%02lu",
                 (unsigned long)((uint32_t)value / 100u),
                 (unsigned long)((uint32_t)value % 100u));
    }
}

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
    char roll_text[24];
    char pitch_text[24];
    char photoreflector_text[12];
    char wheel_rpm_text[24];
    char control_target_text[24];
    char control_error_text[24];
    char control_rate_ref_text[24];
    char control_wheel_command_text[24];

    if (telemetry->gyro_z_valid) {
        format_centi(telemetry->gyro_z_centi_dps, gyro_text,
                     sizeof(gyro_text));
    } else {
        snprintf(gyro_text, sizeof(gyro_text), "NA");
    }

    if (telemetry->gyro_z_angle_valid) {
        format_centi(telemetry->gyro_z_angle_centi_deg, gyro_angle_text,
                     sizeof(gyro_angle_text));
    } else {
        snprintf(gyro_angle_text, sizeof(gyro_angle_text), "NA");
    }

    if (telemetry->gyro_z_angle_valid) {
        format_centi(telemetry->roll_centi_deg, roll_text,
                     sizeof(roll_text));
        format_centi(telemetry->pitch_centi_deg, pitch_text,
                     sizeof(pitch_text));
    } else {
        snprintf(roll_text, sizeof(roll_text), "NA");
        snprintf(pitch_text, sizeof(pitch_text), "NA");
    }

    if (telemetry->photoreflector_valid) {
        snprintf(photoreflector_text, sizeof(photoreflector_text), "%u",
                 (unsigned int)telemetry->photoreflector_adc);
    } else {
        snprintf(photoreflector_text, sizeof(photoreflector_text), "NA");
    }

    if (telemetry->wheel_rpm_valid) {
        format_centi(telemetry->wheel_rpm_centi, wheel_rpm_text,
                     sizeof(wheel_rpm_text));
    } else {
        snprintf(wheel_rpm_text, sizeof(wheel_rpm_text), "NA");
    }
    format_centi(telemetry->control_target_centi_deg, control_target_text,
                 sizeof(control_target_text));
    format_centi(telemetry->control_error_centi_deg, control_error_text,
                 sizeof(control_error_text));
    format_centi(telemetry->control_rate_ref_centi_dps, control_rate_ref_text,
                 sizeof(control_rate_ref_text));
    format_centi(telemetry->control_wheel_command_centi_percent,
                 control_wheel_command_text,
                 sizeof(control_wheel_command_text));

    length = snprintf(
        message,
        message_size,
        "TELEMETRY,wifi_mode=AP,uptime_s=%lu,temp_c=%d.%02d,random=%lu,command_value=%ld,gyro_z_dps=%s,gyro_z_angle_deg=%s,roll_deg=%s,pitch_deg=%s,attitude_calibrated=%u,imu_samples=%lu,imu_rejected=%lu,photodiode_adc=%u|%u|%u|%u,photoreflector_adc=%s,wheel_rpm=%s,control_mode=%s,control_fault=%s,control_target_deg=%s,control_error_deg=%s,control_rate_ref_dps=%s,control_wheel_command_percent=%s,control_elapsed_ms=%lu,control_settled_ms=%lu\n",
        (unsigned long)telemetry->uptime_s,
        telemetry->temperature_centi_c / 100,
        fraction,
        (unsigned long)telemetry->random_value,
        (long)telemetry->command_value,
        gyro_text,
        gyro_angle_text,
        roll_text,
        pitch_text,
        telemetry->attitude_calibrated ? 1u : 0u,
        (unsigned long)telemetry->imu_sample_count,
        (unsigned long)telemetry->imu_rejected_samples,
        (unsigned int)telemetry->photodiode_adc[0],
        (unsigned int)telemetry->photodiode_adc[1],
        (unsigned int)telemetry->photodiode_adc[2],
        (unsigned int)telemetry->photodiode_adc[3],
        photoreflector_text,
        wheel_rpm_text,
        attitude_control_mode_name(
            (attitude_control_mode_t)telemetry->control_mode),
        attitude_control_fault_name(
            (attitude_control_fault_t)telemetry->control_fault),
        control_target_text,
        control_error_text,
        control_rate_ref_text,
        control_wheel_command_text,
        (unsigned long)telemetry->control_elapsed_ms,
        (unsigned long)telemetry->control_settled_ms
    );

    return length >= 0 && (size_t)length < message_size;
}
