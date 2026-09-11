// main.c
#include <stdbool.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/multicore.h"

#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#include "command.h"
#include "protocol.h"
#include "telemetry.h"
#include "icm42688.h"
#include "servo.h"
#include "camera.h"
#include "attitude_control.h"
#include "photodiode.h"
#include "photoreflector.h"
#include "wheel_sensor.h"
#include "sun_capture.h"

#define AP_SSID       "PICOW_DEMO"
#define AP_PASSWORD   "pico-w-demo"
#define PC_IP         "192.168.4.2"
#define TCP_PORT      4242
#define TELEMETRY_INTERVAL_MS 500
#define TELEMETRY_LED_ON_MS    200
#define ATTITUDE_CONTROL_INTERVAL_MS 20
#define WHEEL_SENSOR_INTERVAL_MS 2
#define TEXT_TX_QUEUE_CAPACITY 16u

static struct tcp_pcb *client_pcb = NULL;
static volatile bool tcp_connected = false;
static volatile bool tcp_connecting = false;
static bool telemetry_led_on = false;
static absolute_time_t telemetry_led_off_time;
static protocol_receiver_t command_receiver;
static command_state_t command_state;
static bool camera_initialized = false;
static volatile int capture_request = 0; /* 1=photo, 2=colour bars */

static attitude_control_t attitude_control;
static wheel_sensor_t wheel_sensor;
static absolute_time_t next_attitude_control_update;
static absolute_time_t next_wheel_sensor_update;
static uint64_t last_attitude_control_update_us;
static attitude_control_mode_t reported_control_mode = ATTITUDE_CONTROL_IDLE;
static attitude_control_fault_t reported_control_fault =
    ATTITUDE_CONTROL_FAULT_NONE;
typedef enum {
    CAMERA_TRANSFER_IDLE,
    CAMERA_TRANSFER_SEND_HEADER,
    CAMERA_TRANSFER_SEND_RGB565,
} camera_transfer_stage_t;

static struct {
    bool active;
    camera_transfer_stage_t stage;
    size_t source_position;
    size_t header_length;
    char header[96];
} camera_transfer;

static void trigger_sun_capture_if_needed(void) {
    uint16_t photodiode_adc[PHOTODIODE_CHANNEL_COUNT];
    if (attitude_control_is_active(&attitude_control) ||
        capture_request != 0 || camera_transfer.active || !tcp_connected) {
        return;
    }

    photodiode_read_all(photodiode_adc);
    if (!sun_capture_update(photodiode_adc)) {
        return;
    }

    icm42688_attitude_t attitude;
    if (icm42688_get_attitude(&attitude) && attitude.calibrated &&
        isfinite(attitude.yaw_deg)) {
        /* Keep the sun-detection attitude while the requested photo is taken. */
        attitude_control_hold(&attitude_control, attitude.yaw_deg);
    }
    capture_request = 1;
}

static struct {
    char messages[TEXT_TX_QUEUE_CAPACITY][PROTOCOL_MAX_MESSAGE_LENGTH];
    unsigned head;
    unsigned count;
} text_tx_queue;

static void imu_worker(void) {
    absolute_time_t next = get_absolute_time();
    while (true) {
        next = delayed_by_us(next, ICM42688_SAMPLE_PERIOD_US);
        sleep_until(next);
        icm42688_update();
        if (absolute_time_diff_us(next, get_absolute_time()) >
            (int64_t)ICM42688_SAMPLE_PERIOD_US) {
            next = get_absolute_time();
        }
    }
}

