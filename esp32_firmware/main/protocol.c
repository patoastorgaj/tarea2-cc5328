#include "protocol.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "mini_json.h"

#define MAX_TOKENS 48

static const float k_allowed_hz[] = {0.5f, 1.0f, 2.0f, 5.0f};

void app_config_default(app_config_t *c)
{
    c->sampling_hz = 1.0f;
    c->vars = VAR_ALL;
    c->avg_enabled = true;
    c->avg_window_s = 10;
    c->peak_enabled = true;
    c->peak_threshold_c = 28.0f;
    c->peak_hysteresis_c = 0.3f;
    c->streaming = true;
}

uint32_t app_config_period_ms(const app_config_t *c)
{
    return (uint32_t)lroundf(1000.0f / c->sampling_hz);
}

const char *protocol_cmd_name(cmd_kind_t kind)
{
    switch (kind) {
    case CMD_CONFIG: return "config";
    case CMD_GET_CONFIG: return "get_config";
    case CMD_START: return "start";
    case CMD_STOP: return "stop";
    case CMD_PING: return "ping";
    case CMD_SELFTEST: return "selftest";
    default: return "";
    }
}

static bool set_error(cmd_parse_t *out, const char *code, const char *field, const char *msg)
{
    out->err_code = code;
    out->err_field = field;
    snprintf(out->err_msg, sizeof(out->err_msg), "%s", msg);
    return false;
}

// revisa que todas las claves del objeto esten permitidas
static int find_unknown_key(const char *js, const mj_tok_t *t, int n, int obj,
                            const char *const *allowed, int n_allowed)
{
    for (int k = mj_next_child(t, n, obj, obj); k >= 0; k = mj_next_child(t, n, obj, k)) {
        bool ok = false;
        for (int i = 0; i < n_allowed; i++) {
            if (mj_eq(js, &t[k], allowed[i])) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            return k;
        }
    }
    return -1;
}

static bool valid_id_char(char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
}

static bool parse_vars(const char *js, const mj_tok_t *t, int n, int arr, uint8_t *mask, cmd_parse_t *out)
{
    if (t[arr].type != MJ_ARRAY) {
        return set_error(out, "invalid_value", "variables", "debe ser un arreglo con T, H y/o P");
    }
    uint8_t m = 0;
    for (int e = mj_next_child(t, n, arr, arr); e >= 0; e = mj_next_child(t, n, arr, e)) {
        if (mj_eq(js, &t[e], "T")) {
            m |= VAR_T;
        } else if (mj_eq(js, &t[e], "H")) {
            m |= VAR_H;
        } else if (mj_eq(js, &t[e], "P")) {
            m |= VAR_P;
        } else {
            return set_error(out, "invalid_value", "variables", "variable desconocida");
        }
    }
    if (m == 0) {
        return set_error(out, "invalid_value", "variables", "la seleccion no puede estar vacia");
    }
    *mask = m;
    return true;
}

static bool parse_average(const char *js, const mj_tok_t *t, int n, int obj, app_config_t *cfg, cmd_parse_t *out)
{
    static const char *const keys[] = {"enabled", "window_s"};
    if (t[obj].type != MJ_OBJECT) {
        return set_error(out, "invalid_value", "average", "debe ser un objeto");
    }
    if (find_unknown_key(js, t, n, obj, keys, 2) >= 0) {
        return set_error(out, "unknown_field", "average", "campo no reconocido dentro de average");
    }
    int v = mj_find(js, t, n, obj, "enabled");
    if (v >= 0 && !mj_bool(js, &t[v], &cfg->avg_enabled)) {
        return set_error(out, "invalid_value", "average.enabled", "debe ser true o false");
    }
    v = mj_find(js, t, n, obj, "window_s");
    if (v >= 0) {
        double w;
        if (!mj_number(js, &t[v], &w) || w != floor(w) || w < WINDOW_MIN_S || w > WINDOW_MAX_S) {
            return set_error(out, "invalid_value", "average.window_s", "entero entre 1 y 120");
        }
        cfg->avg_window_s = (uint16_t)w;
    }
    return true;
}

