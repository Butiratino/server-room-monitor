#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "driver/usb_serial_jtag.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "bmp280_temperature.h"
#include "unity.h"
#include "unity_test_runner.h"
#include "wifi_config.h"
#include "wifi_manager.h"
#include "wifi_provisioning.h"
#include "zabbix_sender.h"

static room_monitor_config_t valid_config(void)
{
    return (room_monitor_config_t){
        .ssid = "RoomNet",
        .password = "correct-horse-battery",
        .zabbix_server = "192.168.0.95",
        .zabbix_port = 10051,
    };
}

TEST_CASE("config accepts valid fields", "[wifi_config]")
{
    room_monitor_config_t config = valid_config();
    TEST_ASSERT_EQUAL(ESP_OK, wifi_config_validate(&config));
}

TEST_CASE("config rejects empty SSID", "[wifi_config]")
{
    room_monitor_config_t config = valid_config();
    config.ssid[0] = '\0';
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, wifi_config_validate(&config));
}

TEST_CASE("config rejects empty Zabbix address", "[wifi_config]")
{
    room_monitor_config_t config = valid_config();
    config.zabbix_server[0] = '\0';
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, wifi_config_validate(&config));
}

TEST_CASE("config rejects zero Zabbix port", "[wifi_config]")
{
    room_monitor_config_t config = valid_config();
    config.zabbix_port = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, wifi_config_validate(&config));
}

TEST_CASE("unconfigured NVS returns not found", "[wifi_config]")
{
    room_monitor_config_t config = {0};
    TEST_ASSERT_EQUAL(ESP_OK, wifi_config_clear());
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NOT_FOUND, wifi_config_load(&config));
}

TEST_CASE("config survives NVS save and load", "[wifi_config]")
{
    const room_monitor_config_t expected = valid_config();
    room_monitor_config_t actual = {0};
    TEST_ASSERT_EQUAL(ESP_OK, wifi_config_save(&expected));
    TEST_ASSERT_EQUAL(ESP_OK, wifi_config_load(&actual));
    TEST_ASSERT_EQUAL_STRING(expected.ssid, actual.ssid);
    TEST_ASSERT_EQUAL_STRING(expected.password, actual.password);
    TEST_ASSERT_EQUAL_STRING(expected.zabbix_server, actual.zabbix_server);
    TEST_ASSERT_EQUAL_UINT16(expected.zabbix_port, actual.zabbix_port);
    TEST_ASSERT_EQUAL(ESP_OK, wifi_config_clear());
}

TEST_CASE("setup form decodes SSID and defaults sender port", "[wifi_provisioning]")
{
    const char form[] = "ssid=Room+Net&password=correct-horse-battery&zabbix_server=192.168.0.95";
    room_monitor_config_t config = {0};
    TEST_ASSERT_EQUAL(ESP_OK, wifi_provisioning_parse_form(form, strlen(form), &config));
    TEST_ASSERT_EQUAL_STRING("Room Net", config.ssid);
    TEST_ASSERT_EQUAL_STRING("correct-horse-battery", config.password);
    TEST_ASSERT_EQUAL_STRING("192.168.0.95", config.zabbix_server);
    TEST_ASSERT_EQUAL_UINT16(WIFI_CONFIG_DEFAULT_ZABBIX_PORT, config.zabbix_port);
}

TEST_CASE("setup form decodes percent escapes", "[wifi_provisioning]")
{
    const char form[] = "ssid=Room%20Net&password=correct-horse-battery&zabbix_server=zabbix%2Elocal&port=10051";
    room_monitor_config_t config = {0};
    TEST_ASSERT_EQUAL(ESP_OK, wifi_provisioning_parse_form(form, strlen(form), &config));
    TEST_ASSERT_EQUAL_STRING("Room Net", config.ssid);
    TEST_ASSERT_EQUAL_STRING("zabbix.local", config.zabbix_server);
    TEST_ASSERT_EQUAL_UINT16(10051, config.zabbix_port);
}

TEST_CASE("setup form rejects missing server", "[wifi_provisioning]")
{
    const char form[] = "ssid=RoomNet&password=correct-horse-battery";
    room_monitor_config_t config = {0};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      wifi_provisioning_parse_form(form, strlen(form), &config));
}

TEST_CASE("setup form rejects invalid port and malformed escapes", "[wifi_provisioning]")
{
    const char invalid_port[] = "ssid=RoomNet&password=correct-horse-battery&zabbix_server=192.168.0.95&port=0";
    const char malformed_escape[] = "ssid=Room%2GNet&password=correct-horse-battery&zabbix_server=192.168.0.95";
    room_monitor_config_t config = {0};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      wifi_provisioning_parse_form(invalid_port, strlen(invalid_port), &config));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      wifi_provisioning_parse_form(malformed_escape, strlen(malformed_escape), &config));
}