static uint32_t crc32(const uint8_t *data, size_t size) {
    uint32_t crc = 0xffffffffu;
    while (size--) {
        crc ^= *data++;
        for (int i = 0; i < 8; ++i) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

// 送信
static err_t send_text_now(struct tcp_pcb *pcb, const char *text) {
    if (pcb == NULL || text == NULL) return ERR_ARG;
    err_t err = tcp_write(pcb, text, strlen(text), TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        printf("tcp_write failed: %d\n", err);
        return err;
    }
    return tcp_output(pcb);
}

static err_t queue_text(const char *text) {
    size_t length;
    unsigned tail;

    if (text == NULL) return ERR_ARG;
    length = strlen(text);
    if (length >= PROTOCOL_MAX_MESSAGE_LENGTH ||
        text_tx_queue.count >= TEXT_TX_QUEUE_CAPACITY) {
        printf("control message queue full; discarded\n");
        return ERR_MEM;
    }
    tail = (text_tx_queue.head + text_tx_queue.count) % TEXT_TX_QUEUE_CAPACITY;
    memcpy(text_tx_queue.messages[tail], text, length + 1u);
    ++text_tx_queue.count;
    return ERR_OK;
}

/* Text ACKs, telemetry, and image bytes use one TCP connection. Queue text
 * while a raw frame is active so no control line can be inserted in its body. */
static err_t send_text(struct tcp_pcb *pcb, const char *text) {
    if (camera_transfer.active) return queue_text(text);
    return send_text_now(pcb, text);
}

static void flush_text_queue(struct tcp_pcb *pcb) {
    while (!camera_transfer.active && text_tx_queue.count != 0) {
        char *text = text_tx_queue.messages[text_tx_queue.head];
        err_t err = send_text_now(pcb, text);
        if (err != ERR_OK) return;
        text_tx_queue.head = (text_tx_queue.head + 1u) % TEXT_TX_QUEUE_CAPACITY;
        --text_tx_queue.count;
    }
}

static void finish_camera_transfer(void) {
    camera_transfer.active = false;
    camera_transfer.stage = CAMERA_TRANSFER_IDLE;
    printf("Camera frame queued for PC\n");
}

static void fail_camera_transfer(const char *message) {
    camera_transfer.active = false;
    camera_transfer.stage = CAMERA_TRANSFER_IDLE;
    printf("Camera RGB565 transfer failed: %s", message);
    send_text(client_pcb, message);
}

/* The 38,400-byte QQVGA frame is already in the final wire format. Queue as
 * much as lwIP currently accepts and call tcp_output once per poll. COPY is
 * intentional: the next capture may reuse the DMA frame after bytes are
 * queued but before the peer acknowledges them. */
static void pump_camera_transfer(struct tcp_pcb *pcb) {
    if (!camera_transfer.active || pcb == NULL) return;

    if (camera_transfer.stage == CAMERA_TRANSFER_SEND_HEADER) {
        u16_t available = tcp_sndbuf(pcb);
        if (available < camera_transfer.header_length) return;
        err_t err = tcp_write(pcb, camera_transfer.header,
                              (u16_t)camera_transfer.header_length,
                              TCP_WRITE_FLAG_COPY);
        if (err == ERR_MEM) return;
        if (err != ERR_OK) {
            fail_camera_transfer("ERROR,CAMERA_RGB565_HEADER\n");
            return;
        }
        camera_transfer.stage = CAMERA_TRANSFER_SEND_RGB565;
    }

    if (camera_transfer.stage == CAMERA_TRANSFER_SEND_RGB565) {
        const uint8_t *frame = camera_get_frame();
        const size_t frame_size = camera_get_frame_size();
        bool queued_any = false;
        while (camera_transfer.source_position < frame_size) {
            u16_t available = tcp_sndbuf(pcb);
            if (available == 0) break;
            size_t remaining = frame_size - camera_transfer.source_position;
            size_t count = remaining < available ? remaining : available;
            u8_t flags = TCP_WRITE_FLAG_COPY;
            if (count < remaining) flags |= TCP_WRITE_FLAG_MORE;
            err_t err = tcp_write(pcb,
                                  frame + camera_transfer.source_position,
                                  (u16_t)count, flags);
            if (err == ERR_MEM) break;
            if (err != ERR_OK) {
                fail_camera_transfer("ERROR,CAMERA_RGB565_TRANSFER\n");
                return;
            }
            camera_transfer.source_position += count;
            queued_any = true;
        }
        if (queued_any || camera_transfer.source_position == 0) tcp_output(pcb);
        if (camera_transfer.source_position == frame_size) {
            finish_camera_transfer();
        }
    }
}

static void process_capture_request(void) {
    int request = capture_request;
    if (!tcp_connected || camera_transfer.active || text_tx_queue.count != 0) return;
    if (request == 0) return;
    capture_request = 0;

    camera_status_t status = CAMERA_OK;
    if (!camera_initialized) {
        status = camera_init();
        camera_initialized = status == CAMERA_OK;
    }
    if (status == CAMERA_OK) {
        status = camera_set_test_pattern(request == 2);
    }
    if (status == CAMERA_OK) status = camera_capture_frame();

    if (status != CAMERA_OK) {
        char error[96];
        snprintf(error, sizeof(error), "ERROR,CAMERA,%s\n",
                 camera_status_string(status));
        cyw43_arch_lwip_begin();
        send_text(client_pcb, error);
        cyw43_arch_lwip_end();
        return;
    }

    camera_transfer.header_length = (size_t)snprintf(
        camera_transfer.header, sizeof(camera_transfer.header),
        "%s,%u,%u,RGB565,%u,%08lx\n",
        "FRAME", CAMERA_WIDTH, CAMERA_HEIGHT,
        (unsigned int)camera_get_frame_size(),
        (unsigned long)crc32(camera_get_frame(), camera_get_frame_size()));
    if (camera_transfer.header_length == 0 ||
        camera_transfer.header_length >= sizeof(camera_transfer.header)) {
        cyw43_arch_lwip_begin();
        send_text(client_pcb, "ERROR,CAMERA_RGB565_HEADER\n");
        cyw43_arch_lwip_end();
        return;
    }
    camera_transfer.source_position = 0;
    camera_transfer.stage = CAMERA_TRANSFER_SEND_HEADER;
    camera_transfer.active = true;
}

// 温度センサ
// 送信成功LED
static void flash_telemetry_led(void) {
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);
    telemetry_led_on = true;
    telemetry_led_off_time = make_timeout_time_ms(TELEMETRY_LED_ON_MS);
}

// LED点灯状態更新
static void update_telemetry_led(void) {
    if (telemetry_led_on &&
        absolute_time_diff_us(get_absolute_time(), telemetry_led_off_time) <= 0) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
        telemetry_led_on = false;
    }
}

