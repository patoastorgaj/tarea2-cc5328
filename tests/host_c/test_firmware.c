#include <math.h>
#include <stdio.h>
#include <string.h>

#include "processing.h"
#include "protocol.h"

static int fails = 0;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("FALLA %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
            fails++;                                                   \
        }                                                              \
    } while (0)

static void test_selftest(void)
{
    selftest_result_t r;
    processing_selftest(&r);
    CHECK(r.peaks_hyst == 3);
    CHECK(r.peaks_no_hyst == 4);
    CHECK(r.n_w5 == 5 && r.complete_w5);
    CHECK(r.n_w30 == 10 && !r.complete_w30);
    CHECK(r.pass);
}

// eventos exactos sobre la serie conocida
static void test_peak_events(void)
{
    const float s[] = {27.0f, 27.4f, 28.2f, 28.6f, 28.4f, 28.1f, 27.6f, 27.9f, 28.3f,
                       28.3f, 28.2f, 27.5f, 29.0f, 29.1f, 29.1f, 29.2f, 28.0f};
    const int64_t exp_t[] = {3000, 8000, 13000};
    const float exp_v[] = {28.6f, 28.3f, 29.1f};
    peak_detector_t d;
    peak_event_t ev;
    int k = 0;
    peak_reset(&d);
    for (int i = 0; i < 17; i++) {
        if (peak_update(&d, i * 1000, s[i], 28.0f, 0.3f, &ev)) {
            CHECK(k < 3);
            if (k < 3) {
                CHECK(ev.t_ms == exp_t[k]);
                CHECK(ev.detected_ms == exp_t[k] + 1000);
                CHECK(fabsf(ev.peak_c - exp_v[k]) < 1e-4f);
            }
            k++;
        }
    }
    CHECK(k == 3);
}

// superar el umbral sin maximo local no es pico
static void test_peak_monotonic(void)
{
    peak_detector_t d;
    peak_event_t ev;
    int k = 0;
    peak_reset(&d);
    for (int i = 0; i < 20; i++) {
        k += peak_update(&d, i * 1000, 28.0f + 0.1f * i, 28.0f, 0.0f, &ev);
    }
    CHECK(k == 0);
    // meseta constante sobre el umbral: tampoco
    peak_reset(&d);
    for (int i = 0; i < 20; i++) {
        k += peak_update(&d, i * 1000, 30.0f, 28.0f, 0.0f, &ev);
    }
    CHECK(k == 0);
}

static void test_peak_below_threshold(void)
{
    const float s[] = {25.0f, 26.0f, 25.0f, 27.9f, 27.0f};
    peak_detector_t d;
    peak_event_t ev;
    int k = 0;
    peak_reset(&d);
    for (int i = 0; i < 5; i++) {
        k += peak_update(&d, i * 1000, s[i], 28.0f, 0.0f, &ev);
    }
    CHECK(k == 0);
}

static void test_avg_window_change(void)
{
    static temp_history_t h;
    avg_result_t a;
    th_init(&h);
    // 5 Hz durante 70 s, T = indice de segundo
    for (int i = 0; i <= 350; i++) {
        th_push(&h, i * 200, (float)(i / 5));
    }
    int64_t now = 350 * 200;
    CHECK(th_mean(&h, now, 5, &a));
    CHECK(a.n == 25 && a.complete);
    CHECK(th_mean(&h, now, 60, &a));
    CHECK(a.n == 300 && a.complete);
    // ventana mas larga que lo adquirido
    CHECK(th_mean(&h, now, 120, &a));
    CHECK(a.n == 351 && !a.complete);
}

static void test_avg_retention(void)
{
    static temp_history_t h;
    avg_result_t a;
    th_init(&h);
    // 5 Hz por 10 minutos, no debe desbordar
    for (int i = 0; i < 3000; i++) {
        th_push(&h, i * 200, 20.0f);
    }
    CHECK(h.count <= TEMP_HIST_LEN);
    CHECK(th_mean(&h, 2999 * 200, 120, &a));
    CHECK(a.n == 600);
    CHECK(!th_mean(&h, 2999 * 200 + 200000, 10, &a));
}

static app_config_t base(void)
{
    app_config_t c;
    app_config_default(&c);
    return c;
}

static void test_parse_full_config(void)
{
    app_config_t cur = base();
    cmd_parse_t r;
    const char *line = "{\"type\":\"config\",\"id\":\"c7\",\"sampling_hz\":2,\"variables\":[\"T\",\"P\"],"
                       "\"average\":{\"enabled\":true,\"window_s\":30},"
                       "\"peak\":{\"enabled\":false,\"threshold_c\":28.46}}";
    CHECK(protocol_parse(line, &cur, &r));
    CHECK(r.kind == CMD_CONFIG);
    CHECK(strcmp(r.id, "c7") == 0);
    CHECK(r.cfg.sampling_hz == 2.0f);
    CHECK(r.cfg.vars == (VAR_T | VAR_P));
    CHECK(r.cfg.avg_window_s == 30);
    CHECK(!r.cfg.peak_enabled);
    CHECK(fabsf(r.cfg.peak_threshold_c - 28.5f) < 1e-4f);
}

