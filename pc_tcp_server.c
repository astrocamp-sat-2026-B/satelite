#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wlanapi.h>

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "wlanapi.lib")

#define TCP_PORT 4242
#define HTTP_PORT 8080
#define MAX_FRAME_BYTES (1024u * 1024u)
#define AP_SSID "PICOW_DEMO"
#define SIGNAL_UPDATE_INTERVAL_MS 5000

static volatile LONG signal_monitor_running = 1;
static volatile LONG signal_quality_percent = -1;
static volatile LONG signal_last_update_ms = 0;
static volatile LONG http_server_running = 1;

static const char SIGNAL_DASHBOARD_HTML[] =
"<!doctype html><html lang=\"ja\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>Pico W AP Signal</title><style>"
"*{box-sizing:border-box}body{margin:0;background:#09131d;color:#eaf3fa;font-family:system-ui,sans-serif}main{width:min(920px,calc(100% - 28px));margin:36px auto}h1{margin:0;font-size:clamp(1.5rem,4vw,2.2rem)}.sub{color:#9cb0c2;margin:6px 0 24px}.panel{background:#111f2c;border:1px solid #294257;border-radius:14px;padding:20px;box-shadow:0 14px 40px #0005}.status{display:flex;gap:10px;align-items:center;margin-bottom:20px}.dot{width:12px;height:12px;border-radius:50%;background:#77899a}.dot.ok{background:#42d99a;box-shadow:0 0 14px #42d99a}.dot.error{background:#f06778}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:12px}.card{background:#0c1823;border:1px solid #263d50;border-radius:10px;padding:15px}.label{color:#94aabd;font-size:.76rem;letter-spacing:.06em;text-transform:uppercase}.value{font-size:1.75rem;font-weight:700;margin-top:7px;font-variant-numeric:tabular-nums}.meter{height:16px;background:#263746;border-radius:999px;overflow:hidden;margin:20px 0 8px}.fill{width:0;height:100%;background:#77899a;transition:width .5s,background .5s}.scale{display:flex;justify-content:space-between;color:#8499aa;font-size:.72rem}.note{color:#9cb0c2;line-height:1.65;margin:20px 0 0}.note b{color:#dceaf5}@media(max-width:520px){main{margin:20px auto}.panel{padding:15px}.value{font-size:1.45rem}}</style></head><body><main>"
"<h1>Pico W AP signal monitor</h1><p class=\"sub\">Windows WLAN API measurement / updates every 5 seconds</p><section class=\"panel\"><div class=\"status\"><span id=\"dot\" class=\"dot\"></span><strong id=\"status\">Measuring...</strong></div><div class=\"grid\"><article class=\"card\"><div class=\"label\">SSID</div><div id=\"ssid\" class=\"value\">--</div></article><article class=\"card\"><div class=\"label\">Signal quality</div><div id=\"quality\" class=\"value\">-- %</div></article><article class=\"card\"><div class=\"label\">Estimated RSSI</div><div id=\"dbm\" class=\"value\">-- dBm</div></article><article class=\"card\"><div class=\"label\">Assessment</div><div id=\"rating\" class=\"value\">--</div></article></div><div class=\"meter\"><div id=\"fill\" class=\"fill\"></div></div><div class=\"scale\"><span>0% / -100 dBm</span><span>50% / -75 dBm</span><span>100% / -50 dBm</span></div><p id=\"updated\" class=\"note\">Waiting for a sample...</p><p class=\"note\"><b>dBm</b> is an estimate converted from Windows signal quality. A value closer to 0 is stronger. The measurement is the Pico AP signal as received by this Windows PC.</p></section></main><script>"
"const $=id=>document.getElementById(id);function color(q){return q>=80?'#42d99a':q>=60?'#76cf65':q>=40?'#f2c85b':q>=20?'#ef9454':'#f06778'}async function poll(){try{const d=await(await fetch('/api/signal',{cache:'no-store'})).json();$('ssid').textContent=d.ssid;if(!d.available){$('dot').className='dot error';$('status').textContent='AP signal unavailable';$('quality').textContent='-- %';$('dbm').textContent='-- dBm';$('rating').textContent='--';$('fill').style.width='0';$('updated').textContent='Connect this PC to '+d.ssid+' and wait for the next sample.';return}const c=color(d.quality_percent);$('dot').className='dot ok';$('status').textContent='Receiving AP signal measurements';$('quality').textContent=d.quality_percent+' %';$('dbm').textContent=d.estimated_dbm+' dBm';$('rating').textContent=d.rating;$('rating').style.color=c;$('fill').style.width=d.quality_percent+'%';$('fill').style.background=c;$('updated').textContent='Last measurement: '+d.updated_age_s.toFixed(1)+' seconds ago / sampling interval: '+d.sample_interval_s+' seconds'}catch(e){$('dot').className='dot error';$('status').textContent='Signal API connection error'}}poll();setInterval(poll,1000);"
"</script></body></html>";