// 疑似テレメトリ作成
static void send_telemetry(struct tcp_pcb *pcb) {
    telemetry_data_t telemetry;
    char message[PROTOCOL_MAX_MESSAGE_LENGTH];

    telemetry_collect(&telemetry, command_get_value(&command_state));
    const attitude_control_status_t *control_status =
        attitude_control_get_status(&attitude_control);
    telemetry.wheel_rpm_valid = wheel_sensor.valid && wheel_sensor.direction != 0;
    telemetry.wheel_rpm_centi = (int32_t)lroundf(
        wheel_sensor_signed_rpm(&wheel_sensor) * 100.0f);
    telemetry.control_mode = (uint8_t)control_status->mode;
    telemetry.control_fault = (uint8_t)control_status->fault;
    telemetry.control_target_centi_deg = (int32_t)lroundf(
        control_status->target_yaw_deg * 100.0f);
    telemetry.control_error_centi_deg = (int32_t)lroundf(
        control_status->angle_error_deg * 100.0f);
    telemetry.control_rate_ref_centi_dps = (int32_t)lroundf(
        control_status->target_rate_dps * 100.0f);
    telemetry.control_wheel_command_centi_percent = (int32_t)lroundf(
        control_status->wheel_command_percent * 100.0f);
    telemetry.control_elapsed_ms = control_status->elapsed_ms;
    telemetry.control_settled_ms = control_status->settled_ms;

    if (!protocol_encode_telemetry(&telemetry, message, sizeof(message))) {
        printf("telemetry formatting failed\n");
        return;
    }

    if (send_text(pcb, message) == ERR_OK) {
        flash_telemetry_led();
    }
}

static bool parse_single_float(const char *text, float *value) {
    char *end;
    if (text == NULL || value == NULL || *text == '\0') return false;
    float parsed = strtof(text, &end);
    if (*end != '\0' || !isfinite(parsed)) return false;
    *value = parsed;
    return true;
}

static void format_centi_value(float value, char *text, size_t text_size) {
    int32_t centi = (int32_t)lroundf(value * 100.0f);
    if (centi < 0) {
        uint32_t magnitude = (uint32_t)(-(int64_t)centi);
        snprintf(text, text_size, "-%lu.%02lu",
                 (unsigned long)(magnitude / 100u),
                 (unsigned long)(magnitude % 100u));
    } else {
        snprintf(text, text_size, "%lu.%02lu",
                 (unsigned long)((uint32_t)centi / 100u),
                 (unsigned long)((uint32_t)centi % 100u));
    }
}

static bool is_manual_speed_command(const char *line) {
    return strncmp(line, "SET_VALUE,", 10) == 0 || *line == '-' ||
           (*line >= '0' && *line <= '9');
}

