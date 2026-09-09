#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdio.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

#define TCP_PORT 4242

// 送信
static int send_all(SOCKET sock, const char *data, int length) {
    int total = 0;

    while (total < length) {
        int sent = send(sock, data + total, length - total, 0);
        if (sent == SOCKET_ERROR || sent == 0) {
            return SOCKET_ERROR;
        }
        total += sent;
    }

    return total;
}

// テレメトリ表示
static void display_pico_line(const char *line) {
    if (strncmp(line, "TELEMETRY,", 10) == 0) {
        printf("\nTelemetry <- Pico: %s\n", line + 10);
    } else {
        printf("\nPico -> PC: %s\n", line);
    }

    printf("PC -> Pico > ");
    fflush(stdout);
}

// 受信
static DWORD WINAPI receive_from_pico(LPVOID parameter) {
    SOCKET client = *(SOCKET *)parameter;
    char received_data[128];
    char line[256];
    size_t line_length = 0;

    for (;;) {
        int received = recv(client, received_data, sizeof(received_data), 0);
        if (received <= 0) {
            printf("\nPico disconnected\n");
            return 0;
        }

        for (int i = 0; i < received; i++) {
            char character = received_data[i];

            if (character == '\r') {
                continue;
            }

            if (character == '\n') {
                line[line_length] = '\0';
                display_pico_line(line);
                line_length = 0;
            } else if (line_length < sizeof(line) - 1) {
                line[line_length++] = character;
            } else {
                printf("\nPico message too long; discarded\n");
                line_length = 0;
            }
        }
    }
}

int main(void) {
    WSADATA wsa;
    SOCKET listener;
    SOCKET client;
    struct sockaddr_in address;
    char buffer[256];
    HANDLE receive_thread;

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
    receive_thread = CreateThread(NULL, 0, receive_from_pico, &client, 0, NULL);
    if (receive_thread == NULL) {
        printf("receive thread creation failed\n");
        closesocket(client);
        closesocket(listener);
        WSACleanup();
        return 1;
    }

    printf("Type a message and press Enter. Type /quit to exit.\n");

    for (;;) {
        printf("PC -> Pico > ");
        fflush(stdout);

        if (fgets(buffer, sizeof(buffer), stdin) == NULL) {
            break;
        }

        if (strcmp(buffer, "/quit\n") == 0 ||
            strcmp(buffer, "/quit\r\n") == 0) {
            break;
        }

        if (send_all(client, buffer, (int)strlen(buffer)) == SOCKET_ERROR) {
            printf("send failed: %d\n", WSAGetLastError());
            break;
        }
    }

    shutdown(client, SD_BOTH);
    WaitForSingleObject(receive_thread, INFINITE);
    CloseHandle(receive_thread);
    closesocket(client);
    closesocket(listener);
    WSACleanup();
    return 0;
}