static bool parse_peak(const char *js, const mj_tok_t *t, int n, int obj, app_config_t *cfg, cmd_parse_t *out)
{
    static const char *const keys[] = {"enabled", "threshold_c", "hysteresis_c"};
    if (t[obj].type != MJ_OBJECT) {
        return set_error(out, "invalid_value", "peak", "debe ser un objeto");
    }
    if (find_unknown_key(js, t, n, obj, keys, 3) >= 0) {
        return set_error(out, "unknown_field", "peak", "campo no reconocido dentro de peak");
    }
    int v = mj_find(js, t, n, obj, "enabled");
    if (v >= 0 && !mj_bool(js, &t[v], &cfg->peak_enabled)) {
        return set_error(out, "invalid_value", "peak.enabled", "debe ser true o false");
    }
    v = mj_find(js, t, n, obj, "threshold_c");
    if (v >= 0) {
        double th;
        if (!mj_number(js, &t[v], &th) || th < THRESHOLD_MIN_C || th > THRESHOLD_MAX_C) {
            return set_error(out, "invalid_value", "peak.threshold_c", "fuera de rango (-40 a 85 C)");
        }
        // resolucion de 0.1 C
        cfg->peak_threshold_c = (float)(round(th * 10.0) / 10.0);
    }
    v = mj_find(js, t, n, obj, "hysteresis_c");
    if (v >= 0) {
        double h;
        if (!mj_number(js, &t[v], &h) || h < 0.0 || h > HYSTERESIS_MAX_C) {
            return set_error(out, "invalid_value", "peak.hysteresis_c", "fuera de rango (0 a 5 C)");
        }
        cfg->peak_hysteresis_c = (float)(round(h * 10.0) / 10.0);
    }
    return true;
}

static bool parse_config(const char *js, const mj_tok_t *t, int n, cmd_parse_t *out)
{
    static const char *const keys[] = {"type", "id", "sampling_hz", "variables", "average", "peak"};
    if (find_unknown_key(js, t, n, 0, keys, 6) >= 0) {
        return set_error(out, "unknown_field", "", "campo no reconocido en config");
    }

    // se trabaja sobre una copia; si algo falla no se aplica nada
    app_config_t cfg = out->cfg;
    bool any = false;

    int v = mj_find(js, t, n, 0, "sampling_hz");
    if (v >= 0) {
        double hz;
        bool ok = false;
        if (mj_number(js, &t[v], &hz)) {
            for (unsigned i = 0; i < sizeof(k_allowed_hz) / sizeof(k_allowed_hz[0]); i++) {
                if (fabs(hz - k_allowed_hz[i]) < 1e-6) {
                    cfg.sampling_hz = k_allowed_hz[i];
                    ok = true;
                }
            }
        }
        if (!ok) {
            return set_error(out, "invalid_value", "sampling_hz", "valores permitidos: 0.5, 1, 2, 5");
        }
        any = true;
    }
    v = mj_find(js, t, n, 0, "variables");
    if (v >= 0) {
        if (!parse_vars(js, t, n, v, &cfg.vars, out)) {
            return false;
        }
        any = true;
    }
    v = mj_find(js, t, n, 0, "average");
    if (v >= 0) {
        if (!parse_average(js, t, n, v, &cfg, out)) {
            return false;
        }
        any = true;
    }
    v = mj_find(js, t, n, 0, "peak");
    if (v >= 0) {
        if (!parse_peak(js, t, n, v, &cfg, out)) {
            return false;
        }
        any = true;
    }
    if (!any) {
        return set_error(out, "missing_field", "", "config sin parametros");
    }
    out->cfg = cfg;
    return true;
}