static void send_control_status(void) {
    const attitude_control_status_t *status =
        attitude_control_get_status(&attitude_control);
    char target[24];
    char error[24];
    char rate[24];
    char integral_rate[24];
    format_centi_value(status->target_yaw_deg, target, sizeof(target));
    format_centi_value(status->angle_error_deg, error, sizeof(error));
    format_centi_value(status->body_rate_dps, rate, sizeof(rate));
    format_centi_value(status->integral_rate_dps, integral_rate,
                       sizeof(integral_rate));
    char reply[320];
    snprintf(reply, sizeof(reply),
             "SLEW_STATUS,%s,%s,target_deg=%s,error_deg=%s,rate_dps=%s,integral_rate_dps=%s,wheel_command=%ld,breakaway=%s,stalled_ms=%lu\n",
             attitude_control_mode_name(status->mode),
             attitude_control_fault_name(status->fault), target, error, rate,
             integral_rate,
             (long)status->servo_command_percent,
             status->breakaway_active ? "ON" : "OFF",
             (unsigned long)status->stalled_ms);
    send_text(client_pcb, reply);
}

static bool start_slew(float requested_angle_deg, bool relative) {
    icm42688_attitude_t attitude;
    if (attitude_control_is_active(&attitude_control)) {
        send_text(client_pcb, "ERROR,SLEW_BUSY\n");
        return false;
    }
    if (camera_transfer.active || capture_request != 0) {
        send_text(client_pcb, "ERROR,CAMERA_BUSY\n");
        return false;
    }
    if (!icm42688_get_attitude(&attitude) || !attitude.calibrated) {
        send_text(client_pcb, "ERROR,ATTITUDE_NOT_CALIBRATED\n");
        return false;
    }
    if (command_get_value(&command_state) != 0) {
        send_text(client_pcb, "ERROR,WHEEL_MANUAL_COMMAND_NOT_ZERO\n");
        return false;
    }
    if (wheel_sensor.valid && wheel_sensor_rpm(&wheel_sensor) > 5.0f) {
        send_text(client_pcb, "ERROR,WHEEL_NOT_STOPPED\n");
        return false;
    }
    if (relative && fabsf(requested_angle_deg) > 180.0f) {
        send_text(client_pcb, "ERROR,RELATIVE_ANGLE,-180_TO_180\n");
        return false;
    }
    if (!relative && fabsf(requested_angle_deg) > 3600.0f) {
        send_text(client_pcb, "ERROR,TARGET_ANGLE,-3600_TO_3600\n");
        return false;
    }

    const float target = relative
        ? attitude.yaw_deg + requested_angle_deg : requested_angle_deg;
    servo_set_speed(0);
    attitude_control_start(&attitude_control, target);
    reported_control_mode = ATTITUDE_CONTROL_SLEW;
    reported_control_fault = ATTITUDE_CONTROL_FAULT_NONE;

    char target_text[24];
    format_centi_value(target, target_text, sizeof(target_text));
    char ack[64];
    snprintf(ack, sizeof(ack), "ACK,SLEW,target_deg=%s\n",
             target_text);
    send_text(client_pcb, ack);
    return true;
}

static void configure_slew(const char *arguments) {
    float angle_gain;
    float rate_gain;
    float wheel_gain;
    float max_rate;
    float max_wheel;
    char extra;
    if (attitude_control_is_active(&attitude_control)) {
        send_text(client_pcb, "ERROR,SLEW_BUSY\n");
        return;
    }
    if (sscanf(arguments, "%f,%f,%f,%f,%f%c", &angle_gain, &rate_gain,
               &wheel_gain, &max_rate, &max_wheel, &extra) != 5 ||
        !isfinite(angle_gain) || !isfinite(rate_gain) ||
        !isfinite(wheel_gain) || !isfinite(max_rate) ||
        !isfinite(max_wheel) || angle_gain < 0.05f || angle_gain > 2.0f ||
        rate_gain < 0.1f || rate_gain > 5.0f ||
        wheel_gain < 0.1f || wheel_gain > 10.0f ||
        max_rate < 0.5f || max_rate > 30.0f ||
        max_wheel < 10.0f || max_wheel > 90.0f) {
        send_text(client_pcb,
                  "ERROR,SLEW_CONFIG,angle_gain=0.05..2,rate_gain=0.1..5,wheel_gain=0.1..10,max_rate=0.5..30,max_wheel=10..90\n");
        return;
    }

    attitude_control.config.angle_gain_per_s = angle_gain;
    attitude_control.config.rate_gain_per_s = rate_gain;
    attitude_control.config.wheel_command_gain = wheel_gain;
    attitude_control.config.max_body_rate_dps = max_rate;
    attitude_control.config.max_wheel_command_percent = max_wheel;
    send_text(client_pcb, "ACK,SLEW_CONFIG\n");
}

