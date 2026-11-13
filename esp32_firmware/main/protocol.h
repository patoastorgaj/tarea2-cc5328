#pragma once

// protocolo JSON Lines entre la GUI y el ESP32 (ver README)
// no depende de ESP-IDF

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_config.h"
#include "processing.h"

#define PROTO_MAX_LINE 256
#define PROTO_ID_LEN 24

typedef enum {
    CMD_NONE = 0,
    CMD_CONFIG,
    CMD_GET_CONFIG,
    CMD_START,
    CMD_STOP,
    CMD_PING,
    CMD_SELFTEST,
} cmd_kind_t;

typedef struct {
    cmd_kind_t kind;
    char id[PROTO_ID_LEN];
    app_config_t cfg;  // configuracion resultante si el comando es valido
    const char *err_code;
    const char *err_field;
    char err_msg[80];
} cmd_parse_t;

// valida el comando completo antes de tocar nada; false si hay error
bool protocol_parse(const char *line, const app_config_t *current, cmd_parse_t *out);

const char *protocol_cmd_name(cmd_kind_t kind);

typedef struct {
    uint32_t seq;
    int64_t t_ms;
    bool valid;
    const char *error;
    float temp_c;
    float hum_pct;
    float press_hpa;
    bool has_avg;
    avg_result_t avg;
    uint16_t avg_window_s;
} sample_msg_t;

// todas retornan el largo escrito o -1 si no cabe en el buffer
int protocol_fmt_config(char *buf, size_t n, const char *type, const char *cmd,
                        const char *id, const app_config_t *c);
int protocol_fmt_error(char *buf, size_t n, const char *id, const char *code,
                       const char *field, const char *msg);
int protocol_fmt_sample(char *buf, size_t n, const sample_msg_t *s, uint8_t vars);
int protocol_fmt_peak(char *buf, size_t n, uint32_t peak_seq, const peak_event_t *e);
int protocol_fmt_selftest(char *buf, size_t n, const char *id, const selftest_result_t *r);
