#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

typedef struct {
    double temperature_c;
    double pressure_pa;
    uint32_t raw;
    uint32_t raw_pressure;
    uint8_t raw_bytes[6];
    uint8_t status;
    bool raw_available;
    uint16_t t1;
    int16_t t2;
    int16_t t3;
} bmp280_temperature_t;

// Takes a fresh forced-mode measurement. Raw data remains available when compensation fails.
esp_err_t bmp280_read_temperature(i2c_master_dev_handle_t device,
                                  bmp280_temperature_t *sample);
