#include "camera.h"

#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/i2c.h"
#include "hardware/pio.h"
#include "hardware/pwm.h"
#include "camera_capture.pio.h"

#define CAM_ADDR 0x21

static uint32_t frame[CAMERA_FRAME_BYTES / sizeof(uint32_t)];
static PIO const cam_pio = pio0;
static uint sm;
static uint offset;
static int dma_chan;
static bool hardware_ready;
static bool sensor_ready;
static bool test_pattern_known;
static bool test_pattern_enabled;
static uint8_t sensor_pid;
static uint8_t sensor_ver;

static bool reg_write(uint8_t reg, uint8_t value) {
    uint8_t data[] = {reg, value};
    return i2c_write_timeout_us(i2c1, CAM_ADDR, data, 2, false, 20000) == 2;
}

static bool reg_read(uint8_t reg, uint8_t *value) {
    if (i2c_write_timeout_us(i2c1, CAM_ADDR, &reg, 1, false, 20000) != 1) {
        return false;
    }
    return i2c_read_timeout_us(i2c1, CAM_ADDR, value, 1, false, 20000) == 1;
}

static camera_status_t sensor_init(void) {
    sensor_pid = 0;
    sensor_ver = 0;
    test_pattern_known = false;
    if (!reg_read(0x0a, &sensor_pid) || !reg_read(0x0b, &sensor_ver)) {
        return CAMERA_ERROR_SCCB;
    }
    if (sensor_pid != 0x76 || sensor_ver != 0x73) {
        return CAMERA_ERROR_SENSOR_ID;
    }
    if (!reg_write(0x12, 0x80)) return CAMERA_ERROR_REGISTER_WRITE;
    sleep_ms(100);

    const uint8_t settings[][2] = {
        {0x6b, 0x0a},
        {0x12, 0x04}, // RGB, VGA source window; QVGA is produced by DCW below
        {0x8c, 0x00}, {0x40, 0xd0},
        {0x15, 0x00},

        // OV7675 QVGA window/downsampling.  Setting COM7 to QVGA alone can
        // leave the horizontal and vertical sampling ratios inconsistent,
        // which makes a nominal 320x240 frame look stretched or squeezed.
        {0x0c, 0x04}, // COM3: enable downsample/crop/window
        {0x3e, 0x11}, // COM14: enable DCW and divide pixel clock by 2
        {0x72, 0x22}, // downsample horizontal and vertical axes equally
        {0x73, 0xf2}, // pixel-clock divider used by the QVGA DCW mode
        {0x17, 0x15}, {0x18, 0x03}, {0x32, 0xc0}, // horizontal window
        {0x19, 0x03}, {0x1a, 0x7b}, {0x03, 0xf0}, // vertical window

        // Deterministic ISP tuning for clearer RGB565 output.  The sensor's
        // reset defaults otherwise vary noticeably between module revisions.
        {0x13, 0xcf}, // fast AEC plus automatic exposure/gain/white balance
        {0x41, 0x08}, // enable automatic white-balance gain
        {0x43, 0x14}, {0x44, 0xf0}, {0x45, 0x34},
        {0x46, 0x58}, {0x47, 0x28}, {0x48, 0x3a}, // AWB calibration
        {0x4f, 0xb3}, {0x50, 0xb3}, {0x51, 0x00},
        {0x52, 0x3d}, {0x53, 0xa7}, {0x54, 0xe4}, // RGB colour matrix
        {0x3d, 0xc0}, // gamma and automatic colour saturation
        {0x3f, 0x04}, // light edge enhancement without oversharpening
        {0x62, 0x00}, {0x63, 0x00}, {0x64, 0x04},
        {0x65, 0x20}, {0x66, 0x05}, {0x94, 0x04},
        {0x95, 0x08}, // lens shading correction
        // Smooth 16-step gamma curve: preserves shadow and highlight detail.
        {0x7a, 0x20}, {0x7b, 0x1c}, {0x7c, 0x28}, {0x7d, 0x3c},
        {0x7e, 0x55}, {0x7f, 0x68}, {0x80, 0x76}, {0x81, 0x80},
        {0x82, 0x88}, {0x83, 0x8f}, {0x84, 0x96}, {0x85, 0xa3},
        {0x86, 0xaf}, {0x87, 0xc4}, {0x88, 0xd7}, {0x89, 0xe8},
        {0x70, 0x3a}, {0x71, 0x35}, {0x11, 0x83},
    };
    for (uint i = 0; i < sizeof(settings) / sizeof(settings[0]); ++i) {
        if (!reg_write(settings[i][0], settings[i][1])) {
            return CAMERA_ERROR_REGISTER_WRITE;
        }
    }
    sleep_ms(1500);
    test_pattern_enabled = false;
    test_pattern_known = true;
    return CAMERA_OK;
}

