#include "processing.h"

#include <math.h>
#include <string.h>

void th_init(temp_history_t *h)
{
    memset(h, 0, sizeof(*h));
}

static int oldest_index(const temp_history_t *h)
{
    return (h->head - h->count + TEMP_HIST_LEN) % TEMP_HIST_LEN;
}

void th_push(temp_history_t *h, int64_t t_ms, float temp_c)
{
    if (!h->started) {
        h->started = true;
        h->first_ms = t_ms;
    }
    h->pts[h->head].t_ms = t_ms;
    h->pts[h->head].temp_c = temp_c;
    h->head = (h->head + 1) % TEMP_HIST_LEN;
    if (h->count < TEMP_HIST_LEN) {
        h->count++;
    }

    // se retiene siempre la ventana maxima para poder cambiar W sin perder datos
    int64_t limit = t_ms - (int64_t)WINDOW_MAX_S * 1000;
    while (h->count > 0 && h->pts[oldest_index(h)].t_ms <= limit) {
        h->count--;
    }
}

bool th_mean(const temp_history_t *h, int64_t now_ms, uint32_t window_s, avg_result_t *out)
{
    int64_t from = now_ms - (int64_t)window_s * 1000;
    double sum = 0.0;
    int n = 0;

    // se recorre desde la muestra mas nueva hacia atras
    for (int i = 0; i < h->count; i++) {
        int idx = (h->head - 1 - i + TEMP_HIST_LEN) % TEMP_HIST_LEN;
        const temp_point_t *p = &h->pts[idx];
        if (p->t_ms <= from) {
            break;
        }
        if (p->t_ms <= now_ms) {
            sum += p->temp_c;
            n++;
        }
    }
    if (n == 0) {
        return false;
    }
    out->mean_c = (float)(sum / n);
    out->n = n;
    out->complete = h->started && (now_ms - h->first_ms) >= (int64_t)window_s * 1000;
    return true;
}

void peak_reset(peak_detector_t *d)
{
    memset(d, 0, sizeof(*d));
    d->armed = true;
}

bool peak_update(peak_detector_t *d, int64_t t_ms, float temp_c,
                 float threshold_c, float hysteresis_c, peak_event_t *ev)
{
    if (d->n < 3) {
        d->v[d->n] = temp_c;
        d->t[d->n] = t_ms;
        d->n++;
    } else {
        d->v[0] = d->v[1];
        d->t[0] = d->t[1];
        d->v[1] = d->v[2];
        d->t[1] = d->t[2];
        d->v[2] = temp_c;
        d->t[2] = t_ms;
    }

    bool found = false;
    if (d->n == 3 && d->armed) {
        float a = d->v[0];  // T(k-2)
        float b = d->v[1];  // T(k-1)
        float c = d->v[2];  // T(k)
        if (b > a && b >= c && b >= threshold_c) {
            ev->t_ms = d->t[1];
            ev->detected_ms = d->t[2];
            ev->peak_c = b;
            ev->threshold_c = threshold_c;
            found = true;
            // con histeresis se espera que T baje de theta - h antes de otro evento
            d->armed = hysteresis_c <= 0.0f;
        }
    }
    if (!d->armed && temp_c < threshold_c - hysteresis_c) {
        d->armed = true;
    }
    return found;
}

// serie conocida para validar la deteccion, muestras cada 1 s
static const float k_peak_series[] = {
    27.00f, 27.40f, 28.20f, 28.60f, 28.40f, 28.10f, 27.60f, 27.90f, 28.30f,
    28.30f, 28.20f, 27.50f, 29.00f, 29.10f, 29.10f, 29.20f, 28.00f,
};

static int count_peaks(float threshold_c, float hysteresis_c)
{
    peak_detector_t d;
    peak_event_t ev;
    int count = 0;
    peak_reset(&d);
    for (unsigned i = 0; i < sizeof(k_peak_series) / sizeof(k_peak_series[0]); i++) {
        if (peak_update(&d, (int64_t)i * 1000, k_peak_series[i], threshold_c, hysteresis_c, &ev)) {
            count++;
        }
    }
    return count;
}

// estatico para no usar ~10 KB de stack
static temp_history_t s_test_hist;

void processing_selftest(selftest_result_t *r)
{
    memset(r, 0, sizeof(*r));
    r->peaks_hyst = count_peaks(28.0f, 0.3f);
    r->peaks_no_hyst = count_peaks(28.0f, 0.0f);

    // rampa 20, 21, ..., 29 C cada 1 s
    th_init(&s_test_hist);
    for (int i = 0; i < 10; i++) {
        th_push(&s_test_hist, (int64_t)i * 1000, 20.0f + (float)i);
    }
    avg_result_t a5 = {0}, a30 = {0};
    th_mean(&s_test_hist, 9000, 5, &a5);
    th_mean(&s_test_hist, 9000, 30, &a30);
    r->avg_w5 = a5.mean_c;
    r->n_w5 = a5.n;
    r->complete_w5 = a5.complete;
    r->avg_w30 = a30.mean_c;
    r->n_w30 = a30.n;
    r->complete_w30 = a30.complete;

    r->pass = r->peaks_hyst == 3 && r->peaks_no_hyst == 4 &&
              fabsf(r->avg_w5 - 27.0f) < 1e-4f && r->n_w5 == 5 && r->complete_w5 &&
              fabsf(r->avg_w30 - 24.5f) < 1e-4f && r->n_w30 == 10 && !r->complete_w30;
}
