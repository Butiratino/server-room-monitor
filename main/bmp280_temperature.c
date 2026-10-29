#include "bmp280_temperature.h"

#include <math.h>

#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "BMP280_TEMP";

static esp_err_t read_registers(i2c_master_dev_handle_t device, uint8_t address,
                                uint8_t *data, size_t size)
{
    return i2c_master_transmit_receive(device, &address, 1, data, size, 100);
}

static esp_err_t wait_ready(i2c_master_dev_handle_t device)
{
    const int64_t deadline = esp_timer_get_time() + 100000;
    do {
        uint8_t status;
        esp_err_t error = read_registers(device, 0xF3, &status, 1);
        if (error != ESP_OK) {
            return error;
        }
        if ((status & 0x09) == 0) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(5) + 1);
    } while (esp_timer_get_time() < deadline);
    return ESP_ERR_TIMEOUT;
}

esp_err_t bmp280_read_temperature(i2c_master_dev_handle_t device,
                                 bmp280_temperature_t *sample)
{
    if (sample == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *sample = (bmp280_temperature_t){ .temperature_c = NAN, .pressure_pa = NAN };
    if (device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t identity;
    ESP_RETURN_ON_ERROR(read_registers(device, 0xD0, &identity, 1), TAG, "Chip ID read failed");
    if (identity != 0x58) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    // Sleep first so the filter configuration is writable; pressure is skipped.
    const uint8_t sleep_command[] = {0xF4, 0x20};
    ESP_RETURN_ON_ERROR(i2c_master_transmit(device, sleep_command, sizeof(sleep_command), 100),
                        TAG, "Sleep command failed");
    ESP_RETURN_ON_ERROR(wait_ready(device), TAG, "Sensor or NVM busy");
    const uint8_t filter_command[] = {0xF5, 0x00};
    ESP_RETURN_ON_ERROR(i2c_master_transmit(device, filter_command, sizeof(filter_command), 100),
                        TAG, "Filter configuration failed");

    uint8_t calibration[24];
    ESP_RETURN_ON_ERROR(read_registers(device, 0x88, calibration, sizeof(calibration)),
                        TAG, "Calibration read failed");
    bmp280_temperature_t measurement = {
        .temperature_c = NAN,
        .pressure_pa = NAN,
        .t1 = (uint16_t)(calibration[0] | ((uint16_t)calibration[1] << 8)),
        .t2 = (int16_t)(calibration[2] | ((uint16_t)calibration[3] << 8)),
        .t3 = (int16_t)(calibration[4] | ((uint16_t)calibration[5] << 8)),
    };
    uint16_t p1 = (uint16_t)(calibration[6] | ((uint16_t)calibration[7] << 8));
    int16_t p2 = (int16_t)(calibration[8] | ((uint16_t)calibration[9] << 8));
    int16_t p3 = (int16_t)(calibration[10] | ((uint16_t)calibration[11] << 8));
    int16_t p4 = (int16_t)(calibration[12] | ((uint16_t)calibration[13] << 8));
    int16_t p5 = (int16_t)(calibration[14] | ((uint16_t)calibration[15] << 8));
    int16_t p6 = (int16_t)(calibration[16] | ((uint16_t)calibration[17] << 8));
    int16_t p7 = (int16_t)(calibration[18] | ((uint16_t)calibration[19] << 8));
    int16_t p8 = (int16_t)(calibration[20] | ((uint16_t)calibration[21] << 8));
    int16_t p9 = (int16_t)(calibration[22] | ((uint16_t)calibration[23] << 8));
    // Temperature and pressure oversampling x1, forced mode (then sleep).
    const uint8_t measure_command[] = {0xF4, 0x25};
    ESP_RETURN_ON_ERROR(i2c_master_transmit(device, measure_command, sizeof(measure_command), 100),
                        TAG, "Measurement command failed");
    // Do not mistake the interval before measuring becomes set for completion.
    vTaskDelay(pdMS_TO_TICKS(10) + 1);
    ESP_RETURN_ON_ERROR(wait_ready(device), TAG, "Measurement timed out");

    ESP_RETURN_ON_ERROR(read_registers(device, 0xF3, &measurement.status, 1), TAG,
                        "Status read failed");
    ESP_RETURN_ON_ERROR(read_registers(device, 0xF7, measurement.raw_bytes,
                                       sizeof(measurement.raw_bytes)), TAG, "ADC read failed");
    measurement.raw_pressure = ((uint32_t)measurement.raw_bytes[0] << 12) |
                               ((uint32_t)measurement.raw_bytes[1] << 4) |
                               (measurement.raw_bytes[2] >> 4);
    measurement.raw = ((uint32_t)measurement.raw_bytes[3] << 12) |
                      ((uint32_t)measurement.raw_bytes[4] << 4) |
                      (measurement.raw_bytes[5] >> 4);
    measurement.raw_available = true;
    *sample = measurement;
    if (measurement.t1 == 0 || measurement.t1 == UINT16_MAX || measurement.t2 == 0 || p1 == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (measurement.raw == 0x80000 || measurement.raw_pressure == 0x80000) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    // BMP280 datasheet, section 3.11.3: factory temperature compensation.
    double var1 = (measurement.raw / 16384.0 - measurement.t1 / 1024.0) * measurement.t2;
    double delta = measurement.raw / 131072.0 - measurement.t1 / 8192.0;
    double var2 = delta * delta * measurement.t3;
    double fine_temperature = var1 + var2;
    measurement.temperature_c = fine_temperature / 5120.0;
    if (!isfinite(measurement.temperature_c) || measurement.temperature_c < -40.0 ||
        measurement.temperature_c > 85.0) {
        *sample = measurement;
        return ESP_ERR_INVALID_RESPONSE;
    }

    // BMP280 datasheet, section 3.11.3: double-precision pressure compensation.
    double pressure_var1 = fine_temperature / 2.0 - 64000.0;
    double pressure_var2 = pressure_var1 * pressure_var1 * p6 / 32768.0;
    pressure_var2 += pressure_var1 * p5 * 2.0;
    pressure_var2 = pressure_var2 / 4.0 + p4 * 65536.0;
    pressure_var1 = (p3 * pressure_var1 * pressure_var1 / 524288.0 +
                     p2 * pressure_var1) / 524288.0;
    pressure_var1 = (1.0 + pressure_var1 / 32768.0) * p1;
    if (pressure_var1 == 0.0) {
        *sample = measurement;
        return ESP_ERR_INVALID_RESPONSE;
    }
    double pressure = 1048576.0 - measurement.raw_pressure;
    pressure = (pressure - pressure_var2 / 4096.0) * 6250.0 / pressure_var1;
    pressure_var1 = p9 * pressure * pressure / 2147483648.0;
    pressure_var2 = pressure * p8 / 32768.0;
    measurement.pressure_pa = pressure + (pressure_var1 + pressure_var2 + p7) / 16.0;
    if (!isfinite(measurement.pressure_pa) || measurement.pressure_pa < 30000.0 ||
        measurement.pressure_pa > 110000.0) {
        *sample = measurement;
        return ESP_ERR_INVALID_RESPONSE;
    }

    *sample = measurement;
    return ESP_OK;
}
