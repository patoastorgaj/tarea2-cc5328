#include "bme688.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// registros (datasheet BME688, seccion 5)
#define REG_CHIP_ID     0xD0
#define REG_RESET       0xE0
#define REG_VARIANT_ID  0xF0
#define REG_CTRL_GAS_0  0x70
#define REG_CTRL_GAS_1  0x71
#define REG_CTRL_HUM    0x72
#define REG_CTRL_MEAS   0x74
#define REG_CONFIG      0x75
#define REG_FIELD0      0x1D
#define REG_COEFF1      0x8A
#define REG_COEFF2      0xE1
#define REG_COEFF3      0x00

#define CHIP_ID         0x61
#define RESET_CMD       0xB6
#define LEN_COEFF1      23
#define LEN_COEFF2      14
#define LEN_COEFF3      5
#define LEN_FIELD       10

// oversampling: T x2, P x4, H x1
#define OSRS_T 2
#define OSRS_P 3
#define OSRS_H 1
#define MODE_SLEEP  0x00
#define MODE_FORCED 0x01

#define I2C_TIMEOUT_MS 50

static esp_err_t read_regs(bme688_t *s, uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s->dev, &reg, 1, buf, len, I2C_TIMEOUT_MS);
}

static esp_err_t write_reg(bme688_t *s, uint8_t reg, uint8_t val)
{
    uint8_t b[2] = {reg, val};
    return i2c_master_transmit(s->dev, b, sizeof(b), I2C_TIMEOUT_MS);
}

static uint16_t u16(uint8_t msb, uint8_t lsb)
{
    return (uint16_t)(((uint16_t)msb << 8) | lsb);
}

static esp_err_t read_calibration(bme688_t *s)
{
    uint8_t c[LEN_COEFF1 + LEN_COEFF2 + LEN_COEFF3];
    esp_err_t err = read_regs(s, REG_COEFF1, c, LEN_COEFF1);
    if (err == ESP_OK) {
        err = read_regs(s, REG_COEFF2, c + LEN_COEFF1, LEN_COEFF2);
    }
    if (err == ESP_OK) {
        err = read_regs(s, REG_COEFF3, c + LEN_COEFF1 + LEN_COEFF2, LEN_COEFF3);
    }
    if (err != ESP_OK) {
        return err;
    }

    // indices segun el orden 0x8A..0xA0, 0xE1..0xEE, 0x00..0x04
    bme688_calib_t *k = &s->cal;
    k->t1 = u16(c[32], c[31]);
    k->t2 = (int16_t)u16(c[1], c[0]);
    k->t3 = (int8_t)c[2];
    k->p1 = u16(c[5], c[4]);
    k->p2 = (int16_t)u16(c[7], c[6]);
    k->p3 = (int8_t)c[8];
    k->p4 = (int16_t)u16(c[11], c[10]);
    k->p5 = (int16_t)u16(c[13], c[12]);
    k->p7 = (int8_t)c[14];
    k->p6 = (int8_t)c[15];
    k->p8 = (int16_t)u16(c[19], c[18]);
    k->p9 = (int16_t)u16(c[21], c[20]);
    k->p10 = c[22];
    k->h1 = (uint16_t)(((uint16_t)c[25] << 4) | (c[24] & 0x0F));
    k->h2 = (uint16_t)(((uint16_t)c[23] << 4) | (c[24] >> 4));
    k->h3 = (int8_t)c[26];
    k->h4 = (int8_t)c[27];
    k->h5 = (int8_t)c[28];
    k->h6 = c[29];
    k->h7 = (int8_t)c[30];
    return ESP_OK;
}

// duracion de la conversion TPH segun la API de Bosch
static uint32_t measurement_ms(void)
{
    static const uint8_t cycles[] = {0, 1, 2, 4, 8, 16};
    uint32_t meas_cycles = cycles[OSRS_T] + cycles[OSRS_P] + cycles[OSRS_H];
    uint32_t us = meas_cycles * 1963u + 477u * 4u + 477u * 5u + 1000u;
    return us / 1000u + 2u;
}

static esp_err_t try_address(bme688_t *s, i2c_master_bus_handle_t bus, uint8_t addr)
{
    if (i2c_master_probe(bus, addr, I2C_TIMEOUT_MS) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = CONFIG_APP_I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s->dev);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t id = 0;
    err = read_regs(s, REG_CHIP_ID, &id, 1);
    if (err != ESP_OK || id != CHIP_ID) {
        bme688_deinit(s);
        return err != ESP_OK ? err : ESP_ERR_INVALID_RESPONSE;
    }
    s->addr = addr;
    return ESP_OK;
}

