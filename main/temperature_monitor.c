#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "bmp280_temperature.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ssd1306.h"
#include "wifi_manager.h"
#include "zabbix_sender.h"

static const char *wifi_state_name(wifi_manager_state_t state)
{
    switch (state) {
    case WIFI_STATE_UNCONFIGURED: return "unconfigured";
    case WIFI_STATE_PROVISIONING: return "provisioning";
    case WIFI_STATE_CONNECTING: return "connecting";
    case WIFI_STATE_ONLINE: return "online";
    case WIFI_STATE_RETRY_WAIT: return "retry_wait";
    case WIFI_STATE_ERROR: return "error";
    default: return "unknown";
    }
}

static const char *zabbix_send_state_name(zabbix_send_state_t state)
{
    switch (state) {
    case ZABBIX_SEND_WAITING: return "waiting";
    case ZABBIX_SEND_OK: return "ok";
    case ZABBIX_SEND_ERROR: return "error";
    default: return "unknown";
    }
}

static const char *wifi_display_state_name(wifi_manager_state_t state)
{
    switch (state) {
    case WIFI_STATE_UNCONFIGURED: return "no config";
    case WIFI_STATE_PROVISIONING: return "setup";
    case WIFI_STATE_CONNECTING: return "joining";
    case WIFI_STATE_ONLINE: return "online";
    case WIFI_STATE_RETRY_WAIT: return "retry";
    case WIFI_STATE_ERROR: return "error";
    default: return "unknown";
    }
}

static void format_wifi_setup_title(const wifi_manager_status_t *status,
                                   char *title, size_t capacity)
{
    if (status->failure_reason != WIFI_FAILURE_NONE) {
        snprintf(title, capacity, "Wi-Fi %s",
                 wifi_manager_failure_display_name(status->failure_reason));
        return;
    }
    switch (status->state) {
    case WIFI_STATE_CONNECTING: strlcpy(title, "Wi-Fi joining", capacity); break;
    case WIFI_STATE_RETRY_WAIT: strlcpy(title, "Wi-Fi retry", capacity); break;
    case WIFI_STATE_ERROR: strlcpy(title, "Wi-Fi error", capacity); break;
    default: strlcpy(title, "Wi-Fi setup", capacity); break;
    }
}

static const char *zabbix_display_state_name(zabbix_send_state_t state)
{
    switch (state) {
    case ZABBIX_SEND_WAITING: return "wait";
    case ZABBIX_SEND_OK: return "ok";
    case ZABBIX_SEND_ERROR: return "error";
    default: return "unknown";
    }
}

static const char *zabbix_display_detail(const zabbix_sender_status_t *status)
{
    if (status->state != ZABBIX_SEND_ERROR) {
        return zabbix_display_state_name(status->state);
    }
    switch (status->last_stage) {
    case ZABBIX_STAGE_CONFIG: return "config err";
    case ZABBIX_STAGE_BUILD: return "build err";
    case ZABBIX_STAGE_RESOLVE: return "DNS err";
    case ZABBIX_STAGE_CONNECT: return "connect TO";
    case ZABBIX_STAGE_SEND: return "send TO";
    case ZABBIX_STAGE_RESPONSE_HEADER: return "reply TO";
    case ZABBIX_STAGE_RESPONSE_BODY: return "body TO";
    case ZABBIX_STAGE_VALIDATE: return "bad reply";
    default: return "send error";
    }
}