bool protocol_parse(const char *line, const app_config_t *current, cmd_parse_t *out)
{
    mj_tok_t t[MAX_TOKENS];

    memset(out, 0, sizeof(*out));
    out->cfg = *current;

    int n = mj_parse(line, strlen(line), t, MAX_TOKENS);
    if (n < 0) {
        return set_error(out, "parse_error", "", n == MJ_ERR_NOMEM ? "comando demasiado grande" : "JSON invalido");
    }
    if (t[0].type != MJ_OBJECT) {
        return set_error(out, "parse_error", "", "se esperaba un objeto JSON");
    }

    // el id es opcional y se devuelve en la respuesta
    int v = mj_find(line, t, n, 0, "id");
    if (v >= 0) {
        int len = t[v].end - t[v].start;
        if (t[v].type != MJ_STRING || len <= 0 || len >= PROTO_ID_LEN) {
            return set_error(out, "invalid_value", "id", "id debe ser texto corto");
        }
        for (int i = 0; i < len; i++) {
            if (!valid_id_char(line[t[v].start + i])) {
                return set_error(out, "invalid_value", "id", "id con caracteres no permitidos");
            }
        }
        memcpy(out->id, line + t[v].start, (size_t)len);
        out->id[len] = '\0';
    }

    v = mj_find(line, t, n, 0, "type");
    if (v < 0 || t[v].type != MJ_STRING) {
        return set_error(out, "missing_field", "type", "falta el campo type");
    }

    if (mj_eq(line, &t[v], "config")) {
        out->kind = CMD_CONFIG;
        return parse_config(line, t, n, out);
    }
    if (mj_eq(line, &t[v], "get_config")) {
        out->kind = CMD_GET_CONFIG;
    } else if (mj_eq(line, &t[v], "start")) {
        out->kind = CMD_START;
        out->cfg.streaming = true;
    } else if (mj_eq(line, &t[v], "stop")) {
        out->kind = CMD_STOP;
        out->cfg.streaming = false;
    } else if (mj_eq(line, &t[v], "ping")) {
        out->kind = CMD_PING;
    } else if (mj_eq(line, &t[v], "selftest")) {
        out->kind = CMD_SELFTEST;
    } else {
        return set_error(out, "unknown_command", "type", "comando no reconocido");
    }
    return true;
}

// escritura acumulativa en un buffer
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool overflow;
} sbuf_t;

static void sb_add(sbuf_t *sb, const char *fmt, ...)
{
    if (sb->overflow) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(sb->buf + sb->len, sb->cap - sb->len, fmt, ap);
    va_end(ap);
    if (r < 0 || (size_t)r >= sb->cap - sb->len) {
        sb->overflow = true;
        return;
    }
    sb->len += (size_t)r;
}

static int sb_done(sbuf_t *sb)
{
    return sb->overflow ? -1 : (int)sb->len;
}

static void sb_id(sbuf_t *sb, const char *id)
{
    if (id && id[0]) {
        sb_add(sb, ",\"id\":\"%s\"", id);
    }
}

static void sb_vars(sbuf_t *sb, uint8_t vars)
{
    const char *sep = "";
    sb_add(sb, "[");
    if (vars & VAR_T) {
        sb_add(sb, "%s\"T\"", sep);
        sep = ",";
    }
    if (vars & VAR_H) {
        sb_add(sb, "%s\"H\"", sep);
        sep = ",";
    }
    if (vars & VAR_P) {
        sb_add(sb, "%s\"P\"", sep);
    }
    sb_add(sb, "]");
}

int protocol_fmt_config(char *buf, size_t n, const char *type, const char *cmd,
                        const char *id, const app_config_t *c)
{
    sbuf_t sb = {buf, n, 0, false};
    sb_add(&sb, "{\"type\":\"%s\",\"cmd\":\"%s\",\"status\":\"ok\"", type, cmd);
    sb_id(&sb, id);
    sb_add(&sb, ",\"sampling_hz\":%g,\"variables\":", (double)c->sampling_hz);
    sb_vars(&sb, c->vars);
    sb_add(&sb, ",\"average\":{\"enabled\":%s,\"window_s\":%u}",
           c->avg_enabled ? "true" : "false", (unsigned)c->avg_window_s);
    sb_add(&sb, ",\"peak\":{\"enabled\":%s,\"threshold_c\":%.1f,\"hysteresis_c\":%.1f}",
           c->peak_enabled ? "true" : "false", (double)c->peak_threshold_c, (double)c->peak_hysteresis_c);
    sb_add(&sb, ",\"streaming\":%s}", c->streaming ? "true" : "false");
    return sb_done(&sb);
}