static const char *signal_rating(DWORD quality) {
    if (quality >= 80) return "Excellent";
    if (quality >= 60) return "Good";
    if (quality >= 40) return "Fair";
    if (quality >= 20) return "Weak";
    return "Very weak";
}

/* Windows reports Wi-Fi signal quality as 0..100. Microsoft documents a
 * linear mapping between -100 dBm and -50 dBm for intermediate values. */
static int quality_to_dbm(DWORD quality) {
    if (quality > 100) quality = 100;
    return (int)(quality / 2) - 100;
}

static DWORD WINAPI monitor_wifi_signal(LPVOID parameter) {
    (void)parameter;
    DWORD negotiated_version;
    HANDLE wlan = NULL;

    if (WlanOpenHandle(2, NULL, &negotiated_version, &wlan) != ERROR_SUCCESS) {
        printf("\nWi-Fi signal monitor unavailable\n");
        return 0;
    }

    while (InterlockedCompareExchange(&signal_monitor_running, 1, 1)) {
        PWLAN_INTERFACE_INFO_LIST interfaces = NULL;
        bool found = false;

        if (WlanEnumInterfaces(wlan, NULL, &interfaces) == ERROR_SUCCESS) {
            for (DWORD i = 0; i < interfaces->dwNumberOfItems; ++i) {
                WLAN_INTERFACE_INFO *interface_info = &interfaces->InterfaceInfo[i];
                if (interface_info->isState != wlan_interface_state_connected) {
                    continue;
                }

                DWORD data_size = 0;
                WLAN_OPCODE_VALUE_TYPE opcode_type;
                PWLAN_CONNECTION_ATTRIBUTES connection = NULL;
                if (WlanQueryInterface(
                        wlan, &interface_info->InterfaceGuid,
                        wlan_intf_opcode_current_connection, NULL, &data_size,
                        (PVOID *)&connection, &opcode_type) != ERROR_SUCCESS) {
                    continue;
                }

                DOT11_SSID *ssid = &connection->wlanAssociationAttributes.dot11Ssid;
                if (ssid->uSSIDLength == strlen(AP_SSID) &&
                    memcmp(ssid->ucSSID, AP_SSID, ssid->uSSIDLength) == 0) {
                    DWORD quality =
                        connection->wlanAssociationAttributes.wlanSignalQuality;
                    InterlockedExchange(&signal_quality_percent, (LONG)quality);
                    InterlockedExchange(&signal_last_update_ms,
                                        (LONG)GetTickCount());
                    printf("\nWi-Fi link: %s, about %d dBm, %lu%% (%s)\n",
                           AP_SSID, quality_to_dbm(quality),
                           (unsigned long)quality, signal_rating(quality));
                    printf("PC -> Pico > ");
                    fflush(stdout);
                    found = true;
                }
                WlanFreeMemory(connection);
                if (found) break;
            }
            WlanFreeMemory(interfaces);
        }

        if (!found) {
            InterlockedExchange(&signal_quality_percent, -1);
            printf("\nWi-Fi link: %s signal unavailable\n", AP_SSID);
            printf("PC -> Pico > ");
            fflush(stdout);
        }

        for (int elapsed = 0;
             elapsed < SIGNAL_UPDATE_INTERVAL_MS &&
             InterlockedCompareExchange(&signal_monitor_running, 1, 1);
             elapsed += 100) {
            Sleep(100);
        }
    }

    WlanCloseHandle(wlan, NULL);
    return 0;
}

static int send_all(SOCKET sock, const char *data, int length);

static SOCKET create_listener(unsigned short port) {
    SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in address;
    int reuse = 1;

    if (server == INVALID_SOCKET) return INVALID_SOCKET;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&reuse, sizeof(reuse));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) ==
            SOCKET_ERROR ||
        listen(server, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(server);
        return INVALID_SOCKET;
    }
    return server;
}

static void http_reply(SOCKET client, const char *content_type,
                       const char *body) {
    char header[256];
    int header_length = snprintf(
        header, sizeof(header),
        "HTTP/1.1 200 OK\r\nContent-Type: %s; charset=utf-8\r\n"
        "Content-Length: %zu\r\nCache-Control: no-store\r\n"
        "Connection: close\r\n\r\n",
        content_type, strlen(body));
    if (header_length > 0 && header_length < (int)sizeof(header)) {
        send_all(client, header, header_length);
        send_all(client, body, (int)strlen(body));
    }
}