static void configure_hold(const char *arguments) {
    float integral_gain;
    float max_integral_rate;
    float integral_zone;
    char extra;
    if (attitude_control_is_active(&attitude_control)) {
        send_text(client_pcb, "ERROR,SLEW_BUSY\n");
        return;
    }
    if (sscanf(arguments, "%f,%f,%f%c", &integral_gain,
               &max_integral_rate, &integral_zone, &extra) != 3 ||
        !isfinite(integral_gain) || !isfinite(max_integral_rate) ||
        !isfinite(integral_zone) ||
        integral_gain < 0.0f || integral_gain > 0.5f ||
        max_integral_rate < 0.0f || max_integral_rate > 5.0f ||
        integral_zone < 1.0f || integral_zone > 45.0f) {
        send_text(client_pcb,
                  "ERROR,HOLD_CONFIG,integral_gain=0..0.5,max_integral_rate=0..5,integral_zone=1..45\n");
        return;
    }

    attitude_control.config.angle_integral_gain_per_s2 = integral_gain;
    attitude_control.config.max_integral_rate_dps = max_integral_rate;
    attitude_control.config.integral_zone_deg = integral_zone;
    send_text(client_pcb, "ACK,HOLD_CONFIG\n");
}

static void configure_breakaway(const char *arguments) {
    float minimum_accel;
    float rate_threshold;
    unsigned long delay_ms;
    char extra;
    if (attitude_control_is_active(&attitude_control)) {
        send_text(client_pcb, "ERROR,SLEW_BUSY\n");
        return;
    }
    if (sscanf(arguments, "%f,%f,%lu%c", &minimum_accel,
               &rate_threshold, &delay_ms, &extra) != 3 ||
        !isfinite(minimum_accel) || !isfinite(rate_threshold) ||
        minimum_accel < 0.0f || minimum_accel > 30.0f ||
        rate_threshold < 0.05f || rate_threshold > 2.0f ||
        delay_ms > 2000u) {
        send_text(client_pcb,
                  "ERROR,BREAKAWAY_CONFIG,min_accel=0..30,rate_threshold=0.05..2,delay_ms=0..2000\n");
        return;
    }

    attitude_control.config.breakaway_min_accel_dps2 = minimum_accel;
    attitude_control.config.breakaway_rate_threshold_dps = rate_threshold;
    attitude_control.config.breakaway_delay_ms = (uint32_t)delay_ms;
    send_text(client_pcb, "ACK,BREAKAWAY_CONFIG\n");
}

static void configure_servo(const char *arguments) {
    unsigned int neutral_us;
    unsigned int deadband_us;
    char extra;
    if (attitude_control_is_active(&attitude_control)) {
        send_text(client_pcb, "ERROR,SLEW_BUSY\n");
        return;
    }
    if (command_get_value(&command_state) != 0) {
        send_text(client_pcb, "ERROR,WHEEL_MANUAL_COMMAND_NOT_ZERO\n");
        return;
    }
    if (sscanf(arguments, "%u,%u%c", &neutral_us, &deadband_us, &extra) != 2 ||
        !servo_configure(neutral_us, deadband_us)) {
        send_text(client_pcb,
                  "ERROR,SERVO_CONFIG,neutral_us=1400..1600,deadband_us=0..200\n");
        return;
    }
    char reply[72];
    snprintf(reply, sizeof(reply), "ACK,SERVO_CONFIG,%lu,%lu\n",
             (unsigned long)servo_get_neutral_pulse_us(),
             (unsigned long)servo_get_deadband_us());
    send_text(client_pcb, reply);
}

