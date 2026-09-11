// main.c
#include <stdbool.h>
#include <stdio.h>
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
#include "rle.h"

#define AP_SSID       "PICOW_DEMO"
#define AP_PASSWORD   "pico-w-demo"
#define PC_IP         "192.168.4.2"
#define TCP_PORT      4242
#define TELEMETRY_INTERVAL_MS 500
#define TELEMETRY_LED_ON_MS    200
#define CAMERA_STREAM_INTERVAL_MS 500
#define CAMERA_STREAM_MIN_INTERVAL_MS 250
#define CAMERA_STREAM_MAX_INTERVAL_MS 10000

static struct tcp_pcb *client_pcb = NULL;
static volatile bool tcp_connected = false;
static volatile bool tcp_connecting = false;
static bool telemetry_led_on = false;
static absolute_time_t telemetry_led_off_time;
static protocol_receiver_t command_receiver;
static command_state_t command_state;
static bool camera_initialized = false;
static volatile int capture_request = 0; /* 1=photo, 2=colour bars */
static bool camera_streaming = false;
static uint32_t camera_stream_interval_ms = CAMERA_STREAM_INTERVAL_MS;
static absolute_time_t next_stream_capture;
#define CAMERA_TRANSFER_CHUNK 4096u
static uint8_t camera_transfer_chunk[CAMERA_TRANSFER_CHUNK];
static struct {
    bool active;
    bool header_queued;
    size_t src_pos; /* bytes of the raw frame already RLE-encoded and sent */
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

static void finish_camera_transfer(void) {
    camera_transfer.active = false;
    if (camera_streaming) {
        next_stream_capture = make_timeout_time_ms(camera_stream_interval_ms);
    }
    printf("Camera frame queued for PC\n");
}

/* Queue a captured frame gradually so the lwIP send buffer is never
 * overrun. The body is RLE-compressed on the fly into a small fixed
 * chunk buffer right before each send, so no second full-frame buffer
 * is needed alongside the raw capture. */
static void pump_camera_transfer(struct tcp_pcb *pcb) {
    if (!camera_transfer.active || pcb == NULL) return;

    if (!camera_transfer.header_queued) {
        size_t remaining = camera_transfer.header_length;
        u16_t available = tcp_sndbuf(pcb);
        if (available == 0 || available < remaining) return;

        err_t err = tcp_write(pcb, camera_transfer.header, (u16_t)remaining,
                              TCP_WRITE_FLAG_COPY);
        if (err == ERR_MEM) return;
        if (err != ERR_OK) {
            printf("camera tcp_write failed: %d\n", err);
            camera_transfer.active = false;
            return;
        }
        camera_transfer.header_queued = true;
        tcp_output(pcb);
        return;
    }

    u16_t available = tcp_sndbuf(pcb);
    if (available == 0) return;
    size_t out_cap = available < CAMERA_TRANSFER_CHUNK ? available
                                                        : CAMERA_TRANSFER_CHUNK;

    size_t next_pos = camera_transfer.src_pos;
    size_t chunk_len = rle_encode_rgb565_chunk(
        camera_get_frame(), camera_get_frame_size(), &next_pos,
        camera_transfer_chunk, out_cap);

    if (chunk_len == 0) {
        if (next_pos >= camera_get_frame_size()) finish_camera_transfer();
        return;
    }

    err_t err = tcp_write(pcb, camera_transfer_chunk, (u16_t)chunk_len,
                          TCP_WRITE_FLAG_COPY);
    if (err == ERR_MEM) return;
    if (err != ERR_OK) {
        printf("camera tcp_write failed: %d\n", err);
        camera_transfer.active = false;
        return;
    }
    camera_transfer.src_pos = next_pos;
    tcp_output(pcb);

    if (camera_transfer.src_pos >= camera_get_frame_size()) {
        finish_camera_transfer();
    }
}

static void process_capture_request(void) {
    int request = capture_request;
    bool stream_frame = false;
    if (!tcp_connected || camera_transfer.active) return;
    if (request == 0 && camera_streaming &&
        absolute_time_diff_us(get_absolute_time(), next_stream_capture) <= 0) {
        request = 1;
        stream_frame = true;
    }
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
        camera_streaming = false;
        return;
    }

    /* The header carries the frame's *decompressed* size and the CRC of
     * the raw pixels, exactly as before RLE was added: the receiver
     * doesn't need to know the compressed length up front, since it
     * just keeps decoding incoming bytes until it has produced that many
     * decompressed bytes. */
    const uint8_t *raw_frame = camera_get_frame();
    camera_transfer.header_length = (size_t)snprintf(
        camera_transfer.header, sizeof(camera_transfer.header),
        "%s,%u,%u,RGB565RLE,%u,%08lx\n",
        stream_frame ? "FRAME_STREAM" : "FRAME",
        CAMERA_WIDTH, CAMERA_HEIGHT, (unsigned int)camera_get_frame_size(),
        (unsigned long)crc32(raw_frame, camera_get_frame_size()));
    camera_transfer.header_queued = false;
    camera_transfer.src_pos = 0;
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

    if (strcmp(line, "STREAM_START") == 0 ||
        strncmp(line, "STREAM_START,", 13) == 0) {
        uint32_t interval = CAMERA_STREAM_INTERVAL_MS;
        if (line[12] == ',') {
            char extra;
            unsigned int parsed;
            if (sscanf(line + 13, "%u%c", &parsed, &extra) != 1 ||
                parsed < CAMERA_STREAM_MIN_INTERVAL_MS ||
                parsed > CAMERA_STREAM_MAX_INTERVAL_MS) {
                send_text(client_pcb,
                          "ERROR,STREAM_INTERVAL,250_TO_10000_MS\n");
                return;
            }
            interval = parsed;
        }
        camera_stream_interval_ms = interval;
        camera_streaming = true;
        next_stream_capture = get_absolute_time();
        char ack[48];
        snprintf(ack, sizeof(ack), "ACK,STREAM_START,%lu\n",
                 (unsigned long)interval);
        send_text(client_pcb, ack);
        return;
    }

    if (strcmp(line, "STREAM_STOP") == 0) {
        camera_streaming = false;
        capture_request = 0;
        send_text(client_pcb, "ACK,STREAM_STOP\n");
        return;
    }

    if (strcmp(line, "CAPTURE") == 0 || strcmp(line, "CAPTURE_TEST") == 0) {
        if (camera_streaming || capture_request != 0 ||
            camera_transfer.active) {
            send_text(client_pcb, "ERROR,CAMERA_BUSY\n");
        } else {
            capture_request = strcmp(line, "CAPTURE_TEST") == 0 ? 2 : 1;
            send_text(client_pcb, "ACK,CAPTURE\n");
        }
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
    camera_streaming = false;
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
        camera_streaming = false;
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
    camera_streaming = true;
    camera_stream_interval_ms = CAMERA_STREAM_INTERVAL_MS;
    next_stream_capture = get_absolute_time();
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