static DWORD WINAPI serve_signal_dashboard(LPVOID parameter) {
    SOCKET server = *(SOCKET *)parameter;

    while (InterlockedCompareExchange(&http_server_running, 1, 1)) {
        SOCKET client = accept(server, NULL, NULL);
        if (client == INVALID_SOCKET) break;

        char request[1024];
        int received = recv(client, request, sizeof(request) - 1, 0);
        if (received > 0) {
            request[received] = '\0';
            if (strncmp(request, "GET /api/signal ", 16) == 0) {
                char json[320];
                LONG quality = InterlockedCompareExchange(
                    &signal_quality_percent, 0, 0);
                DWORD updated = (DWORD)InterlockedCompareExchange(
                    &signal_last_update_ms, 0, 0);
                if (quality >= 0) {
                    DWORD age_ms = GetTickCount() - updated;
                    snprintf(json, sizeof(json),
                             "{\"available\":true,\"ssid\":\"%s\","
                             "\"quality_percent\":%ld,\"estimated_dbm\":%d,"
                             "\"rating\":\"%s\",\"updated_age_s\":%.1f,"
                             "\"sample_interval_s\":%.1f}",
                             AP_SSID, quality, quality_to_dbm((DWORD)quality),
                             signal_rating((DWORD)quality), age_ms / 1000.0,
                             SIGNAL_UPDATE_INTERVAL_MS / 1000.0);
                } else {
                    snprintf(json, sizeof(json),
                             "{\"available\":false,\"ssid\":\"%s\","
                             "\"quality_percent\":null,\"estimated_dbm\":null,"
                             "\"rating\":null,\"updated_age_s\":null,"
                             "\"sample_interval_s\":%.1f}",
                             AP_SSID, SIGNAL_UPDATE_INTERVAL_MS / 1000.0);
                }
                http_reply(client, "application/json", json);
            } else {
                http_reply(client, "text/html", SIGNAL_DASHBOARD_HTML);
            }
        }
        closesocket(client);
    }
    return 0;
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
    SOCKET http_listener;
    SOCKET client;
    struct sockaddr_in address;
    char buffer[256];
    HANDLE receive_thread;
    HANDLE signal_thread;
    HANDLE http_thread;

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
    http_listener = create_listener(HTTP_PORT);
    if (http_listener == INVALID_SOCKET) {
        printf("HTTP dashboard bind failed on port %d: %d\n",
               HTTP_PORT, WSAGetLastError());
        closesocket(listener);
        WSACleanup();
        return 1;
    }
    InterlockedExchange(&http_server_running, 1);
    http_thread = CreateThread(NULL, 0, serve_signal_dashboard,
                               &http_listener, 0, NULL);
    InterlockedExchange(&signal_monitor_running, 1);
    signal_thread = CreateThread(NULL, 0, monitor_wifi_signal, NULL, 0, NULL);
    if (http_thread == NULL || signal_thread == NULL) {
        printf("monitor thread creation failed\n");
        InterlockedExchange(&http_server_running, 0);
        InterlockedExchange(&signal_monitor_running, 0);
        closesocket(http_listener);
        if (http_thread != NULL) CloseHandle(http_thread);
        if (signal_thread != NULL) CloseHandle(signal_thread);
        closesocket(listener);
        WSACleanup();
        return 1;
    }
    printf("Signal dashboard: http://localhost:%d\n", HTTP_PORT);
    printf("Waiting on TCP port %d...\n", TCP_PORT);

    client = accept(listener, NULL, NULL);
    if (client == INVALID_SOCKET) {
        printf("accept failed\n");
        InterlockedExchange(&http_server_running, 0);
        InterlockedExchange(&signal_monitor_running, 0);
        closesocket(http_listener);
        WaitForSingleObject(http_thread, 1000);
        WaitForSingleObject(signal_thread, 1000);
        CloseHandle(http_thread);
        CloseHandle(signal_thread);
        closesocket(listener);
        WSACleanup();
        return 1;
    }

    printf("Pico connected\n");
    receive_thread = CreateThread(NULL, 0, receive_from_pico, &client, 0, NULL);
    if (receive_thread == NULL) {
        printf("receive thread creation failed\n");
        InterlockedExchange(&signal_monitor_running, 0);
        if (signal_thread != NULL) {
            WaitForSingleObject(signal_thread, 1000);
            CloseHandle(signal_thread);
        }
        InterlockedExchange(&http_server_running, 0);
        closesocket(http_listener);
        WaitForSingleObject(http_thread, 1000);
        CloseHandle(http_thread);
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
    InterlockedExchange(&signal_monitor_running, 0);
    if (signal_thread != NULL) {
        WaitForSingleObject(signal_thread, 1000);
        CloseHandle(signal_thread);
    }
    InterlockedExchange(&http_server_running, 0);
    closesocket(http_listener);
    WaitForSingleObject(http_thread, 1000);
    CloseHandle(http_thread);
    CloseHandle(receive_thread);
    closesocket(client);
    closesocket(listener);
    WSACleanup();
    return 0;
}
