/**
 * @file    osc_core.c
 * @brief   Núcleo del osciloscopio (C puro).
 *
 * TIEMPO EQUIVALENTE (ETS) CON UN SOLO ADC
 * ----------------------------------------
 * La idea de "varios ADC desfasados" (time-interleaving) es que cada ADC tome
 * la muestra en una fase distinta del mismo período. El STM32H503 tiene un
 * solo ADC, pero si la señal es periódica se logra lo mismo en el tiempo:
 *
 *   1. Se captura a la máxima velocidad (~2 MS/s) muchos períodos seguidos.
 *   2. Se mide el período T con gran precisión (regresión lineal sobre todos
 *      los cruces por el nivel medio, interpolados entre muestras).
 *   3. Cada muestra k cae en la fase  φ = frac((k - t0) / T)  del período.
 *      Como Fs/f no es entero, las fases van cayendo en lugares distintos:
 *      es el mismo "desfase" que tendrían varios ADC en paralelo.
 *   4. Se ordenan por fase en 256 casilleros por período (promediando las que
 *      caen juntas, lo que además baja el ruido).
 *
 * Si Fs/f queda cerca de un cociente de enteros chicos (por ejemplo, 500 kHz
 * con Fs = 2 MS/s da justo 4 muestras por período), todas las muestras caen
 * en las mismas pocas fases. Para evitarlo, osc_ets_best_arr() prueba varias
 * frecuencias de muestreo cercanas y elige la que mejor cubre el período:
 * el micro "desafina" su reloj de muestreo a propósito.
 */
#include <math.h>
#include <string.h>
#include "osc_core.h"

#define CONV_CYCLES  12.5f          /* ciclos de conversión a 12 bits */
static const float k_smp_cycles[8] = {
    2.5f, 6.5f, 12.5f, 24.5f, 47.5f, 92.5f, 247.5f, 640.5f
};

/* ------------------------------------------------------------------------ */
/*  Base de tiempo                                                          */
/* ------------------------------------------------------------------------ */
bool osc_timebase_calc(uint32_t fs_hz, uint32_t timclk_hz, uint32_t adcclk_hz,
                       osc_timebase_t *tb)
{
    if (fs_hz == 0U || fs_hz > timclk_hz) return false;

    /* Timer de 16 bits: div = (PSC+1)·(ARR+1), buscando una división exacta. */
    const uint32_t div = timclk_hz / fs_hz;
    uint32_t psc = 1U;
    while ((div / psc) > 65536U || (div % psc) != 0U) {
        if (++psc > 65536U) return false;
    }
    tb->psc   = (uint16_t)(psc - 1U);
    tb->arr   = (uint16_t)(div / psc - 1U);
    tb->fs_hz = timclk_hz / div;

    /* Ciclos de ADC disponibles por muestra (con medio ciclo de margen). */
    const float budget = (float)adcclk_hz / (float)tb->fs_hz - 0.5f;
    if (budget < k_smp_cycles[0] + CONV_CYCLES) return false;

    /* 1) Sobremuestreo: la mayor potencia de 2 (hasta 256) que entra con un
     *    tiempo de muestreo mínimo de 6,5 ciclos (2,5 si no entra ni uno).    */
    const float smp_min = (budget >= k_smp_cycles[1] + CONV_CYCLES)
                        ? k_smp_cycles[1] : k_smp_cycles[0];
    uint8_t lg = 0U;
    while (lg < 8U && (float)(1UL << (lg + 1U)) * (smp_min + CONV_CYCLES) <= budget) lg++;

    /* 2) Con el tiempo que sobra, el mayor tiempo de muestreo posible
     *    (menos exigencia de impedancia para la fuente de señal).          */
    uint8_t smp = 0U;
    for (int s = 7; s >= 0; s--) {
        if ((float)(1UL << lg) * (k_smp_cycles[s] + CONV_CYCLES) <= budget) {
            smp = (uint8_t)s;
            break;
        }
    }

    tb->osr_log2     = lg;
    tb->smp_code     = smp;
    tb->ovss         = (lg > 4U) ? (uint8_t)(lg - 4U) : 0U;   /* deja 16 bits */
    tb->sw_shift     = (lg < 4U) ? (uint8_t)(4U - lg) : 0U;
    tb->eff_bits_x10 = (uint16_t)(120U + 5U * lg);            /* 12 + log2(R)/2 */
    return true;
}

