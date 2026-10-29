#include <math.h>
#include <stdio.h>

#include "bmp280.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void)
{
    usb_serial_jtag_driver_config_t usb = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));

    bmp280_params_t params;
    bmp280_t device = {0};
    esp_err_t init_error = i2cdev_init();
    if (init_error == ESP_OK) {
        init_error = bmp280_init_default_params(&params);
    }
    if (init_error == ESP_OK) {
        init_error = bmp280_init_desc(&device, BMP280_I2C_ADDRESS_0, I2C_NUM_1,
                                      GPIO_NUM_4, GPIO_NUM_6);
    }
    if (init_error == ESP_OK) {
        // Match the already tested wiring and bus speed, not the library's 1 MHz default.
        device.i2c_dev.cfg.master.clk_speed = 100000;
        device.i2c_dev.cfg.sda_pullup_en = GPIO_PULLUP_ENABLE;
        device.i2c_dev.cfg.scl_pullup_en = GPIO_PULLUP_ENABLE;
        init_error = bmp280_init(&device, &params);
    }

    // Library defaults: normal mode, temperature/pressure x4, filter off, standby 250 ms.
    vTaskDelay(pdMS_TO_TICKS(1000));
    while (true) {
        float temperature = NAN;
        float pressure = NAN;
        esp_err_t error = init_error;
        if (error == ESP_OK) {
            error = bmp280_read_float(&device, &temperature, &pressure, NULL);
        }

        uint8_t raw[6] = {0};
        uint8_t status_control[2] = {0};
        esp_err_t raw_error = init_error;
        esp_err_t status_error = init_error;
        if (init_error == ESP_OK) {
            raw_error = i2c_dev_read_reg(&device.i2c_dev, 0xF7, raw, sizeof(raw));
            status_error = i2c_dev_read_reg(&device.i2c_dev, 0xF3, status_control, sizeof(status_control));
        }
        bool valid = error == ESP_OK && isfinite(temperature) &&
                     temperature >= -40.0f && temperature <= 85.0f;
        char report[512];
        int length;
        if (error == ESP_OK && isfinite(temperature) && isfinite(pressure)) {
            length = snprintf(report, sizeof(report),
                "{\"type\":\"bmp280_reference\",\"library\":\"esp-idf-lib/bmp280@1.0.7\","
                "\"chip_id\":%u,\"temperature_c\":%.2f,\"pressure_pa\":%.2f,\"valid_temperature\":%s,"
                "\"calibration\":[%u,%d,%d],\"raw\":[%u,%u,%u,%u,%u,%u],"
                "\"status\":%u,\"control\":%u,\"errors\":[%d,%d,%d]}\n",
                device.id, temperature, pressure, valid ? "true" : "false",
                device.dig_T1, device.dig_T2, device.dig_T3,
                raw[0], raw[1], raw[2], raw[3], raw[4], raw[5],
                status_control[0], status_control[1], error, raw_error, status_error);
        } else {
            length = snprintf(report, sizeof(report),
                "{\"type\":\"bmp280_reference\",\"temperature_c\":null,"
                "\"valid_temperature\":false,\"error\":\"%s\"}\n", esp_err_to_name(error));
        }
        if (length > 0 && (size_t)length < sizeof(report)) {
            usb_serial_jtag_write_bytes(report, length, pdMS_TO_TICKS(200));
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
