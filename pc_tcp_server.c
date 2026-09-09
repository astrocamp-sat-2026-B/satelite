#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <stdio.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

#define TCP_PORT 4242

static int send_all(SOCKET sock, const char *data, int length) {
    int total = 0;

    while (total < length) {
        int sent = send(sock, data + total, length - total, 0);
        if (sent == SOCKET_ERROR) {
            return SOCKET_ERROR;
        }
        total += sent;
    }

    return total;
}

int main(void) {
    WSADATA wsa;
    SOCKET listener;
    SOCKET client;
    struct sockaddr_in address;
    char buffer[256];

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return 1;
    }

    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(TCP_PORT);

    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) ==
        SOCKET_ERROR) {
        printf("bind failed: %d\n", WSAGetLastError());
        return 1;
    }

    listen(listener, 1);
    printf("Waiting on TCP port %d...\n", TCP_PORT);

    client = accept(listener, NULL, NULL);
    if (client == INVALID_SOCKET) {
        printf("accept failed\n");
        return 1;
    }

    printf("Pico connected\n");
    send_all(client, "PC_READY\n", 9);

    for (;;) {
        int received = recv(client, buffer, sizeof(buffer) - 1, 0);
        if (received <= 0) {
            break;
        }

        buffer[received] = '\0';
        printf("Pico -> PC: %s", buffer);

        if (send_all(client, "PC_ACK\n", 7) == SOCKET_ERROR) {
            break;
        }
    }

    closesocket(client);
    closesocket(listener);
    WSACleanup();
    return 0;
}