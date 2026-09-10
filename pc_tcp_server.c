#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

#define TCP_PORT 4242
#define MAX_FRAME_BYTES (1024u * 1024u)

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

static void put_le16(uint8_t *destination, uint16_t value) {
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *destination, uint32_t value) {
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
    destination[2] = (uint8_t)(value >> 16);
    destination[3] = (uint8_t)(value >> 24);
}

static int save_rgb565_bmp(const uint8_t *frame, unsigned width,
                           unsigned height, char *filename,
                           size_t filename_size) {
    SYSTEMTIME now;
    GetLocalTime(&now);
    snprintf(filename, filename_size,
             "camera_%04u%02u%02u_%02u%02u%02u_%03u.bmp",
             now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
             now.wSecond, now.wMilliseconds);

    FILE *file = fopen(filename, "wb");
    if (file == NULL) return 0;

    uint32_t row_size = (width * 3u + 3u) & ~3u;
    uint32_t pixel_bytes = row_size * height;
    uint8_t header[54] = {0};
    header[0] = 'B';
    header[1] = 'M';
    put_le32(header + 2, 54u + pixel_bytes);
    put_le32(header + 10, 54u);
    put_le32(header + 14, 40u);
    put_le32(header + 18, width);
    put_le32(header + 22, height);
    put_le16(header + 26, 1u);
    put_le16(header + 28, 24u);
    put_le32(header + 34, pixel_bytes);
    if (fwrite(header, 1, sizeof(header), file) != sizeof(header)) {
        fclose(file);
        return 0;
    }

    const uint8_t padding[3] = {0};
    for (unsigned output_y = 0; output_y < height; ++output_y) {
        unsigned source_y = height - 1u - output_y;
        for (unsigned x = 0; x < width; ++x) {
            size_t position = ((size_t)source_y * width + x) * 2u;
            uint16_t pixel = ((uint16_t)frame[position] << 8) |
                             frame[position + 1];
            uint8_t bgr[3] = {
                (uint8_t)(((pixel & 0x1fu) * 255u) / 31u),
                (uint8_t)((((pixel >> 5) & 0x3fu) * 255u) / 63u),
                (uint8_t)((((pixel >> 11) & 0x1fu) * 255u) / 31u),
            };
            if (fwrite(bgr, 1, sizeof(bgr), file) != sizeof(bgr)) {
                fclose(file);
                return 0;
            }
        }
        size_t padding_size = row_size - width * 3u;
        if (padding_size > 0 &&
            fwrite(padding, 1, padding_size, file) != padding_size) {
            fclose(file);
            return 0;
        }
    }
    return fclose(file) == 0;
}

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
    uint8_t *frame = NULL;
    size_t frame_size = 0;
    size_t frame_received = 0;
    unsigned frame_width = 0;
    unsigned frame_height = 0;
    uint32_t expected_crc = 0;

    for (;;) {
        int received = recv(client, received_data, sizeof(received_data), 0);
        if (received <= 0) {
            printf("\nPico disconnected\n");
            free(frame);
            return 0;
        }

        for (int i = 0; i < received;) {
            if (frame != NULL) {
                size_t available = (size_t)(received - i);
                size_t needed = frame_size - frame_received;
                size_t copy_size = available < needed ? available : needed;
                memcpy(frame + frame_received, received_data + i, copy_size);
                frame_received += copy_size;
                i += (int)copy_size;

                if (frame_received == frame_size) {
                    uint32_t actual_crc = crc32(frame, frame_size);
                    if (actual_crc != expected_crc) {
                        printf("\nImage CRC mismatch (expected %08lx, got %08lx)\n",
                               (unsigned long)expected_crc,
                               (unsigned long)actual_crc);
                    } else {
                        char filename[MAX_PATH];
                        if (save_rgb565_bmp(frame, frame_width, frame_height,
                                            filename, sizeof(filename))) {
                            printf("\nImage saved: %s\n", filename);
                        } else {
                            printf("\nCould not save image\n");
                        }
                    }
                    free(frame);
                    frame = NULL;
                    frame_received = 0;
                    printf("PC -> Pico > ");
                    fflush(stdout);
                }
                continue;
            }

            char character = received_data[i];
            ++i;

            if (character == '\r') {
                continue;
            }

            if (character == '\n') {
                line[line_length] = '\0';
                unsigned width, height, size;
                unsigned long received_crc;
                char format[16];
                if (sscanf(line, "FRAME,%u,%u,%15[^,],%u,%lx",
                           &width, &height, format, &size, &received_crc) == 5) {
                    if (strcmp(format, "RGB565") != 0 || width != 320u ||
                        height != 240u || size != width * height * 2u ||
                        size > MAX_FRAME_BYTES) {
                        printf("\nInvalid FRAME header: %s\n", line);
                    } else {
                        frame = (uint8_t *)malloc(size);
                        if (frame == NULL) {
                            printf("\nNot enough memory for image\n");
                        } else {
                            frame_width = width;
                            frame_height = height;
                            frame_size = size;
                            frame_received = 0;
                            expected_crc = (uint32_t)received_crc;
                            printf("\nReceiving %ux%u image (%u bytes)...\n",
                                   width, height, size);
                        }
                    }
                } else {
                    display_pico_line(line);
                }
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

    printf("Commands: -100..100=set servo speed, CAPTURE=take photo, "
           "CAPTURE_TEST=colour bars, /quit=exit.\n");

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