TEST_CASE("provisioning HTTP server starts and stops", "[wifi_provisioning]")
{
    TEST_ASSERT_EQUAL(ESP_OK, wifi_provisioning_start(NULL, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, wifi_provisioning_stop());
}

TEST_CASE("Wi-Fi state transitions from no config to provisioning", "[wifi_manager]")
{
    TEST_ASSERT_EQUAL(WIFI_STATE_PROVISIONING,
                      wifi_manager_transition(WIFI_STATE_UNCONFIGURED,
                                              WIFI_EVENT_CONFIG_MISSING));
}

TEST_CASE("Wi-Fi state transitions from connecting to online", "[wifi_manager]")
{
    TEST_ASSERT_EQUAL(WIFI_STATE_ONLINE,
                      wifi_manager_transition(WIFI_STATE_CONNECTING,
                                              WIFI_EVENT_CONNECTED));
}

TEST_CASE("Wi-Fi disconnect transitions online to retry", "[wifi_manager]")
{
    TEST_ASSERT_EQUAL(WIFI_STATE_RETRY_WAIT,
                      wifi_manager_transition(WIFI_STATE_ONLINE,
                                              WIFI_EVENT_DISCONNECTED));
}

TEST_CASE("PRG setup request transitions any state to provisioning", "[wifi_manager]")
{
    TEST_ASSERT_EQUAL(WIFI_STATE_PROVISIONING,
                      wifi_manager_transition(WIFI_STATE_ONLINE,
                                              WIFI_EVENT_SETUP_REQUESTED));
}

TEST_CASE("Wi-Fi setup failure transitions to error state", "[wifi_manager]")
{
    TEST_ASSERT_EQUAL(WIFI_STATE_ERROR,
                      wifi_manager_transition(WIFI_STATE_CONNECTING,
                                              WIFI_EVENT_ERROR));
}

TEST_CASE("coalesced Wi-Fi events follow current disconnected state", "[wifi_manager]")
{
    TEST_ASSERT_EQUAL(WIFI_STATE_RETRY_WAIT,
                      wifi_manager_resolve_station_events(WIFI_STATE_ONLINE, true, true, false));
}

TEST_CASE("coalesced Wi-Fi events keep current connected state", "[wifi_manager]")
{
    TEST_ASSERT_EQUAL(WIFI_STATE_ONLINE,
                      wifi_manager_resolve_station_events(WIFI_STATE_RETRY_WAIT, true, true, true));
}

TEST_CASE("Wi-Fi auth disconnect reason is named safely", "[wifi_manager]")
{
    wifi_failure_reason_t reason =
        wifi_manager_classify_disconnect_reason(WIFI_REASON_AUTH_FAIL);
    TEST_ASSERT_EQUAL(WIFI_FAILURE_AUTH, reason);
    TEST_ASSERT_EQUAL_STRING("auth_failed", wifi_manager_failure_name(reason));
    TEST_ASSERT_EQUAL_STRING("auth fail", wifi_manager_failure_display_name(reason));
}

TEST_CASE("Wi-Fi missing AP reason is named safely", "[wifi_manager]")
{
    wifi_failure_reason_t reason =
        wifi_manager_classify_disconnect_reason(WIFI_REASON_NO_AP_FOUND);
    TEST_ASSERT_EQUAL(WIFI_FAILURE_NO_AP, reason);
    TEST_ASSERT_EQUAL_STRING("no_ap", wifi_manager_failure_name(reason));
    TEST_ASSERT_EQUAL_STRING("no AP", wifi_manager_failure_display_name(reason));
}

TEST_CASE("sender frame has ZBXD signature and little-endian length", "[zabbix_sender]")
{
    const char payload[] = "{\"x\":1}";
    uint8_t frame[32] = {0};
    size_t frame_length = 0;
    const uint8_t expected_header[] = {'Z', 'B', 'X', 'D', 1, 7, 0, 0, 0, 0, 0, 0, 0};

    TEST_ASSERT_EQUAL(ESP_OK, zabbix_sender_build_frame(payload, strlen(payload),
                                                        frame, sizeof(frame), &frame_length));
    TEST_ASSERT_EQUAL_UINT32(20, frame_length);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_header, frame, sizeof(expected_header));
    TEST_ASSERT_EQUAL_MEMORY(payload, frame + sizeof(expected_header), strlen(payload));
}

