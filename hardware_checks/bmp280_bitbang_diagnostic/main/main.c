#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SDA_GPIO GPIO_NUM_41
#define SCL_GPIO GPIO_NUM_42
#define BMP280_ADDRESS 0x76
#define HALF_CYCLE_US 30
#define STRETCH_TIMEOUT_US 2000
#define ITERATIONS 5

typedef enum {
    BB_OK = 0,
    BB_BUS_BUSY,
    BB_SCL_TIMEOUT,
    BB_ADDRESS_NACK,
    BB_DATA_NACK,
} bb_result_t;

static const char *result_name(bb_result_t result)
{
    switch (result) {
    case BB_OK: return "ok";
    case BB_BUS_BUSY: return "bus_busy";
    case BB_SCL_TIMEOUT: return "scl_timeout";
    case BB_ADDRESS_NACK: return "address_nack";
    case BB_DATA_NACK: return "data_nack";
    default: return "unknown";
    }
}

static void report(const char *format, ...)
{
    char line[512];
    int prefix = snprintf(line, sizeof(line), "{\"t_ms\":%" PRId64 ",", esp_timer_get_time() / 1000);
    if (prefix < 0 || (size_t)prefix >= sizeof(line)) {
        return;
    }

    va_list args;
    va_start(args, format);
    int body = vsnprintf(line + prefix, sizeof(line) - (size_t)prefix, format, args);
    va_end(args);
    if (body < 0 || (size_t)body >= sizeof(line) - (size_t)prefix - 2) {
        return;
    }

    size_t length = (size_t)prefix + (size_t)body;
    line[length++] = '}';
    line[length++] = '\n';
    usb_serial_jtag_write_bytes(line, length, pdMS_TO_TICKS(100));
}

static inline void line_set(gpio_num_t gpio, bool released)
{
    gpio_set_level(gpio, released ? 1 : 0);
}

static bb_result_t scl_release_and_wait(void)
{
    line_set(SCL_GPIO, true);
    int64_t deadline = esp_timer_get_time() + STRETCH_TIMEOUT_US;
    while (!gpio_get_level(SCL_GPIO)) {
        if (esp_timer_get_time() >= deadline) {
            return BB_SCL_TIMEOUT;
        }
    }
    esp_rom_delay_us(HALF_CYCLE_US);
    return BB_OK;
}

static void scl_low(void)
{
    line_set(SCL_GPIO, false);
    esp_rom_delay_us(HALF_CYCLE_US);
}

static bb_result_t bus_start(void)
{
    line_set(SDA_GPIO, true);
    bb_result_t result = scl_release_and_wait();
    if (result != BB_OK) {
        return result;
    }
    if (!gpio_get_level(SDA_GPIO)) {
        return BB_BUS_BUSY;
    }
    line_set(SDA_GPIO, false);
    esp_rom_delay_us(HALF_CYCLE_US);
    scl_low();
    return BB_OK;
}

static bb_result_t bus_repeated_start(void)
{
    line_set(SDA_GPIO, true);
    esp_rom_delay_us(HALF_CYCLE_US);
    bb_result_t result = scl_release_and_wait();
    if (result != BB_OK) {
        return result;
    }
    line_set(SDA_GPIO, false);
    esp_rom_delay_us(HALF_CYCLE_US);
    scl_low();
    return BB_OK;
}

static bb_result_t bus_stop(void)
{
    line_set(SDA_GPIO, false);
    esp_rom_delay_us(HALF_CYCLE_US);
    bb_result_t result = scl_release_and_wait();
    line_set(SDA_GPIO, true);
    esp_rom_delay_us(HALF_CYCLE_US);
    return result;
}

static bb_result_t write_bit(bool bit)
{
    line_set(SDA_GPIO, bit);
    esp_rom_delay_us(HALF_CYCLE_US);
    bb_result_t result = scl_release_and_wait();
    scl_low();
    return result;
}

static bb_result_t read_bit(bool *bit)
{
    line_set(SDA_GPIO, true);
    esp_rom_delay_us(HALF_CYCLE_US);
    bb_result_t result = scl_release_and_wait();
    if (result == BB_OK) {
        *bit = gpio_get_level(SDA_GPIO) != 0;
    }
    scl_low();
    return result;
}

static bb_result_t write_byte(uint8_t value, bool *acked)
{
    for (uint8_t mask = 0x80; mask != 0; mask >>= 1) {
        bb_result_t result = write_bit((value & mask) != 0);
        if (result != BB_OK) {
            return result;
        }
    }
    bool nack = true;
    bb_result_t result = read_bit(&nack);
    if (result == BB_OK) {
        *acked = !nack;
    }
    return result;
}

static bb_result_t read_byte(uint8_t *value, bool ack)
{
    uint8_t data = 0;
    for (int bit_index = 0; bit_index < 8; ++bit_index) {
        bool bit = false;
        bb_result_t result = read_bit(&bit);
        if (result != BB_OK) {
            return result;
        }
        data = (uint8_t)((data << 1) | (bit ? 1U : 0U));
    }
    *value = data;
    return write_bit(!ack);
}

