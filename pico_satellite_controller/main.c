// main.c
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#include "command.h"
#include "protocol.h"
#include "telemetry.h"
#include "icm42688.h"
#include "servo.h"
#include "camera.h"
#include "photodiode.h"
#include "sun_capture.h"

#define AP_SSID       "PICOW_DEMO"
#define AP_PASSWORD   "pico-w-demo"
#define PC_IP         "192.168.4.2"
#define TCP_PORT      4242
#define TELEMETRY_INTERVAL_MS 500
#define TELEMETRY_LED_ON_MS    200

static struct tcp_pcb *client_pcb = NULL;
static volatile bool tcp_connected = false;
static volatile bool tcp_connecting = false;
static bool telemetry_led_on = false;
static absolute_time_t telemetry_led_off_time;
static protocol_receiver_t command_receiver;
static command_state_t command_state;
static bool camera_initialized = false;
static volatile int capture_request = 0; /* 1=photo, 2=colour bars */
static struct {
    bool active;
    bool header_queued;
    size_t offset;
    size_t header_length;
    char header[96];
} camera_transfer;

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
static err_t send_text(struct tcp_pcb *pcb, const char *text) {
    err_t err = tcp_write(pcb, text, strlen(text), TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        printf("tcp_write failed: %d\n", err);
        return err;
    }
    return tcp_output(pcb);
}

/* Queue a captured frame gradually so the lwIP send buffer is never overrun. */
static void pump_camera_transfer(struct tcp_pcb *pcb) {
    if (!camera_transfer.active || pcb == NULL) return;

    const void *data;
    size_t remaining;
    if (!camera_transfer.header_queued) {
        data = camera_transfer.header;
        remaining = camera_transfer.header_length;
    } else {
        data = camera_get_frame() + camera_transfer.offset;
        remaining = camera_get_frame_size() - camera_transfer.offset;
    }

    u16_t available = tcp_sndbuf(pcb);
    if (available == 0) return;
    if (!camera_transfer.header_queued && available < remaining) return;
    size_t chunk = remaining;
    if (chunk > available) chunk = available;
    if (chunk > 4096) chunk = 4096;

    err_t err = tcp_write(pcb, data, (u16_t)chunk, TCP_WRITE_FLAG_COPY);
    if (err == ERR_MEM) return;
    if (err != ERR_OK) {
        printf("camera tcp_write failed: %d\n", err);
        camera_transfer.active = false;
        return;
    }

    if (!camera_transfer.header_queued) {
        camera_transfer.header_queued = true;
    } else {
        camera_transfer.offset += chunk;
        if (camera_transfer.offset == camera_get_frame_size()) {
            camera_transfer.active = false;
            printf("Camera frame queued for PC\n");
        }
    }
    tcp_output(pcb);
}

static void process_capture_request(void) {
    int request = capture_request;
    if (request == 0 || !tcp_connected || camera_transfer.active) return;
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

    const uint8_t *frame = camera_get_frame();
    camera_transfer.header_length = (size_t)snprintf(
        camera_transfer.header, sizeof(camera_transfer.header),
        "FRAME,%u,%u,RGB565,%u,%08lx\n",
        CAMERA_WIDTH, CAMERA_HEIGHT, (unsigned int)camera_get_frame_size(),
        (unsigned long)crc32(frame, camera_get_frame_size()));
    camera_transfer.header_queued = false;
    camera_transfer.offset = 0;
    camera_transfer.active = true;
}

/* 太陽センサーの2chが揃ってしきい値を超えたら自動でCAPTUREをキューイングする。
   撮影中/転送中や手動撮影待ちのときは奪わず、次の周期に譲る。 */
static void process_sun_capture(void) {
    uint16_t photodiode_adc[PHOTODIODE_CHANNEL_COUNT];
    photodiode_read_all(photodiode_adc);

    bool triggered = sun_capture_update(photodiode_adc);
    if (triggered && capture_request == 0 && !camera_transfer.active) {
        capture_request = 1;
        printf("Sun aligned: auto capture queued\n");
    }
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

    if (!protocol_encode_telemetry(&telemetry, message, sizeof(message))) {
        printf("telemetry formatting failed\n");
        return;
    }

    if (send_text(pcb, message) == ERR_OK) {
        flash_telemetry_led();
    }
}