/* ------------------------------------------------------------------------ */
/*  Procesamiento                                                           */
/* ------------------------------------------------------------------------ */
void osc_normalize(uint16_t *x, uint32_t n, uint8_t sw_shift)
{
    if (sw_shift == 0U) return;
    for (uint32_t i = 0; i < n; i++) x[i] = (uint16_t)(x[i] << sw_shift);
}

/* Próximo cruce ascendente con histéresis. Devuelve la posición fraccionaria
 * (interpolación lineal entre las dos muestras que rodean el nivel).        */
static bool next_rise(const uint16_t *x, uint32_t n, uint32_t *k, bool *armed,
                      int32_t level, int32_t hyst, float *c)
{
    for (uint32_t i = *k; i < n; i++) {
        const int32_t v = x[i];
        if (v < level - hyst) {
            *armed = true;
        } else if (*armed && v >= level && i > 0U) {
            const int32_t p = x[i - 1U];               /* p < level */
            *armed = false;
            *c = (float)(i - 1U) + (float)(level - p) / (float)(v - p);
            *k = i + 1U;
            return true;
        }
    }
    *k = n;
    return false;
}

void osc_measure(const uint16_t *x, uint32_t n, osc_meas_t *m)
{
    uint32_t mn = 0xFFFFU, mx = 0U;
    uint64_t s = 0U, s2 = 0U;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t v = x[i];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        s  += v;
        s2 += (uint64_t)v * v;
    }
    m->vmin = (uint16_t)mn;
    m->vmax = (uint16_t)mx;
    m->vavg = (uint16_t)(s / n);
    m->vrms = (uint16_t)sqrt((double)s2 / (double)n);
    m->period_valid = false;
    m->period = 0.0f;
    m->t0 = 0.0f;
    m->ncross = 0U;

    const int32_t pp = (int32_t)(mx - mn);
    if (pp < 512 || n < 8U) return;              /* señal < ~26 mV pico a pico */
    const int32_t level = (int32_t)((mn + mx) / 2U);
    const int32_t hyst  = pp / 10;

    /* Pasada 1: cantidad de cruces, primero y último -> período aproximado. */
    uint32_t k = 0U, cnt = 0U;
    bool armed = false;
    float c, c_first = 0.0f, c_last = 0.0f;
    while (next_rise(x, n, &k, &armed, level, hyst, &c)) {
        if (cnt == 0U) c_first = c;
        c_last = c;
        cnt++;
    }
    if (cnt < 2U) return;
    double t_est = (c_last - c_first) / (double)(cnt - 1U);
    if (t_est < 2.0) return;

    /* Regresión lineal  c_i = t0 + T·i  con ventana creciente: primero sobre
     * los cruces cercanos (donde un T aproximado alcanza para numerarlos
     * bien aunque falte alguno), después sobre más, hasta usarlos todos.   */
    double T = t_est, a0 = c_first, sse = 0.0, N = 0.0;
    for (uint32_t lim = 8U; ; lim *= 4U) {
        double si = 0, sc = 0, sii = 0, sic = 0, scc = 0;
        bool truncated = false;
        k = 0U; armed = false; N = 0.0;
        while (next_rise(x, n, &k, &armed, level, hyst, &c)) {
            const double i = floor((c - c_first) / t_est + 0.5);
            if (i > (double)lim) { truncated = true; break; }
            const double cc = (double)c;
            si += i; sc += cc; sii += i * i; sic += i * cc; scc += cc * cc;
            N += 1.0;
        }
        const double den = N * sii - si * si;
        if (N < 2.0 || den <= 0.0) return;
        T   = (N * sic - si * sc) / den;
        a0  = (sc - T * si) / N;
        sse = scc - 2.0 * a0 * sc - 2.0 * T * sic
            + N * a0 * a0 + 2.0 * a0 * T * si + T * T * sii;
        t_est = T;
        if (!truncated) break;
    }

    /* Validación: dispersión de los cruces < 10 % del período. */
    if (T < 2.0 || (N > 2.0 && sqrt(fabs(sse) / N) > 0.10 * T)) return;

    m->period = (float)T;
    m->t0     = (float)a0;
    m->ncross = (uint32_t)N;
    m->period_valid = true;
}