TEST_CASE("sender frame rejects insufficient capacity", "[zabbix_sender]")
{
    const char payload[] = "{\"x\":1}";
    uint8_t frame[19] = {0};
    size_t frame_length = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      zabbix_sender_build_frame(payload, strlen(payload),
                                                frame, sizeof(frame), &frame_length));
}

TEST_CASE("sender failure stages have safe display names", "[zabbix_sender]")
{
    TEST_ASSERT_EQUAL_STRING("connect", zabbix_sender_stage_name(ZABBIX_STAGE_CONNECT));
    TEST_ASSERT_EQUAL_STRING("response_header",
                             zabbix_sender_stage_name(ZABBIX_STAGE_RESPONSE_HEADER));
}

TEST_CASE("valid measurement payload has the three trapper keys", "[zabbix_sender]")
{
    bmp280_temperature_t sample = {
        .temperature_c = 24.8,
        .pressure_pa = 102600.0,
        .raw = 500000,
        .raw_pressure = 320000,
        .raw_available = true,
    };
    char json[512];
    size_t length = 0;

    TEST_ASSERT_EQUAL(ESP_OK, zabbix_sender_build_payload(&sample, ESP_OK,
                                                         json, sizeof(json), &length));
    TEST_ASSERT_TRUE(length > 0);
    TEST_ASSERT_NOT_NULL(strstr(json, "\"host\":\"server-room-esp32\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"key\":\"sensor.temperature\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"key\":\"sensor.pressure_pa\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"key\":\"sensor.status\""));
}

TEST_CASE("invalid measurement payload reports status only", "[zabbix_sender]")
{
    bmp280_temperature_t sample = {0};
    char json[256];
    size_t length = 0;

    TEST_ASSERT_EQUAL(ESP_OK, zabbix_sender_build_payload(&sample, ESP_ERR_INVALID_RESPONSE,
                                                         json, sizeof(json), &length));
    TEST_ASSERT_TRUE(length > 0);
    TEST_ASSERT_NOT_NULL(strstr(json, "\"key\":\"sensor.status\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"value\":\"error\""));
    TEST_ASSERT_NULL(strstr(json, "\"key\":\"sensor.temperature\""));
    TEST_ASSERT_NULL(strstr(json, "\"key\":\"sensor.pressure_pa\""));
}

typedef enum {
    MOCK_ZABBIX_SUCCESS,
    MOCK_ZABBIX_BAD_SIGNATURE,
    MOCK_ZABBIX_SILENT,
} mock_zabbix_mode_t;

static volatile bool s_mock_server_ready;
static volatile bool s_mock_server_done;
static esp_err_t s_mock_server_error;
static mock_zabbix_mode_t s_mock_server_mode;

static int mock_send_all(int socket_fd, const uint8_t *data, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        int count = send(socket_fd, data + sent, length - sent, 0);
        if (count <= 0) return -1;
        sent += (size_t)count;
    }
    return 0;
}

static void mock_zabbix_server_task(void *argument)
{
    (void)argument;
    int server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (server_fd < 0) {
        s_mock_server_error = ESP_FAIL;
        s_mock_server_done = true;
        vTaskDelete(NULL);
        return;
    }
    int reuse = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(10051),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(server_fd, 1) < 0) {
        s_mock_server_error = ESP_FAIL;
        close(server_fd);
        s_mock_server_done = true;
        vTaskDelete(NULL);
        return;
    }
    s_mock_server_ready = true;

    struct sockaddr_in peer;
    socklen_t peer_length = sizeof(peer);
    int client_fd = accept(server_fd, (struct sockaddr *)&peer, &peer_length);
    if (client_fd < 0) {
        s_mock_server_error = ESP_FAIL;
    } else if (s_mock_server_mode == MOCK_ZABBIX_SILENT) {
        vTaskDelay(pdMS_TO_TICKS(3500));
    } else {
        const char response[] =
            "{\"response\":\"success\",\"info\":\"processed: 3; failed: 0; total: 3; seconds spent: 0.000\"}";
        uint8_t frame[sizeof(response) + 13];
        memcpy(frame, s_mock_server_mode == MOCK_ZABBIX_BAD_SIGNATURE ? "BAD!\x01" : "ZBXD\x01", 5);
        uint64_t response_length = sizeof(response) - 1;
        for (unsigned i = 0; i < 8; ++i) {
            frame[5 + i] = (uint8_t)(response_length >> (i * 8));
        }
        memcpy(frame + 13, response, response_length);
        if (mock_send_all(client_fd, frame, 2) != 0) {
            s_mock_server_error = ESP_FAIL;
        } else {
            vTaskDelay(pdMS_TO_TICKS(30));
            if (mock_send_all(client_fd, frame + 2, 11 + response_length) != 0) {
                s_mock_server_error = ESP_FAIL;
            }
        }
    }

    if (client_fd >= 0) close(client_fd);
    close(server_fd);
    s_mock_server_done = true;
    vTaskDelete(NULL);
}

