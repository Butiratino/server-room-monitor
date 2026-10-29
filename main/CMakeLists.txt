# ВЫБЕРИ ПРОШИВКУ:
# main.c               — OLED-терминал
# sensor_probe.c       — поиск датчика BMP280
# temperature_monitor.c — температура и давление BMP280
# wifi_zabbix_test.c   — Unity-тесты Wi-Fi и Zabbix

set(FIRMWARE "temperature_monitor.c")

if(FIRMWARE STREQUAL "temperature_monitor.c")
    set(APP_SOURCES
        "temperature_monitor.c"
        "bmp280_temperature.c"
        "wifi_config.c"
        "wifi_provisioning.c"
        "wifi_manager.c"
        "zabbix_sender.c"
    )
    set(APP_REQUIRES
        chill-sam__ssd1306
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_usb_serial_jtag
        esp_event
        esp_http_server
        esp_netif
        esp_timer
        esp_wifi
        lwip
        nvs_flash
    )
elseif(FIRMWARE STREQUAL "wifi_zabbix_test.c")
    set(APP_SOURCES
        "wifi_zabbix_test.c"
        "wifi_config.c"
        "wifi_provisioning.c"
        "wifi_manager.c"
        "zabbix_sender.c"
    )
    set(APP_REQUIRES unity esp_driver_usb_serial_jtag esp_driver_i2c esp_timer lwip nvs_flash esp_driver_gpio esp_event esp_netif esp_wifi esp_http_server)
else()
    set(APP_SOURCES "${FIRMWARE}")
    set(APP_REQUIRES "")
endif()

idf_component_register(
    SRCS ${APP_SOURCES}
    INCLUDE_DIRS "."
    REQUIRES ${APP_REQUIRES}
)

if(FIRMWARE STREQUAL "wifi_zabbix_test.c")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE WIFI_CONFIG_TEST_NAMESPACE)
endif()
