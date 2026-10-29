#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "wifi_config.h"

typedef void (*wifi_provisioning_saved_cb_t)(void *context);

// Parse application/x-www-form-urlencoded fields without logging credentials.
esp_err_t wifi_provisioning_parse_form(const char *body, size_t body_length,
                                       room_monitor_config_t *config);

// Starts/stops the local setup page; Wi-Fi AP mode is owned by wifi_manager.
esp_err_t wifi_provisioning_start(wifi_provisioning_saved_cb_t saved_callback,
                                  void *context);
esp_err_t wifi_provisioning_stop(void);
