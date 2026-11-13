#pragma once

#include <stdbool.h>
#include <stdint.h>

// mascara de variables transmitidas
#define VAR_T 0x01
#define VAR_H 0x02
#define VAR_P 0x04
#define VAR_ALL (VAR_T | VAR_H | VAR_P)

// limites aceptados por el protocolo
#define WINDOW_MIN_S 1
#define WINDOW_MAX_S 120
#define THRESHOLD_MIN_C (-40.0f)
#define THRESHOLD_MAX_C 85.0f
#define HYSTERESIS_MAX_C 5.0f

typedef struct {
    float sampling_hz;
    uint8_t vars;
    bool avg_enabled;
    uint16_t avg_window_s;
    bool peak_enabled;
    float peak_threshold_c;
    float peak_hysteresis_c;
    bool streaming;
} app_config_t;

// valores iniciales sugeridos en el enunciado
void app_config_default(app_config_t *c);

uint32_t app_config_period_ms(const app_config_t *c);