void app_main(void)
{
    usb_serial_jtag_driver_config_t usb = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    // Reports are about 260 bytes; the default 256-byte TX ring cannot fit them.
    usb.tx_buffer_size = 1024;
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

    i2c_master_bus_config_t sensor_bus_config = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = GPIO_NUM_4,
        .scl_io_num = GPIO_NUM_6,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t sensor_bus;
    i2c_master_dev_handle_t sensor = NULL;
    esp_err_t init_error = i2c_new_master_bus(&sensor_bus_config, &sensor_bus);
    if (init_error == ESP_OK) {
        i2c_device_config_t device_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = 0x76,
            .scl_speed_hz = 100000,
        };
        init_error = i2c_master_bus_add_device(sensor_bus, &device_config, &sensor);
    }

    if (init_error == ESP_OK) {
        const uint8_t reset[] = {0xE0, 0xB6};
        init_error = i2c_master_transmit(sensor, reset, sizeof(reset), 100);
        vTaskDelay(pdMS_TO_TICKS(20) + 1);
    }

    esp_err_t wifi_start_error = wifi_manager_start();
    esp_err_t sender_start_error = wifi_start_error == ESP_OK
                                       ? zabbix_sender_start()
                                       : wifi_start_error;
    if (wifi_start_error != ESP_OK) {
        ESP_LOGW("ROOM_MONITOR", "Wi-Fi manager unavailable: %s",
                 esp_err_to_name(wifi_start_error));
    } else if (sender_start_error != ESP_OK) {
        ESP_LOGW("ROOM_MONITOR", "Zabbix sender task unavailable: %s",
                 esp_err_to_name(sender_start_error));
    }

    TickType_t next_sample = xTaskGetTickCount();
    while (true) {
        bmp280_temperature_t sample = {0};
        esp_err_t error = init_error;
        if (error == ESP_OK) {
            error = bmp280_read_temperature(sensor, &sample);
        }
        wifi_manager_publish_snapshot(&sample, error);

        wifi_manager_status_t wifi_status = { .state = WIFI_STATE_ERROR };
        wifi_manager_get_status(&wifi_status);
        zabbix_sender_status_t sender_status = {
            .state = sender_start_error == ESP_OK ? ZABBIX_SEND_WAITING : ZABBIX_SEND_ERROR,
            .last_error = sender_start_error,
        };
        if (sender_start_error == ESP_OK) {
            zabbix_sender_get_status(&sender_status);
        }
        const char *wifi_name = wifi_state_name(wifi_status.state);
        const char *send_state_name = zabbix_send_state_name(sender_status.state);
        const char *wifi_error_name = wifi_manager_failure_name(wifi_status.failure_reason);
        const char *zabbix_stage = zabbix_sender_stage_name(sender_status.last_stage);
        const char *zabbix_error_name = esp_err_to_name(sender_status.last_error);
        char display_status[32];
        snprintf(display_status, sizeof(display_status), "W:%s Z:%s",
                 wifi_status.failure_reason != WIFI_FAILURE_NONE
                     ? wifi_manager_failure_display_name(wifi_status.failure_reason)
                     : wifi_display_state_name(wifi_status.state),
                 zabbix_display_detail(&sender_status));

        int64_t uptime_ms = esp_timer_get_time() / 1000;
        char report[640];
        int length;

        ssd1306_clear(display);
        if (wifi_status.setup_mode) {
            char setup_title[24];
            format_wifi_setup_title(&wifi_status, setup_title, sizeof(setup_title));
            ssd1306_draw_text(display, 0, 0, setup_title, true);
            ssd1306_draw_text(display, 0, 10, wifi_status.setup_ssid, true);
            char setup_password_line[24];
            snprintf(setup_password_line, sizeof(setup_password_line), "PW: %.12s",
                     wifi_status.setup_password);
            ssd1306_draw_text(display, 0, 20, setup_password_line, true);
            ssd1306_draw_text(display, 0, 30, "http://192.168.4.1", true);
            if (error == ESP_OK) {
                char measurement_line[24];
                snprintf(measurement_line, sizeof(measurement_line), "T:%.1fC P:%.0fhPa",
                         sample.temperature_c, sample.pressure_pa / 100.0);
                ssd1306_draw_text(display, 0, 42, measurement_line, true);
            } else {
                ssd1306_draw_text(display, 0, 42, "Sensor error", true);
            }
            char zabbix_line[24];
            snprintf(zabbix_line, sizeof(zabbix_line), "Z:%s",
                     zabbix_display_detail(&sender_status));
            ssd1306_draw_text(display, 0, 54, zabbix_line, true);
        } else {
            ssd1306_draw_text(display, 0, 0, "Server room", true);
        }
        if (error == ESP_OK) {
            if (!wifi_status.setup_mode) {
                char temperature[16];
                char pressure[20];
                snprintf(temperature, sizeof(temperature), "%.1f C", sample.temperature_c);
                snprintf(pressure, sizeof(pressure), "%.1f hPa", sample.pressure_pa / 100.0);
                ssd1306_draw_text_scaled(display, 0, 16, temperature, true, 2);
                ssd1306_draw_text(display, 0, 42, pressure, true);
                ssd1306_draw_text(display, 0, 54, display_status, true);
            }
            length = snprintf(report, sizeof(report),
                              "{\"type\":\"measurement\",\"state\":\"ok\",\"model\":\"BMP280\","
                              "\"address\":118,\"raw_bytes\":\"%02X%02X%02X%02X%02X%02X\","
                               "\"raw_temperature\":%" PRIu32 ",\"raw_pressure\":%" PRIu32 ","
                               "\"status\":%u,\"temperature_c\":%.2f,\"pressure_pa\":%.2f,"
                               "\"t1\":%u,\"t2\":%d,\"t3\":%d,\"uptime_ms\":%" PRId64 ","
                               "\"wifi_state\":\"%s\",\"wifi_error\":\"%s\","
                               "\"wifi_reason_code\":%" PRId32 ","
                               "\"zabbix_send_state\":\"%s\",\"zabbix_stage\":\"%s\","
                               "\"zabbix_error\":\"%s\",\"error\":\"ESP_OK\"}\n",
                               sample.raw_bytes[0], sample.raw_bytes[1], sample.raw_bytes[2],
                               sample.raw_bytes[3], sample.raw_bytes[4], sample.raw_bytes[5],
                               sample.raw, sample.raw_pressure, sample.status,
                               sample.temperature_c, sample.pressure_pa, (unsigned)sample.t1,
                               (int)sample.t2, (int)sample.t3, uptime_ms, wifi_name,
                               wifi_error_name, wifi_status.disconnect_reason, send_state_name,
                               zabbix_stage, zabbix_error_name);
        } else {
            if (!wifi_status.setup_mode) {
                ssd1306_draw_text(display, 0, 16, "Sensor error", true);
            }
            if (sample.raw_available) {
                char raw_temperature[20];
                char raw_pressure[20];
                snprintf(raw_temperature, sizeof(raw_temperature), "T raw: %" PRIu32, sample.raw);
                snprintf(raw_pressure, sizeof(raw_pressure), "P raw: %" PRIu32, sample.raw_pressure);
                if (!wifi_status.setup_mode) {
                    ssd1306_draw_text(display, 0, 30, raw_temperature, true);
                    ssd1306_draw_text(display, 0, 40, raw_pressure, true);
                    ssd1306_draw_text(display, 0, 54, display_status, true);
                }
                length = snprintf(report, sizeof(report),
                                  "{\"type\":\"measurement\",\"state\":\"error\","
                                   "\"raw_bytes\":\"%02X%02X%02X%02X%02X%02X\","
                                   "\"raw_temperature\":%" PRIu32 ",\"raw_pressure\":%" PRIu32 ","
                                   "\"status\":%u,\"temperature_c\":null,\"pressure_pa\":null,"
                                   "\"uptime_ms\":%" PRId64 ",\"wifi_state\":\"%s\","
                                   "\"wifi_error\":\"%s\",\"wifi_reason_code\":%" PRId32 ","
                                   "\"zabbix_send_state\":\"%s\",\"zabbix_stage\":\"%s\","
                                   "\"zabbix_error\":\"%s\","
                                   "\"error\":\"%s\"}\n",
                                   sample.raw_bytes[0], sample.raw_bytes[1], sample.raw_bytes[2],
                                   sample.raw_bytes[3], sample.raw_bytes[4], sample.raw_bytes[5],
                                   sample.raw, sample.raw_pressure, sample.status, uptime_ms,
                                   wifi_name, wifi_error_name, wifi_status.disconnect_reason,
                                   send_state_name, zabbix_stage, zabbix_error_name,
                                   esp_err_to_name(error));
            } else {
                if (!wifi_status.setup_mode) {
                    ssd1306_draw_text(display, 0, 32, esp_err_to_name(error), true);
                    ssd1306_draw_text(display, 0, 54, display_status, true);
                }
                length = snprintf(report, sizeof(report),
                                  "{\"type\":\"measurement\",\"state\":\"error\","
                                   "\"raw_bytes\":null,\"raw_temperature\":null,"
                                   "\"raw_pressure\":null,\"status\":null,"
                                   "\"temperature_c\":null,\"pressure_pa\":null,"
                                    "\"uptime_ms\":%" PRId64 ",\"wifi_state\":\"%s\","
                                    "\"wifi_error\":\"%s\",\"wifi_reason_code\":%" PRId32 ","
                                    "\"zabbix_send_state\":\"%s\",\"zabbix_stage\":\"%s\","
                                    "\"zabbix_error\":\"%s\","
                                    "\"error\":\"%s\"}\n",
                                    uptime_ms, wifi_name, wifi_error_name,
                                    wifi_status.disconnect_reason, send_state_name,
                                    zabbix_stage, zabbix_error_name,
                                    esp_err_to_name(error));
            }
        }
        ssd1306_display(display);

        if (length > 0 && (size_t)length < sizeof(report)) {
            usb_serial_jtag_write_bytes(report, length, pdMS_TO_TICKS(100));
        }
        vTaskDelayUntil(&next_sample, pdMS_TO_TICKS(1000));
    }
}