int32_t osc_find_trigger(const uint16_t *x, uint32_t from, uint32_t to,
                         uint16_t level, bool rising)
{
    const int32_t h = 655;                       /* histéresis: 1 % de escala */
    const int32_t L = level;
    bool armed = false;
    for (uint32_t i = 0; i < to; i++) {
        const int32_t v = x[i];
        if (rising) {
            if (v < L - h) armed = true;
            else if (armed && v >= L) { if (i >= from) return (int32_t)i; armed = false; }
        } else {
            if (v > L + h) armed = true;
            else if (armed && v <= L) { if (i >= from) return (int32_t)i; armed = false; }
        }
    }
    return -1;
}

/* ------------------------------------------------------------------------ */
/*  Tiempo equivalente                                                      */
/* ------------------------------------------------------------------------ */
static inline uint32_t phase_bin(float ph)
{
    ph -= floorf(ph);
    uint32_t b = (uint32_t)(ph * (float)OSC_ETS_BINS);
    return (b >= OSC_ETS_BINS) ? (OSC_ETS_BINS - 1U) : b;
}

uint32_t osc_ets_coverage(float period, uint32_t n)
{
    uint32_t bm[OSC_ETS_BINS / 32U] = { 0 };
    const float inv = 1.0f / period;
    for (uint32_t k = 0; k < n; k++) {
        const uint32_t b = phase_bin((float)k * inv);
        bm[b >> 5] |= 1UL << (b & 31U);
    }
    uint32_t filled = 0U;
    for (uint32_t i = 0; i < OSC_ETS_BINS / 32U; i++) filled += (uint32_t)__builtin_popcount(bm[i]);
    return filled;
}

uint16_t osc_ets_best_arr(float period, uint16_t arr_now, uint16_t arr_min,
                          uint16_t arr_max, uint32_t n, uint32_t *coverage)
{
    uint16_t best = arr_now;
    uint32_t best_cov = osc_ets_coverage(period, n);
    for (uint32_t a = arr_min; a <= arr_max; a++) {
        /* Con otro ARR cambia Fs, y con él las muestras por período. */
        const float ta = period * (float)(arr_now + 1U) / (float)(a + 1U);
        const uint32_t cov = osc_ets_coverage(ta, n);
        if (cov > best_cov + 2U) { best = (uint16_t)a; best_cov = cov; }
    }
    if (coverage) *coverage = best_cov;
    return best;
}

static uint32_t s_sum[OSC_ETS_BINS];   /* suma de valores -> luego promedio   */
static float    s_off[OSC_ETS_BINS];   /* suma de desvíos de fase respecto del
                                          centro del casillero (en casilleros) */
static uint16_t s_cnt[OSC_ETS_BINS];

#define WRAP(i)  (((i) + OSC_ETS_BINS) % OSC_ETS_BINS)

