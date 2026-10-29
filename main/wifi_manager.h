#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bmp280_temperature.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "wifi_config.h"

typedef enum {
    WIFI_STATE_UNCONFIGURED = 0,
    WIFI_STATE_PROVISIONING,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_ONLINE,
    WIFI_STATE_RETRY_WAIT,
    WIFI_STATE_ERROR,
} wifi_manager_state_t;

typedef enum {
    WIFI_EVENT_CONFIG_MISSING = 0,
    WIFI_EVENT_CONFIG_SAVED,
    WIFI_EVENT_CONNECTED,
    WIFI_EVENT_DISCONNECTED,
    WIFI_EVENT_SETUP_REQUESTED,
    WIFI_EVENT_RETRY_TIMEOUT,
    WIFI_EVENT_ERROR,
} wifi_manager_event_t;

typedef enum {
    WIFI_FAILURE_NONE = 0,
    WIFI_FAILURE_NO_AP,
    WIFI_FAILURE_AUTH,
    WIFI_FAILURE_ASSOC,
    WIFI_FAILURE_HANDSHAKE,
    WIFI_FAILURE_BEACON_LOST,
    WIFI_FAILURE_CONNECTION,
    WIFI_FAILURE_DRIVER,
    WIFI_FAILURE_CONFIGURATION,
    WIFI_FAILURE_OTHER,
} wifi_failure_reason_t;

typedef struct {
    wifi_manager_state_t state;
    bool setup_mode;
    char setup_ssid[33];
    char setup_password[17];
    char station_ip[16];
    int8_t rssi;
    esp_err_t last_error;
    wifi_failure_reason_t failure_reason;
    int32_t disconnect_reason;
} wifi_manager_status_t;

typedef struct {
    bmp280_temperature_t sample;
    esp_err_t sensor_error;
    bool available;
} wifi_measurement_snapshot_t;

wifi_manager_state_t wifi_manager_transition(wifi_manager_state_t state,
                                             wifi_manager_event_t event);
wifi_manager_state_t wifi_manager_resolve_station_events(wifi_manager_state_t current,
                                                         bool connected_event,
                                                         bool disconnected_event,
                                                         bool station_has_ip);
wifi_failure_reason_t wifi_manager_classify_disconnect_reason(int32_t reason);
const char *wifi_manager_failure_name(wifi_failure_reason_t reason);
const char *wifi_manager_failure_display_name(wifi_failure_reason_t reason);
esp_err_t wifi_manager_start(void);
wifi_manager_state_t wifi_manager_get_state(void);
esp_err_t wifi_manager_get_status(wifi_manager_status_t *status);
esp_err_t wifi_manager_get_config(room_monitor_config_t *config);
void wifi_manager_request_setup(void);
void wifi_manager_publish_snapshot(const bmp280_temperature_t *sample,
                                  esp_err_t sensor_error);
esp_err_t wifi_manager_get_latest_snapshot(wifi_measurement_snapshot_t *snapshot);
