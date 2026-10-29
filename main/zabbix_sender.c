#include "zabbix_sender.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "wifi_manager.h"

#define TAG "ZABBIX_SENDER"
#define ZABBIX_HOST_NAME "server-room-esp32"
#define ZABBIX_HEADER_SIZE 13
#define ZABBIX_MAX_PAYLOAD 1024
#define ZABBIX_IO_TIMEOUT_MS 3000
#define ZABBIX_SEND_INTERVAL_MS 10000

const char *zabbix_sender_stage_name(zabbix_send_stage_t stage)
{
    switch (stage) {
    case ZABBIX_STAGE_NONE: return "none";
    case ZABBIX_STAGE_CONFIG: return "config";
    case ZABBIX_STAGE_BUILD: return "build";
    case ZABBIX_STAGE_RESOLVE: return "resolve";
    case ZABBIX_STAGE_CONNECT: return "connect";
    case ZABBIX_STAGE_SEND: return "send";
    case ZABBIX_STAGE_RESPONSE_HEADER: return "response_header";
    case ZABBIX_STAGE_RESPONSE_BODY: return "response_body";
    case ZABBIX_STAGE_VALIDATE: return "validate";
    default: return "unknown";
    }
}

esp_err_t zabbix_sender_build_payload(const bmp280_temperature_t *sample,
                                      esp_err_t sensor_error,
                                      char *json, size_t capacity, size_t *length)
{
    if (json == NULL || length == NULL || capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int written;
    if (sensor_error != ESP_OK) {
        written = snprintf(json, capacity,
                           "{\"request\":\"sender data\",\"data\":["
                           "{\"host\":\"%s\",\"key\":\"sensor.status\",\"value\":\"error\"}]}",
                           ZABBIX_HOST_NAME);
    } else {
        if (sample == NULL || !isfinite(sample->temperature_c) ||
            !isfinite(sample->pressure_pa)) {
            return ESP_ERR_INVALID_ARG;
        }
        written = snprintf(json, capacity,
                           "{\"request\":\"sender data\",\"data\":["
                           "{\"host\":\"%s\",\"key\":\"sensor.temperature\",\"value\":\"%.2f\"},"
                           "{\"host\":\"%s\",\"key\":\"sensor.pressure_pa\",\"value\":\"%.2f\"},"
                           "{\"host\":\"%s\",\"key\":\"sensor.status\",\"value\":\"ok\"}]}",
                           ZABBIX_HOST_NAME, sample->temperature_c,
                           ZABBIX_HOST_NAME, sample->pressure_pa,
                           ZABBIX_HOST_NAME);
    }
    if (written < 0 || (size_t)written >= capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    *length = (size_t)written;
    return ESP_OK;
}

esp_err_t zabbix_sender_build_frame(const char *json, size_t length,
                                    uint8_t *frame, size_t capacity,
                                    size_t *frame_length)
{
    if (json == NULL || length == 0 || frame == NULL || frame_length == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (length > SIZE_MAX - ZABBIX_HEADER_SIZE ||
        capacity < ZABBIX_HEADER_SIZE + length) {
        return ESP_ERR_INVALID_SIZE;
    }

    static const uint8_t signature[] = {'Z', 'B', 'X', 'D', 1};
    memcpy(frame, signature, sizeof(signature));
    uint64_t payload_length = length;
    for (unsigned i = 0; i < 8; ++i) {
        frame[5 + i] = (uint8_t)(payload_length >> (i * 8));
    }
    memcpy(frame + ZABBIX_HEADER_SIZE, json, length);
    *frame_length = ZABBIX_HEADER_SIZE + length;
    return ESP_OK;
}

static esp_err_t wait_socket(int socket_fd, bool wait_write)
{
    fd_set read_set;
    fd_set write_set;
    FD_ZERO(&read_set);
    FD_ZERO(&write_set);
    if (wait_write) {
        FD_SET(socket_fd, &write_set);
    } else {
        FD_SET(socket_fd, &read_set);
    }
    struct timeval timeout = {
        .tv_sec = ZABBIX_IO_TIMEOUT_MS / 1000,
        .tv_usec = (ZABBIX_IO_TIMEOUT_MS % 1000) * 1000,
    };
    int result = select(socket_fd + 1, wait_write ? NULL : &read_set,
                        wait_write ? &write_set : NULL, NULL, &timeout);
    if (result == 0) return ESP_ERR_TIMEOUT;
    return result < 0 ? ESP_FAIL : ESP_OK;
}

static esp_err_t connect_socket(int socket_fd, const struct sockaddr *address,
                                socklen_t address_length)
{
    int old_flags = fcntl(socket_fd, F_GETFL, 0);
    if (old_flags < 0 || fcntl(socket_fd, F_SETFL, old_flags | O_NONBLOCK) < 0) {
        return ESP_FAIL;
    }

    int result = connect(socket_fd, address, address_length);
    if (result == 0) return ESP_OK;
    if (errno != EINPROGRESS) return ESP_FAIL;

    esp_err_t error = wait_socket(socket_fd, true);
    if (error != ESP_OK) return error;
    int socket_error = 0;
    socklen_t option_length = sizeof(socket_error);
    if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &socket_error, &option_length) < 0 ||
        socket_error != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t send_all(int socket_fd, const uint8_t *data, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        esp_err_t error = wait_socket(socket_fd, true);
        if (error != ESP_OK) return error;
        int count = send(socket_fd, data + sent, length - sent, 0);
        if (count > 0) {
            sent += (size_t)count;
        } else if (count < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static esp_err_t receive_all(int socket_fd, uint8_t *data, size_t length)
{
    size_t received = 0;
    while (received < length) {
        esp_err_t error = wait_socket(socket_fd, false);
        if (error != ESP_OK) return error;
        int count = recv(socket_fd, data + received, length - received, 0);
        if (count > 0) {
            received += (size_t)count;
        } else if (count == 0) {
            return ESP_ERR_INVALID_RESPONSE;
        } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

esp_err_t zabbix_sender_send_diagnostic(const room_monitor_config_t *config,
                                        const char *json, size_t length,
                                        zabbix_send_stage_t *failed_stage)
{
    if (failed_stage != NULL) {
        *failed_stage = ZABBIX_STAGE_NONE;
    }
    if (config == NULL || json == NULL || length == 0 || length > ZABBIX_MAX_PAYLOAD) {
        if (failed_stage != NULL) *failed_stage = ZABBIX_STAGE_CONFIG;
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t error = wifi_config_validate(config);
    if (error != ESP_OK) {
        if (failed_stage != NULL) *failed_stage = ZABBIX_STAGE_CONFIG;
        return error;
    }

    uint8_t frame[ZABBIX_HEADER_SIZE + ZABBIX_MAX_PAYLOAD];
    size_t frame_length = 0;
    error = zabbix_sender_build_frame(json, length, frame, sizeof(frame), &frame_length);
    if (error != ESP_OK) {
        if (failed_stage != NULL) *failed_stage = ZABBIX_STAGE_BUILD;
        return error;
    }

    char service[6];
    snprintf(service, sizeof(service), "%u", (unsigned)config->zabbix_port);
    struct addrinfo hints = {
        .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *addresses = NULL;
    int gai_error = getaddrinfo(config->zabbix_server, service, &hints, &addresses);
    if (gai_error != 0 || addresses == NULL) {
        if (failed_stage != NULL) *failed_stage = ZABBIX_STAGE_RESOLVE;
        return ESP_ERR_NOT_FOUND;
    }

    int socket_fd = -1;
    error = ESP_FAIL;
    for (struct addrinfo *address = addresses; address != NULL; address = address->ai_next) {
        socket_fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket_fd < 0) continue;
        error = connect_socket(socket_fd, address->ai_addr, address->ai_addrlen);
        if (error == ESP_OK) break;
        close(socket_fd);
        socket_fd = -1;
    }
    freeaddrinfo(addresses);
    if (socket_fd < 0) {
        if (failed_stage != NULL) *failed_stage = ZABBIX_STAGE_CONNECT;
        return error;
    }

    error = send_all(socket_fd, frame, frame_length);
    if (error != ESP_OK && failed_stage != NULL) {
        *failed_stage = ZABBIX_STAGE_SEND;
    }
    uint8_t response_header[ZABBIX_HEADER_SIZE];
    if (error == ESP_OK) {
        error = receive_all(socket_fd, response_header, sizeof(response_header));
        if (error != ESP_OK && failed_stage != NULL) {
            *failed_stage = ZABBIX_STAGE_RESPONSE_HEADER;
        }
    }
    if (error == ESP_OK && memcmp(response_header, "ZBXD\x01", 5) != 0) {
        error = ESP_ERR_INVALID_RESPONSE;
        if (failed_stage != NULL) *failed_stage = ZABBIX_STAGE_RESPONSE_HEADER;
    }

    uint64_t response_length = 0;
    if (error == ESP_OK) {
        for (unsigned i = 0; i < 8; ++i) {
            response_length |= (uint64_t)response_header[5 + i] << (i * 8);
        }
        if (response_length == 0 || response_length >= ZABBIX_MAX_PAYLOAD) {
            error = ESP_ERR_INVALID_SIZE;
            if (failed_stage != NULL) *failed_stage = ZABBIX_STAGE_RESPONSE_HEADER;
        }
    }

    char response[ZABBIX_MAX_PAYLOAD];
    if (error == ESP_OK) {
        error = receive_all(socket_fd, (uint8_t *)response, response_length);
        if (error != ESP_OK && failed_stage != NULL) {
            *failed_stage = ZABBIX_STAGE_RESPONSE_BODY;
        }
    }
    if (error == ESP_OK) {
        response[response_length] = '\0';
        if (strstr(response, "\"response\":\"success\"") == NULL ||
            strstr(response, "failed: 0") == NULL) {
            error = ESP_ERR_INVALID_RESPONSE;
            if (failed_stage != NULL) *failed_stage = ZABBIX_STAGE_VALIDATE;
        }
    }
    close(socket_fd);
    return error;
}

esp_err_t zabbix_sender_send(const room_monitor_config_t *config,
                             const char *json, size_t length)
{
    return zabbix_sender_send_diagnostic(config, json, length, NULL);
}

static portMUX_TYPE s_status_mux = portMUX_INITIALIZER_UNLOCKED;
static zabbix_sender_status_t s_status = {
    .state = ZABBIX_SEND_WAITING,
    .last_error = ESP_OK,
};
static bool s_sender_started;

static void update_sender_status(zabbix_send_state_t state, esp_err_t error,
                                 zabbix_send_stage_t stage, bool success)
{
    portENTER_CRITICAL(&s_status_mux);
    s_status.state = state;
    s_status.last_error = error;
    s_status.last_stage = stage;
    if (success) {
        ++s_status.successful_batches;
        s_status.last_success_uptime_ms = esp_timer_get_time() / 1000;
    }
    portEXIT_CRITICAL(&s_status_mux);
}

static void sender_task(void *argument)
{
    (void)argument;
    bool was_online = false;
    TickType_t next_send = xTaskGetTickCount();

    for (;;) {
        if (wifi_manager_get_state() != WIFI_STATE_ONLINE) {
            was_online = false;
            update_sender_status(ZABBIX_SEND_WAITING, ESP_OK, ZABBIX_STAGE_NONE, false);
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        if (!was_online) {
            next_send = xTaskGetTickCount();
            was_online = true;
        }

        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(now - next_send) >= 0) {
            wifi_measurement_snapshot_t snapshot;
            room_monitor_config_t config;
            zabbix_send_stage_t failed_stage = ZABBIX_STAGE_NONE;
            esp_err_t error = wifi_manager_get_latest_snapshot(&snapshot);
            if (error == ESP_OK) {
                error = wifi_manager_get_config(&config);
                if (error != ESP_OK) failed_stage = ZABBIX_STAGE_CONFIG;
            }
            char json[512];
            size_t json_length = 0;
            if (error == ESP_OK) {
                error = zabbix_sender_build_payload(&snapshot.sample, snapshot.sensor_error,
                                                    json, sizeof(json), &json_length);
                if (error != ESP_OK) failed_stage = ZABBIX_STAGE_BUILD;
            }
            if (error == ESP_OK) {
                error = zabbix_sender_send_diagnostic(&config, json, json_length,
                                                      &failed_stage);
            }
            if (error == ESP_OK) {
                update_sender_status(ZABBIX_SEND_OK, ESP_OK, ZABBIX_STAGE_NONE, true);
            } else if (error == ESP_ERR_NOT_FOUND && failed_stage == ZABBIX_STAGE_NONE) {
                update_sender_status(ZABBIX_SEND_WAITING, error, ZABBIX_STAGE_NONE, false);
            } else {
                update_sender_status(ZABBIX_SEND_ERROR, error, failed_stage, false);
                ESP_LOGW(TAG, "Sender failed at %s: %s",
                         zabbix_sender_stage_name(failed_stage), esp_err_to_name(error));
            }
            next_send = xTaskGetTickCount() + pdMS_TO_TICKS(ZABBIX_SEND_INTERVAL_MS);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

esp_err_t zabbix_sender_start(void)
{
    if (s_sender_started) return ESP_ERR_INVALID_STATE;
    BaseType_t result = xTaskCreate(sender_task, "zabbix_sender", 6144, NULL, 4, NULL);
    if (result != pdPASS) return ESP_ERR_NO_MEM;
    s_sender_started = true;
    return ESP_OK;
}

esp_err_t zabbix_sender_get_status(zabbix_sender_status_t *status)
{
    if (status == NULL) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_status_mux);
    *status = s_status;
    portEXIT_CRITICAL(&s_status_mux);
    return ESP_OK;
}