// 送信応答
/* This is the boundary between received TCP bytes and application commands. */
static void handle_ground_command(const char *line, void *context) {
    char reply[PROTOCOL_MAX_MESSAGE_LENGTH];
    (void)context;

    printf("PC -> Pico: %s\n", line);

    if (strncmp(line, "SLEW,", 5) == 0 ||
        strncmp(line, "SLEW_REL,", 9) == 0 ||
        strncmp(line, "SLEW_REL_CAPTURE,", 17) == 0) {
        /* The dashboard used SLEW_REL_CAPTURE before automatic post-slew
         * capture was removed. Keep accepting it as a relative slew so an
         * older dashboard still starts the reaction wheel. */
        const bool legacy_relative =
            strncmp(line, "SLEW_REL_CAPTURE,", 17) == 0;
        const bool relative = legacy_relative ||
            strncmp(line, "SLEW_REL,", 9) == 0;
        float angle;
        const char *value = line + (legacy_relative ? 17 : (relative ? 9 : 5));
        if (!parse_single_float(value, &angle)) {
            send_text(client_pcb, "ERROR,INVALID_SLEW_ANGLE\n");
            return;
        }
        start_slew(angle, relative);
        return;
    }

    if (strcmp(line, "SLEW_ABORT") == 0) {
        attitude_control_abort(&attitude_control);
        servo_set_speed(0);
        capture_request = 0;
        send_text(client_pcb, "ACK,SLEW_ABORT\n");
        return;
    }

    if (strcmp(line, "SLEW_STATUS") == 0) {
        send_control_status();
        return;
    }

    if (strncmp(line, "SLEW_CONFIG,", 12) == 0) {
        configure_slew(line + 12);
        return;
    }

    if (strncmp(line, "HOLD_CONFIG,", 12) == 0) {
        configure_hold(line + 12);
        return;
    }

    if (strncmp(line, "BREAKAWAY_CONFIG,", 17) == 0) {
        configure_breakaway(line + 17);
        return;
    }

    if (strncmp(line, "SERVO_CONFIG,", 13) == 0) {
        configure_servo(line + 13);
        return;
    }

    if (strcmp(line, "SERVO_STATUS") == 0) {
        char status[64];
        snprintf(status, sizeof(status), "SERVO_STATUS,neutral_us=%lu,deadband_us=%lu\n",
                 (unsigned long)servo_get_neutral_pulse_us(),
                 (unsigned long)servo_get_deadband_us());
        send_text(client_pcb, status);
        return;
    }

    if (strcmp(line, "ANGLE_RESET") == 0) {
        if (attitude_control_is_active(&attitude_control)) {
            send_text(client_pcb, "ERROR,SLEW_BUSY\n");
            return;
        }
        icm42688_gyro_z_angle_reset();
        send_text(client_pcb, "ACK,ANGLE_RESET\n");
        return;
    }

    if (strcmp(line, "CAPTURE") == 0 || strcmp(line, "CAPTURE_TEST") == 0) {
        const attitude_control_status_t *control_status =
            attitude_control_get_status(&attitude_control);
        if (control_status->mode == ATTITUDE_CONTROL_SLEW ||
            capture_request != 0 ||
            camera_transfer.active) {
            send_text(client_pcb, "ERROR,CAMERA_BUSY\n");
        } else {
            capture_request = strcmp(line, "CAPTURE_TEST") == 0 ? 2 : 1;
            send_text(client_pcb, "ACK,CAPTURE\n");
        }
        return;
    }

    if (strncmp(line, "SET_SUN_THRESHOLD,", 18) == 0) {
        char extra;
        unsigned int threshold;
        if (sscanf(line + 18, "%u%c", &threshold, &extra) != 1 ||
            threshold > SUN_CAPTURE_ADC_MAX) {
            send_text(client_pcb, "ERROR,INVALID_SUN_THRESHOLD,0_TO_1023\n");
            return;
        }
        sun_capture_set_threshold((uint16_t)threshold);
        snprintf(reply, sizeof(reply), "ACK,SET_SUN_THRESHOLD,%u\n",
                 sun_capture_get_threshold());
        send_text(client_pcb, reply);
        return;
    }

    if (strncmp(line, "SET_SUN_TOLERANCE,", 18) == 0) {
        char extra;
        unsigned int tolerance;
        if (sscanf(line + 18, "%u%c", &tolerance, &extra) != 1 ||
            tolerance > SUN_CAPTURE_ADC_MAX) {
            send_text(client_pcb, "ERROR,INVALID_SUN_TOLERANCE,0_TO_1023\n");
            return;
        }
        sun_capture_set_tolerance((uint16_t)tolerance);
        snprintf(reply, sizeof(reply), "ACK,SET_SUN_TOLERANCE,%u\n",
                 sun_capture_get_tolerance());
        send_text(client_pcb, reply);
        return;
    }

    if (strcmp(line, "GET_SUN_CONFIG") == 0) {
        snprintf(reply, sizeof(reply), "SUN_CONFIG,threshold=%u,tolerance=%u\n",
                 sun_capture_get_threshold(), sun_capture_get_tolerance());
        send_text(client_pcb, reply);
        return;
    }

    const bool manual_speed_command = is_manual_speed_command(line);
    if (!command_handle_line(&command_state, line, reply, sizeof(reply))) {
        printf("command reply formatting failed\n");
        return;
    }

    if (manual_speed_command) {
        attitude_control_init(&attitude_control, NULL);
        servo_set_speed(command_get_value(&command_state));
        wheel_sensor_set_direction(&wheel_sensor,
                                   command_get_value(&command_state));
    }

    err_t err = send_text(client_pcb, reply);
    if (err != ERR_OK) {
        printf("command reply failed: %d\n", err);
    }
}

