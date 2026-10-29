#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ssd1306.h"

void app_main(void)
{
    usb_serial_jtag_driver_config_t usb = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));

    gpio_config_t power = {
        .pin_bit_mask = 1ULL << GPIO_NUM_36,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&power));
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_36, 0));
    vTaskDelay(pdMS_TO_TICKS(200));

    i2c_master_bus_config_t oled_bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = GPIO_NUM_17,
        .scl_io_num = GPIO_NUM_18,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t oled_bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&oled_bus_config, &oled_bus));
    ssd1306_config_t oled_config = {
        .bus = SSD1306_I2C,
        .width = 128,
        .height = 64,
        .iface.i2c = {
            .port = I2C_NUM_0,
            .addr = 0x3C,
            .rst_gpio = GPIO_NUM_21,
        },
    };
    ssd1306_handle_t display;
    ESP_ERROR_CHECK(ssd1306_new_i2c(&oled_config, &display));

    // Keep the external sensor on a separate controller from the built-in OLED.
    i2c_master_bus_config_t sensor_bus_config = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = GPIO_NUM_4,
        .scl_io_num = GPIO_NUM_6,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t sensor_bus = NULL;
    esp_err_t bus_error = i2c_new_master_bus(&sensor_bus_config, &sensor_bus);

    while (true) {
        ssd1306_clear(display);
        ssd1306_draw_text(display, 0, 0, "Sensor check", true);
        ssd1306_draw_text(display, 0, 12, "SDA4 SCL6", true);

        for (int address = 0x76; address <= 0x77; ++address) {
            esp_err_t error = bus_error;
            const char *state = "bus_error";
            const char *model = "unknown";
            int chip_id = -1;
            char screen[32];

            if (error == ESP_OK) {
                error = i2c_master_probe(sensor_bus, address, 100);
                if (error == ESP_ERR_NOT_FOUND) {
                    state = "not_found";
                } else if (error == ESP_OK) {
                    i2c_device_config_t device_config = {
                        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                        .device_address = address,
                        .scl_speed_hz = 100000,
                    };
                    i2c_master_dev_handle_t device;
                    state = "read_error";
                    error = i2c_master_bus_add_device(sensor_bus, &device_config, &device);
                    if (error == ESP_OK) {
                        // Only select and read the identity register; no configuration writes.
                        const uint8_t register_address = 0xD0;
                        uint8_t identity = 0;
                        error = i2c_master_transmit_receive(device, &register_address, 1,
                                                            &identity, 1, 100);
                        esp_err_t remove_error = i2c_master_bus_rm_device(device);
                        if (error == ESP_OK) {
                            chip_id = identity;
                            if (identity == 0x58) {
                                model = "BMP280";
                                state = "found";
                            } else if (identity == 0x60) {
                                model = "BME280";
                                state = "found";
                            } else {
                                state = "unknown_id";
                            }
                            if (remove_error != ESP_OK) {
                                error = remove_error;
                                state = "bus_error";
                            }
                        }
                    }
                }
            }

            if (chip_id >= 0 && error == ESP_OK) {
                snprintf(screen, sizeof(screen), "%02X %s ID:%02X", address, model, chip_id);
            } else if (strcmp(state, "not_found") == 0) {
                snprintf(screen, sizeof(screen), "%02X: not found", address);
            } else {
                snprintf(screen, sizeof(screen), "%02X: I2C error", address);
            }
            ssd1306_draw_text(display, 0, 26 + (address - 0x76) * 12, screen, true);

            char report[192];
            int length = snprintf(report, sizeof(report),
                                  "{\"type\":\"sensor_probe\",\"address\":%d,"
                                  "\"state\":\"%s\",\"chip_id\":%d,\"model\":\"%s\","
                                  "\"error\":\"%s\"}\n",
                                  address, state, chip_id, model, esp_err_to_name(error));
            if (length > 0 && (size_t)length < sizeof(report)) {
                usb_serial_jtag_write_bytes(report, length, pdMS_TO_TICKS(100));
            }
        }

        ssd1306_draw_text(display, 0, 52, "No temp read yet", true);
        ssd1306_display(display);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
