#include <stdio.h>
#include <string.h>

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "lwip/tcp.h"

#define WIFI_SSID "satelite-ap"
#define WIFI_PASSWORD "pico-wifi"
#define TCP_PORT 5000

static err_t tcp_server_recv(void *arg, struct tcp_pcb *connection,
                             struct pbuf *buffer, err_t error) {
    (void)arg;

    if (buffer == NULL) {
        tcp_close(connection);
        return ERR_OK;
    }

    if (error != ERR_OK) {
        pbuf_free(buffer);
        return error;
    }

    printf("Received %u bytes\n", (unsigned)buffer->tot_len);

    const char command[] = "get_number";
    char response[32];
    int response_length;

    if (buffer->tot_len >= strlen(command) &&
        memcmp(buffer->payload, command, strlen(command)) == 0) {
        response_length = snprintf(response, sizeof(response), "42\n");
    } else {
        response_length = snprintf(response, sizeof(response), "unknown command\n");
    }

    err_t write_error = tcp_write(connection, response, response_length,
                                   TCP_WRITE_FLAG_COPY);
    if (write_error == ERR_OK) {
        tcp_output(connection);
    }

    pbuf_free(buffer);
    return write_error;
}

static err_t tcp_server_accept(void *arg, struct tcp_pcb *connection,
                               err_t error) {
    (void)arg;

    if (error != ERR_OK || connection == NULL) {
        return ERR_VAL;
    }

    printf("PC connected\n");
    tcp_recv(connection, tcp_server_recv);
    return ERR_OK;
}

int main(void) {
    stdio_init_all();
    sleep_ms(2000);

    if (cyw43_arch_init()) {
        printf("Wi-Fi init failed\n");
        return 1;
    }

    cyw43_arch_enable_ap_mode(
        WIFI_SSID,
        WIFI_PASSWORD,
        CYW43_AUTH_WPA2_AES_PSK
    );

    printf("AP started\n");
    printf("SSID: %s\n", WIFI_SSID);
    printf("IP address: 192.168.4.1\n");

    struct tcp_pcb *server = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (server == NULL) {
        printf("Could not create TCP server\n");
        cyw43_arch_deinit();
        return 1;
    }

    if (tcp_bind(server, IP_ANY_TYPE, TCP_PORT) != ERR_OK) {
        printf("Could not bind TCP port %d\n", TCP_PORT);
        tcp_close(server);
        cyw43_arch_deinit();
        return 1;
    }

    server = tcp_listen(server);
    if (server == NULL) {
        printf("Could not listen on TCP port %d\n", TCP_PORT);
        cyw43_arch_deinit();
        return 1;
    }

    tcp_accept(server, tcp_server_accept);
    printf("Waiting for PC on port %d\n", TCP_PORT);

    while (true) {
        cyw43_arch_poll();
        sleep_ms(1);
    }
}