static void sample_wheel_sensor(void) {
    if (!time_reached(next_wheel_sensor_update)) return;
    next_wheel_sensor_update = make_timeout_time_ms(WHEEL_SENSOR_INTERVAL_MS);
    uint16_t raw = photoreflector_read_raw();
    if (raw != PHOTOREFLECTOR_INVALID) {
        wheel_sensor_process(&wheel_sensor, raw,
                             to_us_since_boot(get_absolute_time()));
    }
}

static void update_attitude_control(void) {
    if (!time_reached(next_attitude_control_update)) return;
    const uint64_t now_us = to_us_since_boot(get_absolute_time());
    uint32_t dt_ms = ATTITUDE_CONTROL_INTERVAL_MS;
    if (last_attitude_control_update_us != 0u) {
        uint64_t elapsed_us = now_us - last_attitude_control_update_us;
        dt_ms = (uint32_t)(elapsed_us / 1000u);
        if (dt_ms == 0u) dt_ms = 1u;
    }
    last_attitude_control_update_us = now_us;
    next_attitude_control_update =
        make_timeout_time_ms(ATTITUDE_CONTROL_INTERVAL_MS);

    icm42688_attitude_t attitude;
    const bool valid = icm42688_get_attitude(&attitude) && attitude.calibrated;
    attitude_control_update(&attitude_control, valid,
                            valid ? attitude.yaw_deg : 0.0f,
                            valid ? attitude.vertical_rate_dps : 0.0f,
                            dt_ms);
    const attitude_control_status_t *status =
        attitude_control_get_status(&attitude_control);
    if (attitude_control_is_active(&attitude_control)) {
        servo_set_speed(status->servo_command_percent);
    } else if (status->mode == ATTITUDE_CONTROL_FAULT ||
               status->mode == ATTITUDE_CONTROL_ABORTED) {
        servo_set_speed(0);
    }
    wheel_sensor_set_direction(&wheel_sensor,
                               status->servo_command_percent);

    if ((status->mode != reported_control_mode ||
         status->fault != reported_control_fault) && tcp_connected) {
        char event[96];
        snprintf(event, sizeof(event), "EVENT,SLEW,%s,%s\n",
                 attitude_control_mode_name(status->mode),
                 attitude_control_fault_name(status->fault));
        cyw43_arch_lwip_begin();
        send_text(client_pcb, event);
        cyw43_arch_lwip_end();
        reported_control_mode = status->mode;
        reported_control_fault = status->fault;
    }
}

static void receive_ground_bytes(const uint8_t *data, size_t length, void *context) {
    protocol_receiver_feed(&command_receiver, data, length,
                           handle_ground_command, context);
}

static void on_tcp_error(void *arg, err_t err) {
    (void)arg;
    printf("TCP error: %d\n", err);
    client_pcb = NULL;
    tcp_connected = false;
    tcp_connecting = false;
    capture_request = 0;
    camera_transfer.active = false;
    camera_transfer.stage = CAMERA_TRANSFER_IDLE;
    text_tx_queue.count = 0;
    text_tx_queue.head = 0;
    attitude_control_abort(&attitude_control);
    servo_set_speed(0);
}

// 受け取り
static err_t on_receive(void *arg, struct tcp_pcb *pcb, struct pbuf *p,
                        err_t err) {
    (void)arg;

    if (err != ERR_OK) {
        if (p != NULL) pbuf_free(p);
        printf("receive error: %d\n", err);
        capture_request = 0;
        camera_transfer.active = false;
        attitude_control_abort(&attitude_control);
        servo_set_speed(0);
        return err;
    }

    if (p == NULL) {
        printf("PC disconnected\n");
        client_pcb = NULL;
        tcp_connected = false;
        tcp_connecting = false;
        capture_request = 0;
        camera_transfer.active = false;
        camera_transfer.stage = CAMERA_TRANSFER_IDLE;
        text_tx_queue.count = 0;
        text_tx_queue.head = 0;
        attitude_control_abort(&attitude_control);
        servo_set_speed(0);
        return tcp_close(pcb);
    }

    tcp_recved(pcb, p->tot_len);

    for (struct pbuf *q = p; q != NULL; q = q->next) {
        receive_ground_bytes((const uint8_t *)q->payload, q->len, NULL);
    }

    pbuf_free(p);
    return ERR_OK;
}

