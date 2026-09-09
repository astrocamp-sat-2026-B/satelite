// pico_ap_tcp_client.c
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/rand.h"

#include "hardware/adc.h"

#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#define AP_SSID       "PICO2W_DEMO"
#define AP_PASSWORD   "pico2w-demo"
#define PC_IP         "192.168.4.2"
#define TCP_PORT      4242
#define TELEMETRY_INTERVAL_MS 2000
#define TELEMETRY_LED_ON_MS    200

static struct tcp_pcb *client_pcb = NULL;
static volatile bool tcp_connected = false;
static volatile bool tcp_connecting = false;
static bool telemetry_led_on = false;
static absolute_time_t telemetry_led_off_time;

static err_t send_text(struct tcp_pcb *pcb, const char *text) {
    err_t err = tcp_write(pcb, text, strlen(text), TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        printf("tcp_write failed: %d\n", err);
        return err;
    }
    return tcp_output(pcb);
}

static int read_internal_temperature_centi_c(void) {
    const float conversion_factor = 3.3f / 4095.0f;
    uint16_t raw = adc_read();
    float voltage = (float)raw * conversion_factor;
    float temperature_c = 27.0f - (voltage - 0.706f) / 0.001721f;

    return (int)(temperature_c * 100.0f);
}

static void flash_telemetry_led(void) {
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);
    telemetry_led_on = true;
    telemetry_led_off_time = make_timeout_time_ms(TELEMETRY_LED_ON_MS);
}

static void update_telemetry_led(void) {
    if (telemetry_led_on &&
        absolute_time_diff_us(get_absolute_time(), telemetry_led_off_time) <= 0) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
        telemetry_led_on = false;
    }
}

static void send_telemetry(struct tcp_pcb *pcb) {
    char telemetry[128];
    uint32_t uptime_s = (uint32_t)(to_ms_since_boot(get_absolute_time()) / 1000);
    int temperature_centi_c = read_internal_temperature_centi_c();
    int temperature_fraction = temperature_centi_c >= 0
        ? temperature_centi_c % 100
        : (-temperature_centi_c) % 100;
    uint32_t random_value = get_rand_32() % 1000;

    int length = snprintf(
        telemetry,
        sizeof(telemetry),
        "TELEMETRY,uptime_s=%lu,temp_c=%d.%02d,random=%lu\n",
        (unsigned long)uptime_s,
        temperature_centi_c / 100,
        temperature_fraction,
        (unsigned long)random_value
    );

    if (length < 0 || length >= (int)sizeof(telemetry)) {
        printf("telemetry formatting failed\n");
        return;
    }

    if (send_text(pcb, telemetry) == ERR_OK) {
        flash_telemetry_led();
    }
}

static err_t send_reply(struct tcp_pcb *pcb, const struct pbuf *p) {
    static const char prefix[] = "PICO_REPLY: ";
    char last_char = '\0';

    err_t err = tcp_write(pcb, prefix, sizeof(prefix) - 1, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        return err;
    }

    for (const struct pbuf *q = p; q != NULL; q = q->next) {
        err = tcp_write(pcb, q->payload, q->len, TCP_WRITE_FLAG_COPY);
        if (err != ERR_OK) {
            return err;
        }

        if (q->len > 0) {
            last_char = ((const char *)q->payload)[q->len - 1];
        }
    }

    if (last_char != '\n') {
        err = tcp_write(pcb, "\n", 1, TCP_WRITE_FLAG_COPY);
        if (err != ERR_OK) {
            return err;
        }
    }

    return tcp_output(pcb);
}

static void on_tcp_error(void *arg, err_t err) {
    (void)arg;
    printf("TCP error: %d\n", err);
    client_pcb = NULL;
    tcp_connected = false;
    tcp_connecting = false;
}

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

    printf("PC -> Pico: ");
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        fwrite(q->payload, 1, q->len, stdout);
    }
    printf("\n");

    err_t reply_err = send_reply(pcb, p);
    if (reply_err != ERR_OK) {
        printf("reply failed: %d\n", reply_err);
    }

    pbuf_free(p);
    return ERR_OK;
}

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

    if (cyw43_arch_init()) {
        printf("Wi-Fi initialization failed\n");
        return 1;
    }

    // Pico 2 WをAPとして起動
    cyw43_arch_enable_ap_mode(
        AP_SSID,
        AP_PASSWORD,
        CYW43_AUTH_WPA2_AES_PSK
    );

    adc_init();
    adc_set_temp_sensor_enabled(true);
    adc_select_input(ADC_TEMPERATURE_CHANNEL_NUM);
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
