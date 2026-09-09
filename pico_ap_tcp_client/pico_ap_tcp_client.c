// pico_ap_tcp_client.c
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

#define AP_SSID       "PICOW_DEMO"
#define AP_PASSWORD   "pico-w-demo"
#define PC_IP         "192.168.4.2"
#define TCP_PORT      4242
#define TELEMETRY_INTERVAL_MS 2000
#define TELEMETRY_LED_ON_MS    200

static struct tcp_pcb *client_pcb = NULL;
static volatile bool tcp_connected = false;
static volatile bool tcp_connecting = false;
static bool telemetry_led_on = false;
static absolute_time_t telemetry_led_off_time;
static protocol_receiver_t command_receiver;
static command_state_t command_state;

// 送信
static err_t send_text(struct tcp_pcb *pcb, const char *text) {
    err_t err = tcp_write(pcb, text, strlen(text), TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        printf("tcp_write failed: %d\n", err);
        return err;
    }
    return tcp_output(pcb);
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

    if (!command_handle_line(&command_state, line, reply, sizeof(reply))) {
        printf("command reply formatting failed\n");
        return;
    }

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
        cyw43_arch_poll();
        // poll方式のWi-Fi/lwIP処理を進める。
        cyw43_arch_poll();

        // PCサーバが起動していなければ、1秒ごとに接続を再試行
        if (!tcp_connected && !tcp_connecting) {
            cyw43_arch_lwip_begin();
            connect_to_pc();
            cyw43_arch_lwip_end();
        }

        // Pico -> PC: 2秒ごとに送信
        if (tcp_connected &&
            absolute_time_diff_us(get_absolute_time(), next_telemetry) <= 0) {
            cyw43_arch_lwip_begin();
            send_telemetry(client_pcb);
            cyw43_arch_lwip_end();
            next_telemetry = make_timeout_time_ms(TELEMETRY_INTERVAL_MS);
        }

        update_telemetry_led();
        sleep_ms(10);
    }
}
