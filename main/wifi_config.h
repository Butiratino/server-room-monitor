#pragma once

#include <stdint.h>

#include "esp_err.h"

#define WIFI_CONFIG_SSID_CAPACITY 33
#define WIFI_CONFIG_PASSWORD_CAPACITY 65
#define WIFI_CONFIG_SERVER_CAPACITY 254
#define WIFI_CONFIG_DEFAULT_ZABBIX_PORT 10051

typedef struct {
    char ssid[WIFI_CONFIG_SSID_CAPACITY];
    char password[WIFI_CONFIG_PASSWORD_CAPACITY];
    char zabbix_server[WIFI_CONFIG_SERVER_CAPACITY];
    uint16_t zabbix_port;
} room_monitor_config_t;

esp_err_t wifi_config_validate(const room_monitor_config_t *config);
esp_err_t wifi_config_load(room_monitor_config_t *config);
esp_err_t wifi_config_save(const room_monitor_config_t *config);
esp_err_t wifi_config_clear(void);