static esp_err_t start_mock_zabbix_server(mock_zabbix_mode_t mode)
{
    s_mock_server_ready = false;
    s_mock_server_done = false;
    s_mock_server_error = ESP_OK;
    s_mock_server_mode = mode;
    if (xTaskCreate(mock_zabbix_server_task, "mock_zabbix", 4096, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    for (unsigned i = 0; i < 100 && !s_mock_server_ready && !s_mock_server_done; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return s_mock_server_ready ? ESP_OK : s_mock_server_error;
}

static esp_err_t send_to_local_mock(zabbix_send_stage_t *stage)
{
    room_monitor_config_t config = valid_config();
    strlcpy(config.zabbix_server, "192.168.4.1", sizeof(config.zabbix_server));
    const char request[] = "{\"request\":\"sender data\",\"data\":[]}";
    return zabbix_sender_send_diagnostic(&config, request, strlen(request), stage);
}

static void wait_for_mock_zabbix_server(void)
{
    for (unsigned i = 0; i < 200 && !s_mock_server_done; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

TEST_CASE("sender accepts a fragmented local Zabbix response", "[zabbix_sender]" )
{
    zabbix_send_stage_t stage = ZABBIX_STAGE_NONE;
    TEST_ASSERT_EQUAL(ESP_OK, start_mock_zabbix_server(MOCK_ZABBIX_SUCCESS));
    TEST_ASSERT_EQUAL(ESP_OK, send_to_local_mock(&stage));
    TEST_ASSERT_EQUAL(ZABBIX_STAGE_NONE, stage);
    wait_for_mock_zabbix_server();
    TEST_ASSERT_TRUE(s_mock_server_done);
    TEST_ASSERT_EQUAL(ESP_OK, s_mock_server_error);
}

TEST_CASE("sender labels malformed Zabbix response", "[zabbix_sender]")
{
    zabbix_send_stage_t stage = ZABBIX_STAGE_NONE;
    TEST_ASSERT_EQUAL(ESP_OK, start_mock_zabbix_server(MOCK_ZABBIX_BAD_SIGNATURE));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE, send_to_local_mock(&stage));
    TEST_ASSERT_EQUAL(ZABBIX_STAGE_RESPONSE_HEADER, stage);
    wait_for_mock_zabbix_server();
    TEST_ASSERT_TRUE(s_mock_server_done);
    TEST_ASSERT_EQUAL(ESP_OK, s_mock_server_error);
}

TEST_CASE("sender labels a silent Zabbix response timeout", "[zabbix_sender]")
{
    zabbix_send_stage_t stage = ZABBIX_STAGE_NONE;
    TEST_ASSERT_EQUAL(ESP_OK, start_mock_zabbix_server(MOCK_ZABBIX_SILENT));
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, send_to_local_mock(&stage));
    TEST_ASSERT_EQUAL(ZABBIX_STAGE_RESPONSE_HEADER, stage);
    wait_for_mock_zabbix_server();
    TEST_ASSERT_TRUE(s_mock_server_done);
    TEST_ASSERT_EQUAL(ESP_OK, s_mock_server_error);
}

static void unity_test_task(void *argument)
{
    (void)argument;
    int test_count = unity_get_test_count();
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = UNITY_END();
    char result[48];
    int length = snprintf(result, sizeof(result), "UNITY_DONE tests=%d failures=%d\n",
                          test_count, failures);
    if (length > 0 && (size_t)length < sizeof(result)) {
        usb_serial_jtag_write_bytes(result, length, pdMS_TO_TICKS(100));
    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    usb_serial_jtag_driver_config_t usb = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_config_t ap_config = {
        .ap = {
            .ssid = "UnitTestAP",
            .ssid_len = sizeof("UnitTestAP") - 1,
            .password = "test-pass-123",
            .channel = 1,
            .max_connection = 1,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    BaseType_t test_task = xTaskCreate(unity_test_task, "unity_tests", 10240,
                                       NULL, 5, NULL);
    if (test_task != pdPASS) {
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
