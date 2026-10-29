#include "wifi_provisioning.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_http_server.h"

#define WIFI_FORM_MAX_LENGTH 1024
#define WIFI_FORM_KEY_CAPACITY 32
#define WIFI_FORM_VALUE_CAPACITY WIFI_CONFIG_SERVER_CAPACITY
#define WIFI_PROVISIONING_TAG "WIFI_PROVISION"

static httpd_handle_t s_http_server;
static wifi_provisioning_saved_cb_t s_saved_callback;
static void *s_saved_context;

static const char setup_page[] =
    "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Room monitor setup</title></head><body><h1>Room monitor Wi-Fi setup</h1>"
    "<form method='post' action='/save'>"
    "<label>Wi-Fi name <input name='ssid' maxlength='32' required></label><br>"
    "<label>Wi-Fi password <input name='password' type='password' maxlength='64'></label><br>"
    "<label>Zabbix server IP or host <input name='zabbix_server' maxlength='253' required></label><br>"
    "<label>Zabbix port <input name='port' type='number' min='1' max='65535' value='10051'></label><br>"
    "<button type='submit'>Save and connect</button></form></body></html>";

static const char form_error_page[] =
    "<!doctype html><html><body><h1>Invalid settings</h1>"
    "Check Wi-Fi name, Zabbix server address, and port, then return to the setup page."
    "</body></html>";

static const char save_error_page[] =
    "<!doctype html><html><body><h1>Could not save settings</h1>"
    "Return to the setup page and try again.</body></html>";

static const char saved_page[] =
    "<!doctype html><html><body><h1>Settings saved</h1>"
    "The device is connecting. Check its OLED or USB status.</body></html>";

