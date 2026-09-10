#include "telemetry.h"

#include "pico/rand.h"
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "icm42688.h"
#include "photodiode.h"

static int read_internal_temperature_centi_c(void) {
    const float conversion_factor = 3.3f / 4095.0f;
    uint16_t raw = adc_read();
    float voltage = (float)raw * conversion_factor;
    float temperature_c = 27.0f - (voltage - 0.706f) / 0.001721f;

    return (int)(temperature_c * 100.0f);
}

void telemetry_init(void) {
    adc_init();
    adc_set_temp_sensor_enabled(true);
    adc_select_input(ADC_TEMPERATURE_CHANNEL_NUM);
    photodiode_init();
}

void telemetry_collect(telemetry_data_t *telemetry, int32_t command_value) {
    telemetry->uptime_s = (uint32_t)(to_ms_since_boot(get_absolute_time()) / 1000);
    telemetry->temperature_centi_c = read_internal_temperature_centi_c();
    telemetry->random_value = get_rand_32() % 1000;
    telemetry->command_value = command_value;
    telemetry->gyro_z_valid =
        icm42688_read_gyro_z_centi_dps(&telemetry->gyro_z_centi_dps);
    photodiode_read_all(telemetry->photodiode_adc);
}
