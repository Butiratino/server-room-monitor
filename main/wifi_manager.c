#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "wifi_provisioning.h"

#define TAG "WIFI_MANAGER"
#define SETUP_BUTTON_GPIO GPIO_NUM_0
#define SETUP_HOLD_MS 5000
#define RETRY_MIN_MS 1000
#define RETRY_MAX_MS 60000
#define SETUP_PASSWORD_LENGTH 12

#define EVENT_STA_CONNECTED BIT0
#define EVENT_STA_DISCONNECTED BIT1
#define EVENT_SETUP_REQUESTED BIT2
#define EVENT_CONFIG_SAVED BIT3
#define EVENT_MASK (EVENT_STA_CONNECTED | EVENT_STA_DISCONNECTED | \
                    EVENT_SETUP_REQUESTED | EVENT_CONFIG_SAVED)

static const char *s_setup_alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz23456789";
static EventGroupHandle_t s_events;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;
static portMUX_TYPE s_status_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_snapshot_mux = portMUX_INITIALIZER_UNLOCKED;
static wifi_manager_status_t s_status = { .state = WIFI_STATE_UNCONFIGURED };
static wifi_measurement_snapshot_t s_latest_snapshot;
static room_monitor_config_t s_saved_config;
static bool s_wifi_started;
static bool s_setup_active;
static bool s_config_available;
static bool s_station_enabled;
static bool s_manager_started;

wifi_manager_state_t wifi_manager_transition(wifi_manager_state_t state,
                                             wifi_manager_event_t event)
{
    switch (event) {
    case WIFI_EVENT_CONFIG_MISSING:
    case WIFI_EVENT_SETUP_REQUESTED:
        return WIFI_STATE_PROVISIONING;
    case WIFI_EVENT_CONFIG_SAVED:
    case WIFI_EVENT_RETRY_TIMEOUT:
        return WIFI_STATE_CONNECTING;
    case WIFI_EVENT_CONNECTED:
        return WIFI_STATE_ONLINE;
    case WIFI_EVENT_DISCONNECTED:
        return WIFI_STATE_RETRY_WAIT;
    case WIFI_EVENT_ERROR:
        return WIFI_STATE_ERROR;
    default:
        return state;
    }
}

wifi_manager_state_t wifi_manager_resolve_station_events(wifi_manager_state_t current,
                                                         bool connected_event,
                                                         bool disconnected_event,
                                                         bool station_has_ip)
{
    if (!connected_event && !disconnected_event) {
        return current;
    }
    if (station_has_ip) {
        return WIFI_STATE_ONLINE;
    }
    if (disconnected_event || (connected_event && current == WIFI_STATE_ONLINE)) {
        return WIFI_STATE_RETRY_WAIT;
    }
    return current;
}

wifi_failure_reason_t wifi_manager_classify_disconnect_reason(int32_t reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return WIFI_FAILURE_NO_AP;
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_AUTH_LEAVE:
    case WIFI_REASON_802_1X_AUTH_FAILED:
    case WIFI_REASON_ASSOC_NOT_AUTHED:
    case WIFI_REASON_BAD_CIPHER_OR_AKM:
    case WIFI_REASON_GROUP_CIPHER_INVALID:
    case WIFI_REASON_PAIRWISE_CIPHER_INVALID:
        return WIFI_FAILURE_AUTH;
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_ASSOC_TOOMANY:
        return WIFI_FAILURE_ASSOC;
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
        return WIFI_FAILURE_HANDSHAKE;
    case WIFI_REASON_BEACON_TIMEOUT:
        return WIFI_FAILURE_BEACON_LOST;
    case WIFI_REASON_CONNECTION_FAIL:
        return WIFI_FAILURE_CONNECTION;
    default:
        return WIFI_FAILURE_OTHER;
    }
}

const char *wifi_manager_failure_name(wifi_failure_reason_t reason)
{
    switch (reason) {
    case WIFI_FAILURE_NONE: return "none";
    case WIFI_FAILURE_NO_AP: return "no_ap";
    case WIFI_FAILURE_AUTH: return "auth_failed";
    case WIFI_FAILURE_ASSOC: return "association_failed";
    case WIFI_FAILURE_HANDSHAKE: return "handshake_timeout";
    case WIFI_FAILURE_BEACON_LOST: return "beacon_lost";
    case WIFI_FAILURE_CONNECTION: return "connection_failed";
    case WIFI_FAILURE_DRIVER: return "driver_error";
    case WIFI_FAILURE_CONFIGURATION: return "configuration_error";
    default: return "other";
    }
}