static void test_parse_seven_combinations(void)
{
    const char *sets[] = {"[\"T\"]", "[\"H\"]", "[\"P\"]", "[\"T\",\"H\"]",
                          "[\"T\",\"P\"]", "[\"H\",\"P\"]", "[\"T\",\"H\",\"P\"]"};
    const uint8_t masks[] = {1, 2, 4, 3, 5, 6, 7};
    app_config_t cur = base();
    for (int i = 0; i < 7; i++) {
        char line[128];
        cmd_parse_t r;
        snprintf(line, sizeof(line), "{\"type\":\"config\",\"variables\":%s}", sets[i]);
        CHECK(protocol_parse(line, &cur, &r));
        CHECK(r.cfg.vars == masks[i]);
    }
}

// cualquier error deja la configuracion sin cambios
static void test_parse_invalid_atomic(void)
{
    app_config_t cur = base();
    cmd_parse_t r;
    const char *bad[] = {
        "{\"type\":\"config\",\"sampling_hz\":3}",
        "{\"type\":\"config\",\"variables\":[]}",
        "{\"type\":\"config\",\"variables\":[\"X\"]}",
        "{\"type\":\"config\",\"sampling_hz\":2,\"average\":{\"window_s\":0}}",
        "{\"type\":\"config\",\"sampling_hz\":5,\"average\":{\"window_s\":10.5}}",
        "{\"type\":\"config\",\"peak\":{\"threshold_c\":200}}",
        "{\"type\":\"config\",\"peak\":{\"enabled\":1}}",
        "{\"type\":\"config\",\"frecuencia\":1}",
        "{\"type\":\"config\"}",
        "{\"type\":\"reboot\"}",
        "{\"sampling_hz\":1}",
        "{\"type\":\"config\",\"sampling_hz\":1",
        "hola",
        "[1,2]",
        "{\"type\":\"config\",\"id\":\"a\\\"b\",\"sampling_hz\":1}",
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(!protocol_parse(bad[i], &cur, &r));
        CHECK(r.err_code != NULL);
        CHECK(memcmp(&r.cfg, &cur, sizeof(cur)) == 0);
    }
}

static void test_parse_other_commands(void)
{
    app_config_t cur = base();
    cmd_parse_t r;
    CHECK(protocol_parse("{\"type\":\"stop\"}", &cur, &r) && r.kind == CMD_STOP && !r.cfg.streaming);
    CHECK(protocol_parse(" {\"type\" : \"start\" } ", &cur, &r) && r.kind == CMD_START && r.cfg.streaming);
    CHECK(protocol_parse("{\"type\":\"get_config\"}", &cur, &r) && r.kind == CMD_GET_CONFIG);
    CHECK(protocol_parse("{\"type\":\"selftest\",\"id\":\"x1\"}", &cur, &r) && r.kind == CMD_SELFTEST);
}

static void test_format(void)
{
    char buf[384];
    sample_msg_t s = {0};
    s.seq = 145;
    s.t_ms = 73210;
    s.valid = true;
    s.temp_c = 26.4f;
    s.hum_pct = 40.0f;
    s.press_hpa = 1012.7f;
    s.has_avg = true;
    s.avg.mean_c = 26.1f;
    s.avg.n = 10;
    s.avg.complete = true;
    s.avg_window_s = 10;
    CHECK(protocol_fmt_sample(buf, sizeof(buf), &s, VAR_H | VAR_P) > 0);
    CHECK(strstr(buf, "temperature_c") == NULL);
    CHECK(strstr(buf, "\"temp_avg_c\":26.100") != NULL);
    CHECK(strstr(buf, "\"humidity_pct\":40.00") != NULL);
    printf("ejemplo sample: %s\n", buf);

    app_config_t c = base();
    CHECK(protocol_fmt_config(buf, sizeof(buf), "ack", "config", "c1", &c) > 0);
    printf("ejemplo ack:    %s\n", buf);
    CHECK(protocol_fmt_config(buf, 20, "ack", "config", "c1", &c) == -1);
}

int main(void)
{
    test_selftest();
    test_peak_events();
    test_peak_monotonic();
    test_peak_below_threshold();
    test_avg_window_change();
    test_avg_retention();
    test_parse_full_config();
    test_parse_seven_combinations();
    test_parse_invalid_atomic();
    test_parse_other_commands();
    test_format();
    if (fails) {
        printf("%d pruebas fallidas\n", fails);
        return 1;
    }
    printf("todas las pruebas OK\n");
    return 0;
}
