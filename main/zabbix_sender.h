#pragma once

#include <stddef.h>
#include <stdint.h>

#include "bmp280_temperature.h"
#include "esp_err.h"
#include "wifi_config.h"

typedef enum {
    ZABBIX_SEND_WAITING = 0,
    ZABBIX_SEND_OK,
    ZABBIX_SEND_ERROR,
} zabbix_send_state_t;

typedef enum {
    ZABBIX_STAGE_NONE = 0,
    ZABBIX_STAGE_CONFIG,
    ZABBIX_STAGE_BUILD,
    ZABBIX_STAGE_RESOLVE,
    ZABBIX_STAGE_CONNECT,
    ZABBIX_STAGE_SEND,
    ZABBIX_STAGE_RESPONSE_HEADER,
    ZABBIX_STAGE_RESPONSE_BODY,
    ZABBIX_STAGE_VALIDATE,
} zabbix_send_stage_t;

typedef struct {
    zabbix_send_state_t state;
    esp_err_t last_error;
    zabbix_send_stage_t last_stage;
    uint32_t successful_batches;
    int64_t last_success_uptime_ms;
} zabbix_sender_status_t;

esp_err_t zabbix_sender_build_payload(const bmp280_temperature_t *sample,
                                      esp_err_t sensor_error,
                                      char *json, size_t capacity, size_t *length);
esp_err_t zabbix_sender_build_frame(const char *json, size_t length,
                                    uint8_t *frame, size_t capacity,
                                    size_t *frame_length);
esp_err_t zabbix_sender_send(const room_monitor_config_t *config,
                             const char *json, size_t length);
esp_err_t zabbix_sender_send_diagnostic(const room_monitor_config_t *config,
                                        const char *json, size_t length,
                                        zabbix_send_stage_t *failed_stage);
const char *zabbix_sender_stage_name(zabbix_send_stage_t stage);
esp_err_t zabbix_sender_start(void);
esp_err_t zabbix_sender_get_status(zabbix_sender_status_t *status);