const char *wifi_manager_failure_display_name(wifi_failure_reason_t reason)
{
    switch (reason) {
    case WIFI_FAILURE_NO_AP: return "no AP";
    case WIFI_FAILURE_AUTH: return "auth fail";
    case WIFI_FAILURE_ASSOC: return "assoc fail";
    case WIFI_FAILURE_HANDSHAKE: return "handshake";
    case WIFI_FAILURE_BEACON_LOST: return "beacon lost";
    case WIFI_FAILURE_CONNECTION: return "connect fail";
    case WIFI_FAILURE_DRIVER: return "driver error";
    case WIFI_FAILURE_CONFIGURATION: return "config error";
    case WIFI_FAILURE_NONE: return "";
    default: return "Wi-Fi error";
    }
}

static void set_state(wifi_manager_state_t state, esp_err_t last_error)
{
    portENTER_CRITICAL(&s_status_mux);
    s_status.state = state;
    s_status.last_error = last_error;
    if (state == WIFI_STATE_ERROR && s_status.failure_reason == WIFI_FAILURE_NONE) {
        s_status.failure_reason = WIFI_FAILURE_DRIVER;
    }
    portEXIT_CRITICAL(&s_status_mux);
}

static void set_failure_reason(wifi_failure_reason_t reason)
{
    portENTER_CRITICAL(&s_status_mux);
    s_status.failure_reason = reason;
    portEXIT_CRITICAL(&s_status_mux);
}

static void set_setup_status(const char *ssid, const char *password)
{
    portENTER_CRITICAL(&s_status_mux);
    s_status.state = WIFI_STATE_PROVISIONING;
    s_status.setup_mode = true;
    strlcpy(s_status.setup_ssid, ssid, sizeof(s_status.setup_ssid));
    strlcpy(s_status.setup_password, password, sizeof(s_status.setup_password));
    strlcpy(s_status.station_ip, "192.168.4.1", sizeof(s_status.station_ip));
    s_status.last_error = ESP_OK;
    portEXIT_CRITICAL(&s_status_mux);
}

static void report_setup_credentials(const char *ssid, const char *password)
{
    char report[128];
    int length = snprintf(report, sizeof(report),
                          "{\"type\":\"wifi_setup\",\"ssid\":\"%s\","
                          "\"setup_password\":\"%s\",\"url\":\"http://192.168.4.1\"}\n",
                          ssid, password);
    if (length > 0 && (size_t)length < sizeof(report)) {
        usb_serial_jtag_write_bytes(report, length, pdMS_TO_TICKS(100));
    }
}

static void provisioning_saved(void *context)
{
    (void)context;
    if (s_events != NULL) {
        xEventGroupSetBits(s_events, EVENT_CONFIG_SAVED);
    }
}

static void wifi_event_handler(void *argument, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)argument;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED &&
        s_events != NULL) {
        const wifi_event_sta_disconnected_t *disconnected = event_data;
        int32_t reason = disconnected != NULL ? disconnected->reason : WIFI_REASON_UNSPECIFIED;
        portENTER_CRITICAL(&s_status_mux);
        s_status.disconnect_reason = reason;
        s_status.failure_reason = wifi_manager_classify_disconnect_reason(reason);
        s_status.last_error = ESP_ERR_WIFI_CONN;
        portEXIT_CRITICAL(&s_status_mux);
        xEventGroupSetBits(s_events, EVENT_STA_DISCONNECTED);
    }
}

static void ip_event_handler(void *argument, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    (void)argument;
    (void)event_data;
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP && s_events != NULL) {
        xEventGroupSetBits(s_events, EVENT_STA_CONNECTED);
    }
}

