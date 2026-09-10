// OV7675 bring-up for the Astrocamp Pico W board. See README.md for protocol.
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "hardware/clocks.h"
#include "hardware/i2c.h"
#include "hardware/pwm.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "camera_capture.pio.h"

#define WIDTH 320u
#define HEIGHT 240u
#define FRAME_BYTES (WIDTH * HEIGHT * 2u)
#define CAM_ADDR 0x21
static uint32_t frame[FRAME_BYTES / 4];
static PIO const cam_pio = pio0;
static uint sm, offset;
static int dma_chan;
static bool ready;

static bool reg_write(uint8_t reg, uint8_t value) {
    uint8_t data[] = {reg, value};
    return i2c_write_timeout_us(i2c1, CAM_ADDR, data, 2, false, 20000) == 2;
}

static bool reg_read(uint8_t reg, uint8_t *value) {
    // SCCB register-select phase ends with STOP (no repeated START).
    if (i2c_write_timeout_us(i2c1, CAM_ADDR, &reg, 1, false, 20000) != 1) return false;
    return i2c_read_timeout_us(i2c1, CAM_ADDR, value, 1, false, 20000) == 1;
}

static bool sensor_init(void) {
    uint8_t pid = 0, ver = 0;
    if (!reg_read(0x0a, &pid) || !reg_read(0x0b, &ver)) {
        puts("ERR SCCB: check power, GP14/15, XCLK, PEN/PDN"); return false;
    }
    printf("INFO sensor PID=%02x VER=%02x\n", pid, ver);
    // OV7675 datasheet v2.0 specifies 0x76 / 0x73, not 0x76 / 0x75.
    if (pid != 0x76 || ver != 0x73) {
        puts("ERR unsupported sensor ID (expected 76:73)"); return false;
    }
    if (!reg_write(0x12, 0x80)) return false;
    sleep_ms(100);
    // Use documented OV7675 controls; retain factory ISP defaults.
    // Do not apply OV7670 reserved-register/scaling tables to this sensor.
    const uint8_t settings[][2] = {
        {0x6b, 0x0a}, // PLL bypass, retain low reserved bits
        {0x12, 0x14}, // QVGA + RGB
        {0x8c, 0x00}, // RGB444 off
        {0x40, 0xd0}, // full-range RGB565
        {0x15, 0x00}, // positive HREF/VSYNC, free-running PCLK
        {0x0c, 0x00}, // no byte swap
        {0x13, 0xcf}, // fast AEC, AGC, AWB; banding off for initial bring-up
        {0x70, 0x3a}, {0x71, 0x35}, // sensor test pattern off
        {0x11, 0x83}, // retain CLKRC bit7; divide clock by 4
    };
    for (uint i = 0; i < sizeof(settings) / sizeof(settings[0]); ++i) {
        if (!reg_write(settings[i][0], settings[i][1])) {
            printf("ERR register write %02x\n", settings[i][0]); return false;
        }
    }
    sleep_ms(1500); // allow auto exposure / white balance to settle
    return true;
}
static void capture_hw_init(void) {
    for (uint pin = 0; pin < 8; ++pin) pio_gpio_init(cam_pio, pin);
    pio_gpio_init(cam_pio, 22);
    pio_gpio_init(cam_pio, 26);
    pio_gpio_init(cam_pio, 27);
    sm = pio_claim_unused_sm(cam_pio, true);
    offset = pio_add_program(cam_pio, &camera_capture_program);
    dma_chan = dma_claim_unused_channel(true);
}
static bool capture(void) {
    pio_sm_set_enabled(cam_pio, sm, false);
    dma_channel_abort(dma_chan);
    pio_sm_config config = camera_capture_program_get_default_config(offset);
    sm_config_set_in_pins(&config, 0);
    sm_config_set_jmp_pin(&config, 26);
    sm_config_set_in_shift(&config, true, true, 32);
    pio_sm_init(cam_pio, sm, offset, &config);
    pio_interrupt_clear(cam_pio, 0);
    pio_interrupt_clear(cam_pio, 1);
    cam_pio->fdebug = 1u << (PIO_FDEBUG_RXSTALL_LSB + sm);
    pio_sm_put_blocking(cam_pio, sm, HEIGHT - 1);
    pio_sm_put_blocking(cam_pio, sm, WIDTH * 2 - 1);
    dma_channel_config dc = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, true);
    channel_config_set_dreq(&dc, pio_get_dreq(cam_pio, sm, false));
    dma_channel_configure(dma_chan, &dc, frame, &cam_pio->rxf[sm], FRAME_BYTES / 4, true);
    pio_sm_set_enabled(cam_pio, sm, true);
    absolute_time_t deadline = make_timeout_time_ms(5000);
    while ((!pio_interrupt_get(cam_pio, 0) || dma_channel_is_busy(dma_chan)) &&
           !pio_interrupt_get(cam_pio, 1) && !time_reached(deadline)) tight_loop_contents();
    bool ok = pio_interrupt_get(cam_pio, 0) && !dma_channel_is_busy(dma_chan) &&
              !pio_interrupt_get(cam_pio, 1) &&
              !(cam_pio->fdebug & (1u << (PIO_FDEBUG_RXSTALL_LSB + sm)));
    pio_sm_set_enabled(cam_pio, sm, false);
    dma_channel_abort(dma_chan);
    if (!ok) puts("ERR capture: timeout/short line/overflow; check GP0-7,22,26,27,28");
    return ok;
}
static uint32_t crc32(const uint8_t *data, size_t size) {
    uint32_t crc = 0xffffffffu;
    while (size--) {
        crc ^= *data++;
        for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
int main(void) {
    stdio_init_all();
    // 125 MHz / 10 = 12.5 MHz, exact 50% duty at default Pico clock.
    gpio_set_function(28, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(28);
    pwm_config pc = pwm_get_default_config();
    pwm_config_set_wrap(&pc, 9);
    pwm_init(slice, &pc, false);
    pwm_set_gpio_level(28, 5);
    pwm_set_enabled(slice, true);
    i2c_init(i2c1, 100000);
    gpio_set_function(14, GPIO_FUNC_I2C); gpio_set_function(15, GPIO_FUNC_I2C);
    gpio_pull_up(14); gpio_pull_up(15);
    capture_hw_init();
    while (true) {
        int cmd = getchar_timeout_us(100000);
        if (cmd == 'I') {
            ready = sensor_init();
            if (ready) printf("OK OV7675 320 240 RGB565 XCLK=%lu\n", (unsigned long)(clock_get_hz(clk_sys) / 10));
        } else if (cmd == 'C' || cmd == 'T') {
            if (!ready) { puts("ERR initialize with I first"); continue; }
            if (!reg_write(0x71, cmd == 'T' ? 0xb5 : 0x35)) {
                ready = false; puts("ERR test-pattern register"); continue;
            }
            sleep_ms(300);
            if (!capture()) continue;
            uint8_t *bytes = (uint8_t *)frame;
            printf("FRAME %u %u RGB565 %u %08lx\n", WIDTH, HEIGHT, FRAME_BYTES,
                   (unsigned long)crc32(bytes, FRAME_BYTES));
            stdio_flush();
            stdio_put_string((const char *)bytes, FRAME_BYTES, false, false);
            stdio_flush();
        } else if (cmd == '?') puts("OV7675 camera: I=init C=photo T=sensor color bars");
    }
}
