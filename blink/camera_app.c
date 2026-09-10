// USB CDC command-line wrapper for the reusable camera component.
#include <stdio.h>

#include "camera.h"
#include "hardware/clocks.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"

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

static void print_error(camera_status_t status) {
    printf("ERR %s\n", camera_status_string(status));
}

int main(void) {
    stdio_init_all();

    while (true) {
        int cmd = getchar_timeout_us(100000);
        if (cmd == 'I') {
            camera_status_t status = camera_init();
            uint8_t pid, ver;
            camera_get_sensor_id(&pid, &ver);
            if (status == CAMERA_OK) {
                printf("INFO sensor PID=%02x VER=%02x\n", pid, ver);
                printf("OK OV7675 %u %u RGB565 XCLK=%lu\n",
                       CAMERA_WIDTH, CAMERA_HEIGHT,
                       (unsigned long)(clock_get_hz(clk_sys) / 10));
            } else {
                print_error(status);
            }
        } else if (cmd == 'C' || cmd == 'T') {
            camera_status_t status = camera_set_test_pattern(cmd == 'T');
            if (status == CAMERA_OK) status = camera_capture_frame();
            if (status != CAMERA_OK) {
                print_error(status);
                continue;
            }

            const uint8_t *frame = camera_get_frame();
            size_t frame_size = camera_get_frame_size();
            printf("FRAME %u %u RGB565 %u %08lx\n",
                   CAMERA_WIDTH, CAMERA_HEIGHT, (unsigned int)frame_size,
                   (unsigned long)crc32(frame, frame_size));
            stdio_flush();
            stdio_put_string((const char *)frame, (int)frame_size, false, false);
            stdio_flush();
        } else if (cmd == '?') {
            puts("OV7675 camera: I=init C=photo T=sensor color bars");
        }
    }
}
