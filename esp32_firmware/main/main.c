#include <math.h>
#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app_config.h"
#include "bme688.h"
#include "processing.h"
#include "protocol.h"

#define FW_NAME "t2-bme688"
#define FW_VERSION "1.0"

#define SERIAL_PORT UART_NUM_0
#define TX_BUF_LEN 384
#define REINIT_PERIOD_MS 2000
#define MAX_READ_FAILS 3

static app_config_t s_cfg;
static SemaphoreHandle_t s_cfg_mtx;
static SemaphoreHandle_t s_tx_mtx;
static TaskHandle_t s_sensor_task;
static i2c_master_bus_handle_t s_bus;
static volatile bool s_sensor_ok;
static volatile esp_err_t s_sensor_err = ESP_ERR_INVALID_STATE;

// estado del procesamiento, solo lo usa la tarea del sensor
static bme688_t s_sensor;
static temp_history_t s_hist;
static peak_detector_t s_peak;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

// envia una linea completa sin que se mezcle con otra tarea
static void send_line(const char *line, int len)
{
    if (len < 0) {
        return;
    }
    xSemaphoreTake(s_tx_mtx, portMAX_DELAY);
    uart_write_bytes(SERIAL_PORT, line, (size_t)len);
    uart_write_bytes(SERIAL_PORT, "\n", 1);
    xSemaphoreGive(s_tx_mtx);
}

static app_config_t cfg_get(void)
{
    xSemaphoreTake(s_cfg_mtx, portMAX_DELAY);
    app_config_t c = s_cfg;
    xSemaphoreGive(s_cfg_mtx);
    return c;
}

static void cfg_set(const app_config_t *c)
{
    xSemaphoreTake(s_cfg_mtx, portMAX_DELAY);
    s_cfg = *c;
    xSemaphoreGive(s_cfg_mtx);
}

static void send_status(void)
{
    char buf[TX_BUF_LEN];
    int len;
    if (s_sensor_ok) {
        len = snprintf(buf, sizeof(buf),
                       "{\"type\":\"status\",\"fw\":\"%s\",\"version\":\"%s\",\"sensor\":\"ok\","
                       "\"addr\":\"0x%02X\",\"variant\":\"%s\",\"t_ms\":%lld}",
                       FW_NAME, FW_VERSION, s_sensor.addr,
                       s_sensor.variant == 0x01 ? "BME688" : "BME680", (long long)now_ms());
    } else {
        len = snprintf(buf, sizeof(buf),
                       "{\"type\":\"status\",\"fw\":\"%s\",\"version\":\"%s\",\"sensor\":\"error\","
                       "\"detail\":\"%s\",\"t_ms\":%lld}",
                       FW_NAME, FW_VERSION, esp_err_to_name(s_sensor_err), (long long)now_ms());
    }
    send_line(buf, len);
}

// redondeo a la resolucion transmitida (0.01 C) para que el PC vea lo mismo que se procesa
static float round2(float x)
{
    return roundf(x * 100.0f) / 100.0f;
}

static void sensor_task(void *arg)
{
    char buf[TX_BUF_LEN];
    app_config_t prev = cfg_get();
    bool sensor_ok = false;
    int fails = 0;
    int64_t last_init_try = -REINIT_PERIOD_MS;
    uint32_t seq = 0;
    uint32_t peak_seq = 0;

    th_init(&s_hist);
    peak_reset(&s_peak);

    TickType_t last_tick = xTaskGetTickCount();
    while (1) {
        app_config_t c = cfg_get();

        // al cambiar el umbral o reactivar la deteccion se parte de cero
        if (c.peak_enabled != prev.peak_enabled || c.peak_threshold_c != prev.peak_threshold_c ||
            c.peak_hysteresis_c != prev.peak_hysteresis_c) {
            peak_reset(&s_peak);
        }
        prev = c;

        if (!sensor_ok && now_ms() - last_init_try >= REINIT_PERIOD_MS) {
            last_init_try = now_ms();
            s_sensor_err = bme688_init(&s_sensor, s_bus);
            sensor_ok = (s_sensor_err == ESP_OK);
            s_sensor_ok = sensor_ok;
            fails = 0;
            send_status();
        }

        sample_msg_t msg = {0};
        bme688_data_t d;
        last_tick = xTaskGetTickCount();
        msg.t_ms = now_ms();
        msg.seq = seq++;

        if (!sensor_ok) {
            msg.valid = false;
            msg.error = "sensor_not_ready";
        } else if (bme688_read(&s_sensor, &d) != ESP_OK) {
            msg.valid = false;
            msg.error = "read_failed";
            peak_reset(&s_peak);  // se pierde la continuidad de las tres muestras
            if (++fails >= MAX_READ_FAILS) {
                sensor_ok = false;
                s_sensor_ok = false;
                s_sensor_err = ESP_FAIL;
                send_status();
            }
        } else {
            fails = 0;
            msg.valid = true;
            msg.temp_c = round2(d.temperature_c);
            msg.hum_pct = d.humidity_pct;
            msg.press_hpa = d.pressure_hpa;
            // la temperatura se adquiere siempre, aunque no se transmita
            th_push(&s_hist, msg.t_ms, msg.temp_c);
        }

        if (c.avg_enabled && msg.valid) {
            msg.avg_window_s = c.avg_window_s;
            msg.has_avg = th_mean(&s_hist, msg.t_ms, c.avg_window_s, &msg.avg);
        }

        peak_event_t ev;
        bool peak = msg.valid && c.peak_enabled &&
                    peak_update(&s_peak, msg.t_ms, msg.temp_c, c.peak_threshold_c, c.peak_hysteresis_c, &ev);

        if (c.streaming) {
            send_line(buf, protocol_fmt_sample(buf, sizeof(buf), &msg, c.vars));
            if (peak) {
                send_line(buf, protocol_fmt_peak(buf, sizeof(buf), ++peak_seq, &ev));
            }
        }

        // espera hasta el siguiente periodo; un cambio de config recalcula el plazo
        TickType_t next = last_tick + pdMS_TO_TICKS(app_config_period_ms(&c));
        while (1) {
            TickType_t now = xTaskGetTickCount();
            if ((int32_t)(next - now) <= 0) {
                break;
            }
            if (ulTaskNotifyTake(pdTRUE, next - now) > 0) {
                app_config_t nc = cfg_get();
                next = last_tick + pdMS_TO_TICKS(app_config_period_ms(&nc));
            }
        }
    }
}

