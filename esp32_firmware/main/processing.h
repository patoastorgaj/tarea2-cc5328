#pragma once

// procesamiento local de temperatura: media movil temporal y deteccion de picos
// no depende de ESP-IDF, se compila tambien en el PC para las pruebas

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"

// 120 s a 5 Hz = 600 muestras, con margen
#define TEMP_HIST_LEN 650

typedef struct {
    int64_t t_ms;
    float temp_c;
} temp_point_t;

typedef struct {
    temp_point_t pts[TEMP_HIST_LEN];
    int head;
    int count;
    int64_t first_ms;
    bool started;
} temp_history_t;

typedef struct {
    float mean_c;
    int n;
    bool complete;
} avg_result_t;

void th_init(temp_history_t *h);

// guarda una muestra y descarta las mas antiguas que WINDOW_MAX_S
void th_push(temp_history_t *h, int64_t t_ms, float temp_c);

// media de las muestras con now - W < t_i <= now; false si no hay muestras
bool th_mean(const temp_history_t *h, int64_t now_ms, uint32_t window_s, avg_result_t *out);

typedef struct {
    float v[3];
    int64_t t[3];
    int n;
    bool armed;
} peak_detector_t;

typedef struct {
    int64_t t_ms;         // instante de la muestra pico (k-1)
    int64_t detected_ms;  // instante en que se confirmo (k)
    float peak_c;
    float threshold_c;
} peak_event_t;

void peak_reset(peak_detector_t *d);

// procesa una muestra; retorna true si se confirma un maximo local
bool peak_update(peak_detector_t *d, int64_t t_ms, float temp_c,
                 float threshold_c, float hysteresis_c, peak_event_t *ev);

typedef struct {
    int peaks_hyst;     // picos con histeresis 0.3 (esperado 3)
    int peaks_no_hyst;  // picos sin histeresis (esperado 4)
    float avg_w5;       // esperado 27.0
    int n_w5;           // esperado 5
    bool complete_w5;   // esperado true
    float avg_w30;      // esperado 24.5
    int n_w30;          // esperado 10
    bool complete_w30;  // esperado false
    bool pass;
} selftest_result_t;

// corre los algoritmos sobre series conocidas, sin tocar el estado real
void processing_selftest(selftest_result_t *r);
