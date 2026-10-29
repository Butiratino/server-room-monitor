#include "wifi_config.h"

#include <stdbool.h>
#include <string.h>

#include "esp_check.h"
#include "nvs.h"

#ifdef WIFI_CONFIG_TEST_NAMESPACE
#define WIFI_CONFIG_NVS_NAMESPACE "wifi_zx_test"
#else
#define WIFI_CONFIG_NVS_NAMESPACE "wifi_zabbix"
#endif
#define WIFI_CONFIG_NVS_KEY "config"

static size_t bounded_length(const char *value, size_t capacity)
{
    size_t length = 0;
    while (length < capacity && value[length] != '\0') {
        ++length;
    }
    return length;
}

static bool valid_server_character(char value)
{
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '.' || value == '-';
}

esp_err_t wifi_config_validate(const room_monitor_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t ssid_length = bounded_length(config->ssid, sizeof(config->ssid));
    const size_t password_length = bounded_length(config->password, sizeof(config->password));
    const size_t server_length = bounded_length(config->zabbix_server,
                                                sizeof(config->zabbix_server));
    if (ssid_length == 0 || ssid_length > 32 || password_length == sizeof(config->password) ||
        server_length == 0 || server_length > 253 || config->zabbix_port == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Empty passwords allow open Wi-Fi; secured passphrases follow WPA2 limits.
    if (password_length != 0 &&
        (password_length < 8 ||
         (password_length > 63 && password_length != 64))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (password_length == 64) {
        for (size_t i = 0; i < password_length; ++i) {
            char c = config->password[i];
            bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                       (c >= 'A' && c <= 'F');
            if (!hex) {
                return ESP_ERR_INVALID_ARG;
            }
        }
    }

    if (config->zabbix_server[0] == '.' ||
        config->zabbix_server[server_length - 1] == '.') {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < server_length; ++i) {
        if (!valid_server_character(config->zabbix_server[i])) {
            return ESP_ERR_INVALID_ARG;
        }
        if (config->zabbix_server[i] == '.' && i + 1 < server_length &&
            config->zabbix_server[i + 1] == '.') {
            return ESP_ERR_INVALID_ARG;
        }
    }

    return ESP_OK;
}

esp_err_t wifi_config_load(room_monitor_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(config, 0, sizeof(*config));

    nvs_handle_t handle;
    esp_err_t error = nvs_open(WIFI_CONFIG_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (error != ESP_OK) {
        return error == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NVS_NOT_FOUND : error;
    }

    size_t size = sizeof(*config);
    error = nvs_get_blob(handle, WIFI_CONFIG_NVS_KEY, config, &size);
    nvs_close(handle);
    if (error != ESP_OK) {
        memset(config, 0, sizeof(*config));
        return error == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NVS_NOT_FOUND : error;
    }
    if (size != sizeof(*config) || wifi_config_validate(config) != ESP_OK) {
        memset(config, 0, sizeof(*config));
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t wifi_config_save(const room_monitor_config_t *config)
{
    ESP_RETURN_ON_ERROR(wifi_config_validate(config), "WIFI_CONFIG", "Invalid configuration");

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(WIFI_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle),
                        "WIFI_CONFIG", "NVS open failed");
    esp_err_t error = nvs_set_blob(handle, WIFI_CONFIG_NVS_KEY, config, sizeof(*config));
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error;
}

esp_err_t wifi_config_clear(void)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(WIFI_CONFIG_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        return error;
    }

    error = nvs_erase_key(handle, WIFI_CONFIG_NVS_KEY);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        error = ESP_OK;
    }
    if (error == ESP_OK) {
        error = nvs_commit(handle);
    }
    nvs_close(handle);
    return error;
}
