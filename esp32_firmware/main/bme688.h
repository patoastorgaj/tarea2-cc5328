#pragma once

// driver minimo del BME688 por I2C: solo T, H y P en modo forzado, sin gas

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

typedef struct {
    uint16_t t1;
    int16_t t2;
    int8_t t3;
    uint16_t p1;
    int16_t p2;
    int8_t p3;
    int16_t p4;
    int16_t p5;
    int8_t p6;
    int8_t p7;
    int16_t p8;
    int16_t p9;
    uint8_t p10;
    uint16_t h1;
    uint16_t h2;
    int8_t h3;
    int8_t h4;
    int8_t h5;
    uint8_t h6;
    int8_t h7;
} bme688_calib_t;

typedef struct {
    i2c_master_dev_handle_t dev;
    uint8_t addr;
    uint8_t variant;
    bme688_calib_t cal;
    uint32_t meas_ms;
} bme688_t;

typedef struct {
    float temperature_c;
    float humidity_pct;
    float pressure_hpa;
} bme688_data_t;

// busca el sensor en 0x77 y 0x76, lo resetea y lo configura
esp_err_t bme688_init(bme688_t *s, i2c_master_bus_handle_t bus);

// libera el dispositivo del bus (para reintentar la inicializacion)
void bme688_deinit(bme688_t *s);

// dispara una medicion forzada y espera a que termine la conversion
esp_err_t bme688_read(bme688_t *s, bme688_data_t *out);