static void handle_line(const char *line)
{
    char buf[TX_BUF_LEN];
    cmd_parse_t cmd;
    app_config_t cur = cfg_get();

    if (!protocol_parse(line, &cur, &cmd)) {
        send_line(buf, protocol_fmt_error(buf, sizeof(buf), cmd.id, cmd.err_code, cmd.err_field, cmd.err_msg));
        return;
    }

    switch (cmd.kind) {
    case CMD_CONFIG:
    case CMD_START:
    case CMD_STOP:
        // se aplica todo junto, nunca a medias
        cfg_set(&cmd.cfg);
        xTaskNotifyGive(s_sensor_task);
        send_line(buf, protocol_fmt_config(buf, sizeof(buf), "ack", protocol_cmd_name(cmd.kind), cmd.id, &cmd.cfg));
        break;
    case CMD_GET_CONFIG:
        send_line(buf, protocol_fmt_config(buf, sizeof(buf), "ack", "get_config", cmd.id, &cur));
        send_status();
        break;
    case CMD_PING: {
        int len = snprintf(buf, sizeof(buf), "{\"type\":\"pong\",\"t_ms\":%lld%s%s%s}",
                           (long long)now_ms(), cmd.id[0] ? ",\"id\":\"" : "", cmd.id, cmd.id[0] ? "\"" : "");
        send_line(buf, len);
        break;
    }
    case CMD_SELFTEST: {
        selftest_result_t r;
        processing_selftest(&r);
        send_line(buf, protocol_fmt_selftest(buf, sizeof(buf), cmd.id, &r));
        break;
    }
    default:
        break;
    }
}

static void command_task(void *arg)
{
    char line[PROTO_MAX_LINE];
    uint8_t rx[64];
    int len = 0;
    bool overflow = false;

    while (1) {
        int n = uart_read_bytes(SERIAL_PORT, rx, sizeof(rx), pdMS_TO_TICKS(100));
        for (int i = 0; i < n; i++) {
            char ch = (char)rx[i];
            if (ch == '\r') {
                continue;
            }
            if (ch != '\n') {
                if (len < PROTO_MAX_LINE - 1) {
                    line[len++] = ch;
                } else {
                    overflow = true;
                }
                continue;
            }
            if (overflow) {
                char buf[TX_BUF_LEN];
                send_line(buf, protocol_fmt_error(buf, sizeof(buf), "", "line_too_long", "", "linea sobre 255 bytes"));
            } else if (len > 0) {
                line[len] = '\0';
                handle_line(line);
            }
            len = 0;
            overflow = false;
        }
    }
}

void app_main(void)
{
    s_cfg_mtx = xSemaphoreCreateMutex();
    s_tx_mtx = xSemaphoreCreateMutex();
    app_config_default(&s_cfg);

    uart_config_t ucfg = {
        .baud_rate = CONFIG_APP_SERIAL_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(SERIAL_PORT, 1024, 2048, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(SERIAL_PORT, &ucfg));

    i2c_master_bus_config_t bcfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = CONFIG_APP_I2C_SDA,
        .scl_io_num = CONFIG_APP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bcfg, &s_bus));

    char buf[TX_BUF_LEN];
    int len = snprintf(buf, sizeof(buf), "{\"type\":\"hello\",\"fw\":\"%s\",\"version\":\"%s\",\"t_ms\":%lld}",
                       FW_NAME, FW_VERSION, (long long)now_ms());
    send_line(buf, len);
    send_line(buf, protocol_fmt_config(buf, sizeof(buf), "ack", "boot", "", &s_cfg));

    xTaskCreate(sensor_task, "sensor", 6144, NULL, 5, &s_sensor_task);
    xTaskCreate(command_task, "cmd", 6144, NULL, 6, NULL);
}