int protocol_fmt_error(char *buf, size_t n, const char *id, const char *code,
                       const char *field, const char *msg)
{
    sbuf_t sb = {buf, n, 0, false};
    sb_add(&sb, "{\"type\":\"error\",\"code\":\"%s\"", code);
    sb_id(&sb, id);
    if (field && field[0]) {
        sb_add(&sb, ",\"field\":\"%s\"", field);
    }
    sb_add(&sb, ",\"msg\":\"%s\"}", msg ? msg : "");
    return sb_done(&sb);
}

int protocol_fmt_sample(char *buf, size_t n, const sample_msg_t *s, uint8_t vars)
{
    sbuf_t sb = {buf, n, 0, false};
    sb_add(&sb, "{\"type\":\"sample\",\"seq\":%lu,\"t_ms\":%lld,\"valid\":%s",
           (unsigned long)s->seq, (long long)s->t_ms, s->valid ? "true" : "false");
    if (!s->valid) {
        sb_add(&sb, ",\"error\":\"%s\"", s->error ? s->error : "read_failed");
    } else {
        if (vars & VAR_T) {
            sb_add(&sb, ",\"temperature_c\":%.2f", (double)s->temp_c);
        }
        if (vars & VAR_H) {
            sb_add(&sb, ",\"humidity_pct\":%.2f", (double)s->hum_pct);
        }
        if (vars & VAR_P) {
            sb_add(&sb, ",\"pressure_hpa\":%.2f", (double)s->press_hpa);
        }
    }
    if (s->has_avg) {
        sb_add(&sb, ",\"temp_avg_c\":%.3f,\"avg_n\":%d,\"avg_window_s\":%u,\"avg_window_complete\":%s",
               (double)s->avg.mean_c, s->avg.n, (unsigned)s->avg_window_s,
               s->avg.complete ? "true" : "false");
    }
    sb_add(&sb, "}");
    return sb_done(&sb);
}

int protocol_fmt_peak(char *buf, size_t n, uint32_t peak_seq, const peak_event_t *e)
{
    sbuf_t sb = {buf, n, 0, false};
    sb_add(&sb, "{\"type\":\"peak\",\"peak_seq\":%lu,\"t_ms\":%lld,\"detected_ms\":%lld,"
                "\"peak_c\":%.2f,\"threshold_c\":%.1f}",
           (unsigned long)peak_seq, (long long)e->t_ms, (long long)e->detected_ms,
           (double)e->peak_c, (double)e->threshold_c);
    return sb_done(&sb);
}

int protocol_fmt_selftest(char *buf, size_t n, const char *id, const selftest_result_t *r)
{
    sbuf_t sb = {buf, n, 0, false};
    sb_add(&sb, "{\"type\":\"selftest\",\"status\":\"%s\"", r->pass ? "ok" : "fail");
    sb_id(&sb, id);
    sb_add(&sb, ",\"peaks_hyst\":%d,\"peaks_no_hyst\":%d", r->peaks_hyst, r->peaks_no_hyst);
    sb_add(&sb, ",\"avg_w5\":%.3f,\"n_w5\":%d,\"complete_w5\":%s",
           (double)r->avg_w5, r->n_w5, r->complete_w5 ? "true" : "false");
    sb_add(&sb, ",\"avg_w30\":%.3f,\"n_w30\":%d,\"complete_w30\":%s}",
           (double)r->avg_w30, r->n_w30, r->complete_w30 ? "true" : "false");
    return sb_done(&sb);
}