static void generate_setup_credentials(char *ssid, size_t ssid_capacity,
                                       char *password, size_t password_capacity)
{
    uint8_t mac[6] = {0};
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    snprintf(ssid, ssid_capacity, "RoomMonitor-%02X%02X%02X", mac[3], mac[4], mac[5]);

    const size_t alphabet_length = strlen(s_setup_alphabet);
    size_t length = password_capacity - 1;
    if (length > SETUP_PASSWORD_LENGTH) {
        length = SETUP_PASSWORD_LENGTH;
    }
    for (size_t i = 0; i < length; ++i) {
        password[i] = s_setup_alphabet[esp_random() % alphabet_length];
    }
    password[length] = '\0';
}

static esp_err_t start_setup_mode(void)
{
    if (s_setup_active) {
        wifi_provisioning_stop();
    }

    char ssid[33];
    char password[17];
    generate_setup_credentials(ssid, sizeof(ssid), password, sizeof(password));

    // GPIO0/PRG is sampled by ROM at reset; only use it as a button after boot.
    if (s_wifi_started && s_station_enabled) {
        esp_wifi_disconnect();
    }
    s_station_enabled = false;
    wifi_config_t ap_config = {0};
    strlcpy((char *)ap_config.ap.ssid, ssid, sizeof(ap_config.ap.ssid));
    strlcpy((char *)ap_config.ap.password, password, sizeof(ap_config.ap.password));
    ap_config.ap.ssid_len = strlen(ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 2;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_config.ap.pmf_cfg.capable = true;
    ap_config.ap.pmf_cfg.required = false;

    esp_err_t error = esp_wifi_set_mode(WIFI_MODE_AP);
    if (error == ESP_OK) {
        error = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    }
    if (error == ESP_OK && !s_wifi_started) {
        error = esp_wifi_start();
        s_wifi_started = error == ESP_OK;
    }
    if (error == ESP_OK) {
        error = wifi_provisioning_start(provisioning_saved, NULL);
    }
    if (error != ESP_OK) {
        set_state(WIFI_STATE_ERROR, error);
        ESP_LOGE(TAG, "Setup AP start failed: %s", esp_err_to_name(error));
        memset(password, 0, sizeof(password));
        return error;
    }

    s_setup_active = true;
    set_setup_status(ssid, password);
    report_setup_credentials(ssid, password);
    memset(password, 0, sizeof(password));
    return ESP_OK;
}

static esp_err_t start_station_mode(bool keep_setup_ap)
{
    wifi_config_t sta_config = {0};
    strlcpy((char *)sta_config.sta.ssid, s_saved_config.ssid,
            sizeof(sta_config.sta.ssid));
    strlcpy((char *)sta_config.sta.password, s_saved_config.password,
            sizeof(sta_config.sta.password));
    sta_config.sta.threshold.authmode = s_saved_config.password[0] == '\0'
                                           ? WIFI_AUTH_OPEN
                                           : WIFI_AUTH_WPA2_PSK;
    sta_config.sta.pmf_cfg.capable = true;
    sta_config.sta.pmf_cfg.required = false;

    esp_err_t error = esp_wifi_set_mode(keep_setup_ap ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    if (error == ESP_OK) {
        error = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    }
    if (error == ESP_OK) {
        s_station_enabled = true;
    }
    if (error == ESP_OK && !s_wifi_started) {
        error = esp_wifi_start();
        s_wifi_started = error == ESP_OK;
    }
    if (error == ESP_OK) {
        error = esp_wifi_connect();
    }
    if (error == ESP_OK) {
        s_config_available = true;
        portENTER_CRITICAL(&s_status_mux);
        s_status.failure_reason = WIFI_FAILURE_NONE;
        s_status.disconnect_reason = 0;
        portEXIT_CRITICAL(&s_status_mux);
        set_state(WIFI_STATE_CONNECTING, ESP_OK);
    } else {
        set_state(WIFI_STATE_ERROR, error);
        ESP_LOGE(TAG, "Station start failed: %s", esp_err_to_name(error));
    }
    return error;
}

static void update_station_status(void)
{
    esp_netif_ip_info_t ip_info;
    char ip[16] = {0};
    if (s_sta_netif != NULL && esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK) {
        esp_ip4addr_ntoa(&ip_info.ip, ip, sizeof(ip));
    }
    wifi_ap_record_t ap_info = {0};
    int8_t rssi = 0;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
    }
    portENTER_CRITICAL(&s_status_mux);
    strlcpy(s_status.station_ip, ip, sizeof(s_status.station_ip));
    s_status.rssi = rssi;
    s_status.last_error = ESP_OK;
    s_status.failure_reason = WIFI_FAILURE_NONE;
    s_status.disconnect_reason = 0;
    portEXIT_CRITICAL(&s_status_mux);
}

static bool station_has_ip(void)
{
    wifi_ap_record_t ap_info = {0};
    esp_netif_ip_info_t ip_info = {0};
    return s_sta_netif != NULL && esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK &&
           esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0;
}

static void handle_station_connected(void)
{
    update_station_status();
    if (s_setup_active) {
        wifi_provisioning_stop();
        esp_wifi_set_mode(WIFI_MODE_STA);
        s_setup_active = false;
        portENTER_CRITICAL(&s_status_mux);
        s_status.setup_mode = false;
        s_status.setup_ssid[0] = '\0';
        s_status.setup_password[0] = '\0';
        portEXIT_CRITICAL(&s_status_mux);
    }
    set_state(WIFI_STATE_ONLINE, ESP_OK);
}

static void manager_task(void *argument)
{
    (void)argument;
    const EventBits_t mask = EVENT_MASK;
    uint32_t retry_delay_ms = RETRY_MIN_MS;
    int64_t retry_at_us = 0;
    bool button_pressed = false;
    bool button_latched = false;
    TickType_t button_started = 0;

    for (;;) {
        EventBits_t events = xEventGroupWaitBits(s_events, mask, pdTRUE, pdFALSE,
                                                 pdMS_TO_TICKS(100));

        if ((events & EVENT_SETUP_REQUESTED) != 0) {
            esp_err_t error = start_setup_mode();
            retry_at_us = 0;
            retry_delay_ms = RETRY_MIN_MS;
            if (error != ESP_OK) {
                ESP_LOGE(TAG, "Could not enter setup mode: %s", esp_err_to_name(error));
            }
        }

        if ((events & EVENT_CONFIG_SAVED) != 0) {
            esp_err_t error = wifi_config_load(&s_saved_config);
            if (error == ESP_OK) {
                s_config_available = true;
                retry_delay_ms = RETRY_MIN_MS;
                retry_at_us = 0;
                start_station_mode(true);
            } else {
                set_state(WIFI_STATE_ERROR, error);
                set_failure_reason(WIFI_FAILURE_CONFIGURATION);
                ESP_LOGE(TAG, "Saved network configuration unavailable: %s",
                         esp_err_to_name(error));
            }
        }

        bool connected_event = (events & EVENT_STA_CONNECTED) != 0;
        bool disconnected_event = (events & EVENT_STA_DISCONNECTED) != 0;
        if ((connected_event || disconnected_event) && s_config_available &&
            s_station_enabled) {
            wifi_manager_state_t resolved_state = wifi_manager_resolve_station_events(
                wifi_manager_get_state(), connected_event, disconnected_event,
                station_has_ip());
            if (resolved_state == WIFI_STATE_ONLINE) {
                retry_delay_ms = RETRY_MIN_MS;
                retry_at_us = 0;
                handle_station_connected();
            } else if (resolved_state == WIFI_STATE_RETRY_WAIT) {
                set_state(WIFI_STATE_RETRY_WAIT, ESP_ERR_WIFI_CONN);
                retry_at_us = esp_timer_get_time() + (int64_t)retry_delay_ms * 1000;
                retry_delay_ms = retry_delay_ms >= RETRY_MAX_MS / 2
                                     ? RETRY_MAX_MS
                                     : retry_delay_ms * 2;
            }
        }

        int64_t now_us = esp_timer_get_time();
        if (s_config_available && s_station_enabled && retry_at_us != 0 &&
            now_us >= retry_at_us) {
            esp_err_t error = esp_wifi_connect();
            if (error == ESP_OK) {
                set_state(WIFI_STATE_CONNECTING, ESP_OK);
            } else {
                portENTER_CRITICAL(&s_status_mux);
                s_status.last_error = error;
                portEXIT_CRITICAL(&s_status_mux);
                retry_at_us = now_us + (int64_t)retry_delay_ms * 1000;
                retry_delay_ms = retry_delay_ms >= RETRY_MAX_MS / 2
                                     ? RETRY_MAX_MS
                                     : retry_delay_ms * 2;
            }
        }

        bool pressed = gpio_get_level(GPIO_NUM_0) == 0;
        if (pressed && !button_pressed) {
            button_pressed = true;
            button_latched = false;
            button_started = xTaskGetTickCount();
        } else if (!pressed) {
            button_pressed = false;
            button_latched = false;
        } else if (!button_latched &&
                   (xTaskGetTickCount() - button_started) >= pdMS_TO_TICKS(SETUP_HOLD_MS)) {
            button_latched = true;
            xEventGroupSetBits(s_events, EVENT_SETUP_REQUESTED);
        }
    }
}

esp_err_t wifi_manager_start(void)
{
    if (s_manager_started) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t error = nvs_flash_init();
    if (error != ESP_OK) {
        set_state(WIFI_STATE_ERROR, error);
        set_failure_reason(WIFI_FAILURE_CONFIGURATION);
        return error;
    }
    error = esp_netif_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        set_state(WIFI_STATE_ERROR, error);
        return error;
    }
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        set_state(WIFI_STATE_ERROR, error);
        return error;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_sta_netif == NULL || s_ap_netif == NULL) {
        set_state(WIFI_STATE_ERROR, ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        set_state(WIFI_STATE_ERROR, ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    error = esp_wifi_init(&wifi_init);
    if (error == ESP_OK) {
        error = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    }
    if (error != ESP_OK) {
        set_state(WIFI_STATE_ERROR, error);
        return error;
    }
    error = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                 wifi_event_handler, NULL,
                                                 &s_wifi_handler);
    if (error == ESP_OK) {
        error = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                     ip_event_handler, NULL,
                                                     &s_ip_handler);
    }
    if (error != ESP_OK) {
        set_state(WIFI_STATE_ERROR, error);
        return error;
    }

    gpio_config_t prg_config = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    error = gpio_config(&prg_config);
    if (error != ESP_OK) {
        set_state(WIFI_STATE_ERROR, error);
        return error;
    }

    error = wifi_config_load(&s_saved_config);
    if (error == ESP_OK) {
        s_config_available = true;
        error = start_station_mode(false);
    } else if (error == ESP_ERR_NVS_NOT_FOUND || error == ESP_ERR_INVALID_STATE) {
        s_config_available = false;
        error = start_setup_mode();
    } else {
        ESP_LOGE(TAG, "Network settings could not be loaded: %s", esp_err_to_name(error));
        s_config_available = false;
        error = start_setup_mode();
    }
    if (error != ESP_OK) {
        return error;
    }

    BaseType_t task_result = xTaskCreate(manager_task, "wifi_manager", 4096, NULL, 5, NULL);
    if (task_result != pdPASS) {
        set_state(WIFI_STATE_ERROR, ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    s_manager_started = true;
    return ESP_OK;
}

wifi_manager_state_t wifi_manager_get_state(void)
{
    portENTER_CRITICAL(&s_status_mux);
    wifi_manager_state_t state = s_status.state;
    portEXIT_CRITICAL(&s_status_mux);
    return state;
}

esp_err_t wifi_manager_get_status(wifi_manager_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_status_mux);
    *status = s_status;
    portEXIT_CRITICAL(&s_status_mux);
    return ESP_OK;
}

esp_err_t wifi_manager_get_config(room_monitor_config_t *config)
{
    return wifi_config_load(config);
}

void wifi_manager_request_setup(void)
{
    if (s_events != NULL) {
        xEventGroupSetBits(s_events, EVENT_SETUP_REQUESTED);
    }
}

void wifi_manager_publish_snapshot(const bmp280_temperature_t *sample,
                                   esp_err_t sensor_error)
{
    if (sample == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_snapshot_mux);
    s_latest_snapshot.sample = *sample;
    s_latest_snapshot.sensor_error = sensor_error;
    s_latest_snapshot.available = true;
    portEXIT_CRITICAL(&s_snapshot_mux);
}

esp_err_t wifi_manager_get_latest_snapshot(wifi_measurement_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_snapshot_mux);
    bool available = s_latest_snapshot.available;
    if (available) {
        *snapshot = s_latest_snapshot;
    }
    portEXIT_CRITICAL(&s_snapshot_mux);
    return available ? ESP_OK : ESP_ERR_NOT_FOUND;
}
