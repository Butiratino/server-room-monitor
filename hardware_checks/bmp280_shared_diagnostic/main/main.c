#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "driver/usb_serial_jtag.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void report(const char *format, ...)
{
    char line[384];
    int prefix = snprintf(line, sizeof(line), "t_ms=%" PRId64 " ", esp_timer_get_time() / 1000);
    va_list args;
    va_start(args, format);
    int length = vsnprintf(line + prefix, sizeof(line) - prefix, format, args);
    va_end(args);
    if (length >= 0 && prefix + length < sizeof(line) - 1) {
        line[prefix + length++] = '\n';
        usb_serial_jtag_write_bytes(line, prefix + length, pdMS_TO_TICKS(100));
    }
}

static esp_err_t read_regs(i2c_master_dev_handle_t dev, const char *phase, uint8_t reg, size_t count)
{
    uint8_t data[24];
    esp_err_t err = i2c_master_transmit_receive(dev, &reg, 1, data, count, 100);
    if (err != ESP_OK) {
        report("phase=%s read=0x%02X count=%u error=%s", phase, reg, (unsigned)count, esp_err_to_name(err));
        return err;
    }
    char bytes[sizeof(data) * 3 + 1];
    size_t pos = 0;
    for (size_t i = 0; i < count; ++i) {
        pos += snprintf(bytes + pos, sizeof(bytes) - pos, "%s%02X", i ? " " : "", data[i]);
    }
    report("phase=%s read=0x%02X count=%u bytes=%s", phase, reg, (unsigned)count, bytes);
    return ESP_OK;
}

static esp_err_t write_reg(i2c_master_dev_handle_t dev, const char *phase, uint8_t reg, uint8_t value)
{
    uint8_t command[] = {reg, value};
    esp_err_t err = i2c_master_transmit(dev, command, sizeof(command), 100);
    report("phase=%s write=0x%02X value=0x%02X result=%s", phase, reg, value, esp_err_to_name(err));
    return err;
}

static void snapshot(i2c_master_dev_handle_t dev, const char *phase)
{
    read_regs(dev, phase, 0xF3, 1);
    read_regs(dev, phase, 0xF4, 1);
    read_regs(dev, phase, 0xF5, 1);
}

void app_main(void)
{
    usb_serial_jtag_driver_config_t usb = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
    report("diagnostic=bmp280_shared target=esp32s3 i2c_port=1 sda=4 scl=6 address=0x76 hz=100000 pullups=internal iterations=5 reset_delay_ms=20 conversion_delay_ms=10");

    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_1, .sda_io_num = GPIO_NUM_4, .scl_io_num = GPIO_NUM_6,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    esp_err_t err = i2c_new_master_bus(&bus_config, &bus);
    if (err != ESP_OK) {
        report("phase=bus_init error=%s", esp_err_to_name(err));
        return;
    }
    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x76, .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t dev;
    err = i2c_master_bus_add_device(bus, &device_config, &dev);
    if (err != ESP_OK) {
        report("phase=device_init error=%s", esp_err_to_name(err));
        return;
    }

    read_regs(dev, "initial", 0xD0, 1);
    snapshot(dev, "before_reset");
    write_reg(dev, "reset", 0xE0, 0xB6);
    vTaskDelay(pdMS_TO_TICKS(20) + 1);
    snapshot(dev, "after_reset_delay");
    read_regs(dev, "calibration_burst", 0x88, 24);
    for (uint8_t reg = 0x88; reg <= 0x8D; ++reg) {
        read_regs(dev, "calibration_individual", reg, 1);
    }

    for (int iteration = 1; iteration <= 5; ++iteration) {
        report("iteration=%d begin", iteration);
        if (write_reg(dev, "sleep", 0xF4, 0x20) != ESP_OK) break;
        snapshot(dev, "after_sleep");
        if (write_reg(dev, "configure", 0xF5, 0x00) != ESP_OK) break;
        snapshot(dev, "before_forced");
        if (write_reg(dev, "forced", 0xF4, 0x21) != ESP_OK) break;
        snapshot(dev, "after_forced_immediate");
        vTaskDelay(pdMS_TO_TICKS(10) + 1);
        snapshot(dev, "after_conversion_delay");
        // Bounded status polling makes a stuck measuring/NVM bit visible without a busy loop.
        for (int poll = 0; poll < 20; ++poll) {
            uint8_t reg = 0xF3, status = 0;
            err = i2c_master_transmit_receive(dev, &reg, 1, &status, 1, 100);
            report("phase=ready_poll attempt=%d status=0x%02X result=%s", poll + 1, status, esp_err_to_name(err));
            if (err != ESP_OK || (status & 0x09) == 0) break;
            vTaskDelay(pdMS_TO_TICKS(5) + 1);
        }
        snapshot(dev, "after_poll");
        read_regs(dev, "raw_burst", 0xF7, 6);
        for (uint8_t reg = 0xF7; reg <= 0xFC; ++reg) {
            read_regs(dev, "raw_individual", reg, 1);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    report("diagnostic=complete");
}