// 送信応答
/* This is the boundary between received TCP bytes and application commands. */
static void handle_ground_command(const char *line, void *context) {
    char reply[PROTOCOL_MAX_MESSAGE_LENGTH];
    (void)context;

    printf("PC -> Pico: %s\n", line);

    if (strcmp(line, "CAPTURE") == 0 || strcmp(line, "CAPTURE_TEST") == 0) {
        if (capture_request != 0 || camera_transfer.active) {
            send_text(client_pcb, "ERROR,CAMERA_BUSY\n");
        } else {
            capture_request = strcmp(line, "CAPTURE_TEST") == 0 ? 2 : 1;
            send_text(client_pcb, "ACK,CAPTURE\n");
        }
        return;
    }

    static const char set_threshold_prefix[] = "SET_SUN_THRESHOLD,";
    static const char set_tolerance_prefix[] = "SET_SUN_TOLERANCE,";

    if (strncmp(line, set_threshold_prefix, sizeof(set_threshold_prefix) - 1) == 0 ||
        strncmp(line, set_tolerance_prefix, sizeof(set_tolerance_prefix) - 1) == 0) {
        bool is_threshold =
            strncmp(line, set_threshold_prefix, sizeof(set_threshold_prefix) - 1) == 0;
        const char *value_text =
            line + (is_threshold ? sizeof(set_threshold_prefix) - 1
                                  : sizeof(set_tolerance_prefix) - 1);
        char *end;
        long value = strtol(value_text, &end, 10);

        if (*value_text == '\0' || *end != '\0' || value < 0 ||
            value > SUN_CAPTURE_ADC_MAX) {
            char error[64];
            snprintf(error, sizeof(error), "ERROR,INVALID_VALUE,0_TO_%u\n",
                     SUN_CAPTURE_ADC_MAX);
            send_text(client_pcb, error);
        } else {
            char ack[64];
            if (is_threshold) {
                sun_capture_set_threshold((uint16_t)value);
                snprintf(ack, sizeof(ack), "ACK,SET_SUN_THRESHOLD,%ld\n", value);
            } else {
                sun_capture_set_tolerance((uint16_t)value);
                snprintf(ack, sizeof(ack), "ACK,SET_SUN_TOLERANCE,%ld\n", value);
            }
            send_text(client_pcb, ack);
        }
        return;
    }

    if (strcmp(line, "GET_SUN_CONFIG") == 0) {
        char status[80];
        snprintf(status, sizeof(status), "SUN_CONFIG,THRESHOLD,%u,TOLERANCE,%u\n",
                 sun_capture_get_threshold(), sun_capture_get_tolerance());
        send_text(client_pcb, status);
        return;
    }

    if (!command_handle_line(&command_state, line, reply, sizeof(reply))) {
        printf("command reply formatting failed\n");
        return;
    }

    servo_set_speed(command_get_value(&command_state));

    err_t err = send_text(client_pcb, reply);
    if (err != ERR_OK) {
        printf("command reply failed: %d\n", err);
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
}

// 受け取り
static err_t on_receive(void *arg, struct tcp_pcb *pcb, struct pbuf *p,
                        err_t err) {
    (void)arg;

    if (err != ERR_OK) {
        if (p != NULL) pbuf_free(p);
        printf("receive error: %d\n", err);
        return err;
    }

    if (p == NULL) {
        printf("PC disconnected\n");
        client_pcb = NULL;
        tcp_connected = false;
        tcp_connecting = false;
        capture_request = 0;
        camera_transfer.active = false;
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
    telemetry_init();
    sun_capture_init();
    servo_init();

    if (!icm42688_init()) {
        printf("ICM-42688 initialization failed\n");
        return 1;
    }

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

    while (true) {
        // poll方式のWi-Fi/lwIP処理を進める。
        cyw43_arch_poll();

        process_sun_capture();
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
        sleep_ms(10);
    }
}
