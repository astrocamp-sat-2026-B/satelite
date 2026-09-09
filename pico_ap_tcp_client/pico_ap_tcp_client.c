// pico_ap_tcp_client.c
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#define AP_SSID       "PICO2W_DEMO"
#define AP_PASSWORD   "pico2w-demo"
#define PC_IP         "192.168.4.2"
#define TCP_PORT      4242

static struct tcp_pcb *client_pcb = NULL;
static volatile bool tcp_connected = false;
static volatile bool tcp_connecting = false;

static err_t send_text(struct tcp_pcb *pcb, const char *text) {
    err_t err = tcp_write(pcb, text, strlen(text), TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        printf("tcp_write failed: %d\n", err);
        return err;
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

    printf("AP started: %s\n", AP_SSID);
    printf("Connect PC, set IP to %s, then start its TCP server.\n", PC_IP);

    absolute_time_t next_send = make_timeout_time_ms(2000);

    while (true) {
        // PCサーバが起動していなければ、1秒ごとに接続を再試行
        if (!tcp_connected && !tcp_connecting) {
            cyw43_arch_lwip_begin();
            connect_to_pc();
            cyw43_arch_lwip_end();
        }

        // Pico -> PC: 2秒ごとに送信
        if (tcp_connected &&
            absolute_time_diff_us(get_absolute_time(), next_send) <= 0) {
            cyw43_arch_lwip_begin();
            send_text(client_pcb, "PICO_TICK\n");
            cyw43_arch_lwip_end();

            next_send = make_timeout_time_ms(2000);
        }

        sleep_ms(1000);
    }
}