static esp_err_t setup_get_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_send(request, setup_page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t setup_post_handler(httpd_req_t *request)
{
    if (request->content_len == 0 || request->content_len > WIFI_FORM_MAX_LENGTH) {
        httpd_resp_set_status(request, "400 Bad Request");
        httpd_resp_set_type(request, "text/html; charset=utf-8");
        return httpd_resp_send(request, form_error_page, HTTPD_RESP_USE_STRLEN);
    }

    const size_t body_length = request->content_len;
    char *body = malloc(body_length + 1);
    if (body == NULL) {
        httpd_resp_set_status(request, "500 Internal Server Error");
        return httpd_resp_send(request, save_error_page, HTTPD_RESP_USE_STRLEN);
    }

    size_t received = 0;
    while (received < body_length) {
        int count = httpd_req_recv(request, body + received, body_length - received);
        if (count <= 0) {
            memset(body, 0, body_length + 1);
            free(body);
            httpd_resp_set_status(request, "400 Bad Request");
            httpd_resp_set_type(request, "text/html; charset=utf-8");
            return httpd_resp_send(request, form_error_page, HTTPD_RESP_USE_STRLEN);
        }
        received += (size_t)count;
    }
    body[body_length] = '\0';

    room_monitor_config_t config;
    esp_err_t error = wifi_provisioning_parse_form(body, body_length, &config);
    memset(body, 0, body_length + 1);
    free(body);

    if (error == ESP_OK) {
        error = wifi_config_save(&config);
    }
    memset(config.password, 0, sizeof(config.password));
    if (error != ESP_OK) {
        httpd_resp_set_status(request, "400 Bad Request");
        httpd_resp_set_type(request, "text/html; charset=utf-8");
        return httpd_resp_send(request, form_error_page, HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_set_type(request, "text/html; charset=utf-8");
    esp_err_t response_error = httpd_resp_send(request, saved_page, HTTPD_RESP_USE_STRLEN);
    if (s_saved_callback != NULL) {
        s_saved_callback(s_saved_context);
    }
    return response_error;
}

esp_err_t wifi_provisioning_start(wifi_provisioning_saved_cb_t saved_callback,
                                  void *context)
{
    if (s_http_server != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 4;
    config.lru_purge_enable = true;
    ESP_RETURN_ON_ERROR(httpd_start(&s_http_server, &config), WIFI_PROVISIONING_TAG,
                        "HTTP server start failed");

    const httpd_uri_t get_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = setup_get_handler,
    };
    const httpd_uri_t post_uri = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = setup_post_handler,
    };
    esp_err_t error = httpd_register_uri_handler(s_http_server, &get_uri);
    if (error == ESP_OK) {
        error = httpd_register_uri_handler(s_http_server, &post_uri);
    }
    if (error != ESP_OK) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
        return error;
    }

    s_saved_callback = saved_callback;
    s_saved_context = context;
    return ESP_OK;
}

esp_err_t wifi_provisioning_stop(void)
{
    if (s_http_server == NULL) {
        return ESP_OK;
    }
    esp_err_t error = httpd_stop(s_http_server);
    if (error == ESP_OK) {
        s_http_server = NULL;
        s_saved_callback = NULL;
        s_saved_context = NULL;
    }
    return error;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static esp_err_t decode_component(const char *source, size_t length,
                                  char *destination, size_t capacity)
{
    if (destination == NULL || capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t output = 0;
    for (size_t input = 0; input < length; ++input) {
        unsigned char value = (unsigned char)source[input];
        if (value == '+') {
            value = ' ';
        } else if (value == '%') {
            if (input + 2 >= length) {
                return ESP_ERR_INVALID_ARG;
            }
            int high = hex_value(source[input + 1]);
            int low = hex_value(source[input + 2]);
            if (high < 0 || low < 0) {
                return ESP_ERR_INVALID_ARG;
            }
            value = (unsigned char)((high << 4) | low);
            input += 2;
        }
        if (value == '\0' || value < 0x20 || value == 0x7f || output + 1 >= capacity) {
            return ESP_ERR_INVALID_ARG;
        }
        destination[output++] = (char)value;
    }
    destination[output] = '\0';
    return ESP_OK;
}

static esp_err_t assign_field(room_monitor_config_t *config, const char *key, const char *value,
                              bool *ssid_seen, bool *password_seen,
                              bool *server_seen, bool *port_seen)
{
    if (strcmp(key, "ssid") == 0) {
        if (*ssid_seen || strlen(value) >= sizeof(config->ssid)) return ESP_ERR_INVALID_ARG;
        memcpy(config->ssid, value, strlen(value) + 1);
        *ssid_seen = true;
    } else if (strcmp(key, "password") == 0) {
        if (*password_seen || strlen(value) >= sizeof(config->password)) return ESP_ERR_INVALID_ARG;
        memcpy(config->password, value, strlen(value) + 1);
        *password_seen = true;
    } else if (strcmp(key, "zabbix_server") == 0) {
        if (*server_seen || strlen(value) >= sizeof(config->zabbix_server)) return ESP_ERR_INVALID_ARG;
        memcpy(config->zabbix_server, value, strlen(value) + 1);
        *server_seen = true;
    } else if (strcmp(key, "port") == 0) {
        if (*port_seen || value[0] == '\0') return ESP_ERR_INVALID_ARG;
        for (size_t i = 0; value[i] != '\0'; ++i) {
            if (value[i] < '0' || value[i] > '9') return ESP_ERR_INVALID_ARG;
        }
        char *end = NULL;
        unsigned long port = strtoul(value, &end, 10);
        if (end == value || *end != '\0' || port == 0 || port > UINT16_MAX) {
            return ESP_ERR_INVALID_ARG;
        }
        config->zabbix_port = (uint16_t)port;
        *port_seen = true;
    }
    return ESP_OK;
}

esp_err_t wifi_provisioning_parse_form(const char *body, size_t body_length,
                                       room_monitor_config_t *config)
{
    if (body == NULL || config == NULL || body_length == 0 ||
        body_length > WIFI_FORM_MAX_LENGTH) {
        return ESP_ERR_INVALID_ARG;
    }

    *config = (room_monitor_config_t){ .zabbix_port = WIFI_CONFIG_DEFAULT_ZABBIX_PORT };
    bool ssid_seen = false;
    bool password_seen = false;
    bool server_seen = false;
    bool port_seen = false;
    size_t position = 0;

    while (position < body_length) {
        size_t key_start = position;
        while (position < body_length && body[position] != '=' && body[position] != '&') {
            ++position;
        }
        if (position == body_length || body[position] != '=') return ESP_ERR_INVALID_ARG;
        size_t key_length = position - key_start;
        ++position;

        size_t value_start = position;
        while (position < body_length && body[position] != '&') ++position;
        size_t value_length = position - value_start;
        if (position < body_length) ++position;

        char key[WIFI_FORM_KEY_CAPACITY];
        char value[WIFI_FORM_VALUE_CAPACITY];
        ESP_RETURN_ON_ERROR(decode_component(body + key_start, key_length, key, sizeof(key)),
                            "WIFI_PROVISION", "Invalid form key encoding");
        ESP_RETURN_ON_ERROR(decode_component(body + value_start, value_length, value, sizeof(value)),
                            "WIFI_PROVISION", "Invalid form value encoding");
        ESP_RETURN_ON_ERROR(assign_field(config, key, value, &ssid_seen, &password_seen,
                                         &server_seen, &port_seen),
                            "WIFI_PROVISION", "Invalid form field");
    }

    if (!ssid_seen || !server_seen) return ESP_ERR_INVALID_ARG;
    return wifi_config_validate(config);
}