static bb_result_t checked_write(uint8_t value, bb_result_t nack_result)
{
    bool acked = false;
    bb_result_t result = write_byte(value, &acked);
    if (result != BB_OK) {
        return result;
    }
    return acked ? BB_OK : nack_result;
}

static bb_result_t finish_failed_transaction(bb_result_t result)
{
    bb_result_t stop_result = bus_stop();
    return result != BB_OK ? result : stop_result;
}

static bb_result_t bmp_write_reg(uint8_t reg, uint8_t value)
{
    bb_result_t result = bus_start();
    if (result != BB_OK) return result;
    result = checked_write((uint8_t)(BMP280_ADDRESS << 1), BB_ADDRESS_NACK);
    if (result == BB_OK) result = checked_write(reg, BB_DATA_NACK);
    if (result == BB_OK) result = checked_write(value, BB_DATA_NACK);
    if (result != BB_OK) return finish_failed_transaction(result);
    return bus_stop();
}

static bb_result_t bmp_read_regs(uint8_t reg, uint8_t *data, size_t count)
{
    if (count == 0) {
        return BB_OK;
    }

    bb_result_t result = bus_start();
    if (result != BB_OK) return result;
    result = checked_write((uint8_t)(BMP280_ADDRESS << 1), BB_ADDRESS_NACK);
    if (result == BB_OK) result = checked_write(reg, BB_DATA_NACK);
    if (result == BB_OK) result = bus_repeated_start();
    if (result == BB_OK) result = checked_write((uint8_t)((BMP280_ADDRESS << 1) | 1U), BB_ADDRESS_NACK);
    for (size_t index = 0; result == BB_OK && index < count; ++index) {
        result = read_byte(&data[index], index + 1 < count);
    }
    if (result != BB_OK) return finish_failed_transaction(result);
    return bus_stop();
}

static void report_write(const char *phase, uint8_t reg, uint8_t value)
{
    bb_result_t result = bmp_write_reg(reg, value);
    report("\"phase\":\"%s\",\"op\":\"write\",\"reg\":\"%02X\",\"value\":\"%02X\",\"result\":\"%s\"",
           phase, reg, value, result_name(result));
}

static bb_result_t report_read(const char *phase, uint8_t reg, size_t count)
{
    uint8_t data[24] = {0};
    if (count > sizeof(data)) {
        report("\"phase\":\"%s\",\"op\":\"read\",\"reg\":\"%02X\",\"result\":\"invalid_count\"", phase, reg);
        return BB_DATA_NACK;
    }

    bb_result_t result = bmp_read_regs(reg, data, count);
    char bytes[sizeof(data) * 2 + 1];
    size_t position = 0;
    if (result == BB_OK) {
        for (size_t index = 0; index < count; ++index) {
            position += (size_t)snprintf(bytes + position, sizeof(bytes) - position, "%02X", data[index]);
        }
    }
    bytes[position] = '\0';
    report("\"phase\":\"%s\",\"op\":\"read\",\"reg\":\"%02X\",\"count\":%u,\"bytes\":\"%s\",\"result\":\"%s\"",
           phase, reg, (unsigned)count, bytes, result_name(result));
    return result;
}

static void register_snapshot(const char *phase)
{
    report_read(phase, 0xD0, 1);
    report_read(phase, 0xF3, 1);
    report_read(phase, 0xF4, 1);
    report_read(phase, 0xF5, 1);
}

static void bus_init(void)
{
    gpio_config_t config = {
        .pin_bit_mask = (1ULL << SDA_GPIO) | (1ULL << SCL_GPIO),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&config);
    line_set(SDA_GPIO, true);
    line_set(SCL_GPIO, true);
    esp_rom_delay_us(STRETCH_TIMEOUT_US);
}

void app_main(void)
{
    usb_serial_jtag_driver_config_t usb_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_config));
    bus_init();

    report("\"event\":\"start\",\"target\":\"esp32s3\",\"address\":\"76\",\"sda\":41,\"scl\":42,\"half_cycle_us\":30,\"stretch_timeout_us\":2000,\"iterations\":5");
    report("\"event\":\"bus_idle\",\"sda\":%d,\"scl\":%d", gpio_get_level(SDA_GPIO), gpio_get_level(SCL_GPIO));

    register_snapshot("initial");
    report_read("initial_calibration", 0x88, 24);

    report_write("soft_reset", 0xE0, 0xB6);
    vTaskDelay(pdMS_TO_TICKS(20) + 1);
    register_snapshot("after_reset_20ms");
    report_read("after_reset_calibration", 0x88, 24);

    for (int iteration = 1; iteration <= ITERATIONS; ++iteration) {
        report("\"event\":\"iteration\",\"iteration\":%d", iteration);
        report_write("configure", 0xF5, 0x00);
        report_write("forced_temp_x1_pressure_skip", 0xF4, 0x21);
        report_read("status_immediate", 0xF3, 1);
        vTaskDelay(pdMS_TO_TICKS(20) + 1);
        report_read("status_delayed_20ms", 0xF3, 1);
        report_read("raw_burst", 0xF7, 6);
        for (uint8_t reg = 0xF7; reg <= 0xFC; ++reg) {
            report_read("raw_individual", reg, 1);
        }
        if (iteration < ITERATIONS) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    report("\"event\":\"complete\"");
}