uint32_t osc_ets_reconstruct(const uint16_t *x, uint32_t n, float period, float t0,
                             uint16_t *out)
{
    memset(s_sum, 0, sizeof s_sum);
    memset(s_off, 0, sizeof s_off);
    memset(s_cnt, 0, sizeof s_cnt);

    /* 1) Cada muestra va al casillero de su fase dentro del período. Se
     *    guarda también dónde cayó dentro del casillero.                   */
    const float inv = 1.0f / period;
    for (uint32_t k = 0; k < n; k++) {
        float ph = ((float)k - t0) * inv;
        ph -= floorf(ph);
        const float pos = ph * (float)OSC_ETS_BINS;
        uint32_t b = (uint32_t)pos;
        if (b >= OSC_ETS_BINS) b = OSC_ETS_BINS - 1U;
        s_sum[b] += x[k];
        s_off[b] += pos - (float)b - 0.5f;
        s_cnt[b]++;
    }

    uint32_t filled = 0U, first = OSC_ETS_BINS;
    for (uint32_t b = 0; b < OSC_ETS_BINS; b++) {
        if (s_cnt[b]) {
            s_sum[b] = (s_sum[b] + s_cnt[b] / 2U) / s_cnt[b];     /* promedio */
            s_off[b] /= (float)s_cnt[b];                          /* desvío medio */
            if (first == OSC_ETS_BINS) first = b;
            filled++;
        }
    }
    if (filled == 0U) return 0U;

    /* 2) El promedio de un casillero corresponde a su fase MEDIA, no a su
     *    centro. Se lo corrige con la pendiente local (vecinos):
     *        y_centro = y_medio - pendiente · desvío_medio                  */
    for (uint32_t b = 0; b < OSC_ETS_BINS; b++) {
        if (!s_cnt[b]) continue;
        float y = (float)s_sum[b];
        const uint32_t l = WRAP(b - 1U), r = WRAP(b + 1U);
        if (s_cnt[l] && s_cnt[r]) {
            const float dx = 2.0f + s_off[r] - s_off[l];
            const float slope = ((float)s_sum[r] - (float)s_sum[l]) / dx;
            y -= slope * s_off[b];
        }
        if (y < 0.0f) y = 0.0f;
        if (y > 65535.0f) y = 65535.0f;
        out[b] = (uint16_t)(y + 0.5f);
    }

    /* 3) Casilleros vacíos: interpolación lineal circular entre vecinos. */
    uint32_t prev = first;
    for (uint32_t step = 1; step <= OSC_ETS_BINS; step++) {
        const uint32_t b = (first + step) % OSC_ETS_BINS;
        if (s_cnt[b] == 0U && b != first) continue;
        const uint32_t gap = (b + OSC_ETS_BINS - prev) % OSC_ETS_BINS;
        const uint32_t g   = (gap == 0U) ? OSC_ETS_BINS : gap;
        const int32_t  y0 = out[prev], y1 = out[b];
        for (uint32_t j = 1; j < g; j++) {
            out[(prev + j) % OSC_ETS_BINS] = (uint16_t)(y0 + (y1 - y0) * (int32_t)j / (int32_t)g);
        }
        prev = b;
    }

    for (uint32_t b = 0; b < OSC_ETS_BINS; b++) out[b + OSC_ETS_BINS] = out[b];  /* 2 períodos */
    return filled;
}

/* ------------------------------------------------------------------------ */
void osc_frame_build(osc_frame_hdr_t *h, osc_mode_t mode, uint8_t flags,
                     uint16_t eff_bits_x10, const uint16_t *pts, uint16_t n,
                     uint16_t trig_index, uint32_t dt_ps, uint32_t fs_hz,
                     uint32_t freq_mhz, uint16_t trig_level,
                     const osc_meas_t *m, uint16_t coverage_permille)
{
    uint16_t sum = 0U;
    for (uint32_t i = 0; i < n; i++) sum = (uint16_t)(sum + pts[i]);

    h->magic[0] = OSC_MAGIC0;  h->magic[1] = OSC_MAGIC1;
    h->magic[2] = 'O';         h->magic[3] = 'S';
    h->version  = OSC_VERSION;
    h->mode     = (uint8_t)mode;
    h->flags    = flags;
    h->eff_bits_x10 = (uint8_t)eff_bits_x10;
    h->n        = n;
    h->trig_index = trig_index;
    h->dt_ps    = dt_ps;
    h->fs_hz    = fs_hz;
    h->freq_mhz = freq_mhz;
    h->trig_level = trig_level;
    h->vmin = m->vmin;  h->vmax = m->vmax;
    h->vavg = m->vavg;  h->vrms = m->vrms;
    h->coverage_permille = coverage_permille;
    h->checksum = sum;
}