// 接続完了
static err_t on_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg;
    tcp_connecting = false;

    if (err != ERR_OK) {
        printf("connect failed: %d\n", err);
        client_pcb = NULL;
        return err;
    }

    client_pcb = pcb;
    tcp_connected = true;
    tcp_recv(pcb, on_receive);

    printf("Connected to PC server\n");
    return send_text(pcb, "PICO_CONNECTED\n");
}

// 接続確立
static void connect_to_pc(void) {
    ip_addr_t pc_address;

    if (!ipaddr_aton(PC_IP, &pc_address)) {
        printf("Invalid PC IP\n");
        return;
    }

    struct tcp_pcb *pcb = tcp_new();
    if (pcb == NULL) {
        printf("TCP PCB allocation failed\n");
        return;
    }

    tcp_arg(pcb, NULL);
    tcp_err(pcb, on_tcp_error);

    err_t err = tcp_connect(pcb, &pc_address, TCP_PORT, on_connected);
    if (err != ERR_OK) {
        printf("tcp_connect failed: %d\n", err);
        tcp_abort(pcb);
        return;
    }

    client_pcb = pcb;
    tcp_connecting = true;
}

int main(void) {
    stdio_init_all();
    sleep_ms(2000);

    protocol_receiver_init(&command_receiver);
    command_init(&command_state);
    sun_capture_init();
    telemetry_init();
    servo_init();
    wheel_sensor_init(&wheel_sensor);
    attitude_control_init(&attitude_control, NULL);

    if (!icm42688_init()) {
        printf("ICM-42688 initialization failed\n");
        return 1;
    }
    multicore_launch_core1(imu_worker);

    if (cyw43_arch_init()) {
        printf("Wi-Fi initialization failed\n");
        return 1;
    }

    // Pico WをAPとして起動
    cyw43_arch_enable_ap_mode(
        AP_SSID,
        AP_PASSWORD,
        CYW43_AUTH_WPA2_AES_PSK
    );

    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);

    printf("AP started: %s\n", AP_SSID);
    printf("Connect PC, set IP to %s, then start its TCP server.\n", PC_IP);

    absolute_time_t next_telemetry = make_timeout_time_ms(TELEMETRY_INTERVAL_MS);
    next_attitude_control_update =
        make_timeout_time_ms(ATTITUDE_CONTROL_INTERVAL_MS);
    next_wheel_sensor_update = make_timeout_time_ms(WHEEL_SENSOR_INTERVAL_MS);

    while (true) {
        sample_wheel_sensor();
        update_attitude_control();
        servo_update();

        // poll方式のWi-Fi/lwIP処理を進める。
        cyw43_arch_poll();

        if (tcp_connected && !camera_transfer.active && text_tx_queue.count != 0) {
            cyw43_arch_lwip_begin();
            flush_text_queue(client_pcb);
            cyw43_arch_lwip_end();
        }

        trigger_sun_capture_if_needed();
        process_capture_request();

        // PCサーバが起動していなければ、1秒ごとに接続を再試行
        if (!tcp_connected && !tcp_connecting) {
            cyw43_arch_lwip_begin();
            connect_to_pc();
            cyw43_arch_lwip_end();
        }

        // Pico -> PC: 0.5秒ごとに送信
        if (tcp_connected && !camera_transfer.active &&
            absolute_time_diff_us(get_absolute_time(), next_telemetry) <= 0) {
            cyw43_arch_lwip_begin();
            send_telemetry(client_pcb);
            cyw43_arch_lwip_end();
            next_telemetry = make_timeout_time_ms(TELEMETRY_INTERVAL_MS);
        }

        if (tcp_connected && camera_transfer.active) {
            cyw43_arch_lwip_begin();
            pump_camera_transfer(client_pcb);
            cyw43_arch_lwip_end();
        }

        update_telemetry_led();
        sleep_ms(2);
    }
}