esp_err_t bme688_init(bme688_t *s, i2c_master_bus_handle_t bus)
{
    bme688_deinit(s);
    memset(s, 0, sizeof(*s));

    esp_err_t err = try_address(s, bus, 0x77);
    if (err != ESP_OK) {
        err = try_address(s, bus, 0x76);
    }
    if (err != ESP_OK) {
        return err;
    }

    err = write_reg(s, REG_RESET, RESET_CMD);
    vTaskDelay(pdMS_TO_TICKS(10));
    if (err == ESP_OK) {
        err = read_regs(s, REG_VARIANT_ID, &s->variant, 1);
    }
    if (err == ESP_OK) {
        err = read_calibration(s);
    }
    // calefactor apagado y sin medicion de gas
    if (err == ESP_OK) {
        err = write_reg(s, REG_CTRL_GAS_0, 0x08);
    }
    if (err == ESP_OK) {
        err = write_reg(s, REG_CTRL_GAS_1, 0x00);
    }
    // filtro IIR desactivado para no retrasar los picos
    if (err == ESP_OK) {
        err = write_reg(s, REG_CONFIG, 0x00);
    }
    if (err == ESP_OK) {
        err = write_reg(s, REG_CTRL_HUM, OSRS_H);
    }
    if (err == ESP_OK) {
        err = write_reg(s, REG_CTRL_MEAS, (OSRS_T << 5) | (OSRS_P << 2) | MODE_SLEEP);
    }
    if (err != ESP_OK) {
        bme688_deinit(s);
        return err;
    }
    s->meas_ms = measurement_ms();
    return ESP_OK;
}

void bme688_deinit(bme688_t *s)
{
    if (s->dev) {
        i2c_master_bus_rm_device(s->dev);
        s->dev = NULL;
    }
}

// compensacion en punto flotante (formulas de la BME68x SensorAPI)
static float comp_temperature(const bme688_calib_t *k, uint32_t adc, float *t_fine)
{
    float var1 = (((float)adc / 16384.0f) - ((float)k->t1 / 1024.0f)) * (float)k->t2;
    float var2 = ((float)adc / 131072.0f) - ((float)k->t1 / 8192.0f);
    var2 = var2 * var2 * ((float)k->t3 * 16.0f);
    *t_fine = var1 + var2;
    return *t_fine / 5120.0f;
}

static float comp_pressure(const bme688_calib_t *k, uint32_t adc, float t_fine)
{
    float var1 = (t_fine / 2.0f) - 64000.0f;
    float var2 = var1 * var1 * ((float)k->p6 / 131072.0f);
    var2 = var2 + (var1 * (float)k->p5 * 2.0f);
    var2 = (var2 / 4.0f) + ((float)k->p4 * 65536.0f);
    var1 = ((((float)k->p3 * var1 * var1) / 16384.0f) + ((float)k->p2 * var1)) / 524288.0f;
    var1 = (1.0f + (var1 / 32768.0f)) * (float)k->p1;
    float p = 1048576.0f - (float)adc;
    if ((int)var1 == 0) {
        return 0.0f;
    }
    p = ((p - (var2 / 4096.0f)) * 6250.0f) / var1;
    var1 = ((float)k->p9 * p * p) / 2147483648.0f;
    var2 = p * ((float)k->p8 / 32768.0f);
    float var3 = (p / 256.0f) * (p / 256.0f) * (p / 256.0f) * ((float)k->p10 / 131072.0f);
    p = p + (var1 + var2 + var3 + ((float)k->p7 * 128.0f)) / 16.0f;
    return p;  // Pa
}

static float comp_humidity(const bme688_calib_t *k, uint16_t adc, float t_fine)
{
    float temp = t_fine / 5120.0f;
    float var1 = (float)adc - (((float)k->h1 * 16.0f) + (((float)k->h3 / 2.0f) * temp));
    float var2 = var1 * (((float)k->h2 / 262144.0f) *
                         (1.0f + (((float)k->h4 / 16384.0f) * temp) +
                          (((float)k->h5 / 1048576.0f) * temp * temp)));
    float var3 = (float)k->h6 / 16384.0f;
    float var4 = (float)k->h7 / 2097152.0f;
    float h = var2 + ((var3 + (var4 * temp)) * var2 * var2);
    if (h > 100.0f) {
        h = 100.0f;
    } else if (h < 0.0f) {
        h = 0.0f;
    }
    return h;
}

esp_err_t bme688_read(bme688_t *s, bme688_data_t *out)
{
    if (!s->dev) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = write_reg(s, REG_CTRL_MEAS, (OSRS_T << 5) | (OSRS_P << 2) | MODE_FORCED);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(s->meas_ms));

    // se espera el bit new_data por si la conversion se alarga
    uint8_t f[LEN_FIELD];
    for (int tries = 0; tries < 10; tries++) {
        err = read_regs(s, REG_FIELD0, f, LEN_FIELD);
        if (err != ESP_OK) {
            return err;
        }
        if (f[0] & 0x80) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (!(f[0] & 0x80)) {
        return ESP_ERR_TIMEOUT;
    }

    uint32_t adc_p = ((uint32_t)f[2] << 12) | ((uint32_t)f[3] << 4) | (f[4] >> 4);
    uint32_t adc_t = ((uint32_t)f[5] << 12) | ((uint32_t)f[6] << 4) | (f[7] >> 4);
    uint16_t adc_h = u16(f[8], f[9]);

    // 0x80000 es el valor de reset: la conversion no se hizo
    if (adc_t == 0x80000 || adc_p == 0x80000) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    float t_fine;
    float t = comp_temperature(&s->cal, adc_t, &t_fine);
    float p = comp_pressure(&s->cal, adc_p, t_fine) / 100.0f;
    float h = comp_humidity(&s->cal, adc_h, t_fine);

    // rangos de operacion del datasheet
    if (t < -40.0f || t > 85.0f || p < 300.0f || p > 1100.0f) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    out->temperature_c = t;
    out->pressure_hpa = p;
    out->humidity_pct = h;
    return ESP_OK;
}