static void hardware_init(void) {
    gpio_set_function(28, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(28);
    pwm_config pc = pwm_get_default_config();
    pwm_config_set_wrap(&pc, 9);
    pwm_init(slice, &pc, false);
    pwm_set_gpio_level(28, 5);
    pwm_set_enabled(slice, true);

    i2c_init(i2c1, 100000);
    gpio_set_function(14, GPIO_FUNC_I2C);
    gpio_set_function(15, GPIO_FUNC_I2C);
    gpio_pull_up(14);
    gpio_pull_up(15);

    for (uint pin = 0; pin < 8; ++pin) pio_gpio_init(cam_pio, pin);
    pio_gpio_init(cam_pio, 22);
    pio_gpio_init(cam_pio, 26);
    pio_gpio_init(cam_pio, 27);
    sm = pio_claim_unused_sm(cam_pio, true);
    offset = pio_add_program(cam_pio, &camera_capture_program);
    dma_chan = dma_claim_unused_channel(true);
    hardware_ready = true;
}

camera_status_t camera_init(void) {
    if (!hardware_ready) hardware_init();
    sensor_ready = false;
    camera_status_t status = sensor_init();
    sensor_ready = status == CAMERA_OK;
    return status;
}

camera_status_t camera_set_test_pattern(bool enabled) {
    if (!sensor_ready) return CAMERA_ERROR_NOT_INITIALIZED;
    /* The normal stream repeatedly requests "false". Its register value was
     * already installed during sensor_init(), so avoid a needless SCCB write
     * and the former 300 ms settling delay for every frame. */
    if (test_pattern_known && test_pattern_enabled == enabled) return CAMERA_OK;
    if (!reg_write(0x71, enabled ? 0xb5 : 0x35)) {
        sensor_ready = false;
        test_pattern_known = false;
        return CAMERA_ERROR_REGISTER_WRITE;
    }
    test_pattern_enabled = enabled;
    test_pattern_known = true;
    /* Only an actual pattern-mode switch needs the sensor to settle. */
    sleep_ms(300);
    return CAMERA_OK;
}

camera_status_t camera_capture_frame(void) {
    if (!sensor_ready) return CAMERA_ERROR_NOT_INITIALIZED;
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
    pio_sm_put_blocking(cam_pio, sm, CAMERA_HEIGHT - 1);
    pio_sm_put_blocking(cam_pio, sm, CAMERA_WIDTH * CAMERA_BYTES_PER_PIXEL - 1);

    dma_channel_config dc = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, true);
    channel_config_set_dreq(&dc, pio_get_dreq(cam_pio, sm, false));
    dma_channel_configure(dma_chan, &dc, frame, &cam_pio->rxf[sm],
                          CAMERA_FRAME_BYTES / sizeof(uint32_t), true);
    pio_sm_set_enabled(cam_pio, sm, true);

    absolute_time_t deadline = make_timeout_time_ms(5000);
    while ((!pio_interrupt_get(cam_pio, 0) || dma_channel_is_busy(dma_chan)) &&
           !pio_interrupt_get(cam_pio, 1) && !time_reached(deadline)) {
        tight_loop_contents();
    }
    bool ok = pio_interrupt_get(cam_pio, 0) && !dma_channel_is_busy(dma_chan) &&
              !pio_interrupt_get(cam_pio, 1) &&
              !(cam_pio->fdebug & (1u << (PIO_FDEBUG_RXSTALL_LSB + sm)));
    pio_sm_set_enabled(cam_pio, sm, false);
    dma_channel_abort(dma_chan);
    return ok ? CAMERA_OK : CAMERA_ERROR_CAPTURE;
}

const uint8_t *camera_get_frame(void) { return (const uint8_t *)frame; }
size_t camera_get_frame_size(void) { return CAMERA_FRAME_BYTES; }

void camera_get_sensor_id(uint8_t *pid, uint8_t *ver) {
    if (pid != NULL) *pid = sensor_pid;
    if (ver != NULL) *ver = sensor_ver;
}

const char *camera_status_string(camera_status_t status) {
    switch (status) {
        case CAMERA_OK: return "OK";
        case CAMERA_ERROR_SCCB: return "SCCB communication failed";
        case CAMERA_ERROR_SENSOR_ID: return "unsupported sensor ID";
        case CAMERA_ERROR_REGISTER_WRITE: return "sensor register write failed";
        case CAMERA_ERROR_NOT_INITIALIZED: return "camera is not initialized";
        case CAMERA_ERROR_CAPTURE: return "capture timeout, short line, or overflow";
        default: return "unknown camera error";
    }
}
