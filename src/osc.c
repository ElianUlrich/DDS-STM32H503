/**
 * @file    osc.c
 * @brief   Osciloscopio: máquina de estados que corre en el lazo principal,
 *          en paralelo con el DDS (que trabaja solo, por DMA).
 *
 *     LIBRE ──► CAPTURANDO ──► (procesar) ──► ENVIANDO ──► LIBRE
 *                    │               │
 *                    │               └─ ETS: si conviene otra Fs, recaptura
 *                    └─ NORMAL sin trigger: recaptura sin enviar
 */
#include <stddef.h>
#include "osc.h"
#include "osc_hw.h"
#include "board.h"
#include "dds_config.h"

/* Frecuencias de muestreo en tiempo real (todas dividen exacto 250 MHz). */
static const uint32_t k_fs_table[] = {
    2000000UL, 1000000UL, 500000UL, 200000UL, 100000UL, 50000UL,
    20000UL, 10000UL, 5000UL, 2000UL, 1000UL
};
#define FS_COUNT  (sizeof(k_fs_table) / sizeof(k_fs_table[0]))

static const char *const k_mode_names[OSC_MODE_COUNT] = { "AUTO", "NORMAL", "ETS" };

static osc_timebase_t s_tb_table[FS_COUNT];
static uint16_t s_cap[OSC_N_CAPTURE];           /* 4 KB: captura cruda        */
static uint16_t s_ets[OSC_W_ETS];               /* 1 KB: reconstrucción ETS   */
static osc_frame_hdr_t s_hdr;

static osc_cfg_t s_cfg = { false, OSC_MODE_AUTO, 0U, 32768U, true };
static osc_timebase_t s_tb;                     /* la de la captura en curso  */
static uint16_t s_ets_arr = OSC_ETS_ARR_MIN;
static uint8_t  s_retunes;
static volatile bool s_restart;

static enum { ST_IDLE, ST_CAPTURING, ST_SENDING } s_state = ST_IDLE;

__attribute__((weak)) void display_scope(const osc_frame_hdr_t *h, const uint16_t *pts)
{
    (void)h; (void)pts;
}

/* ------------------------------------------------------------------------ */
void osc_init(void)
{
    for (uint32_t i = 0; i < FS_COUNT; i++) {
        (void)osc_timebase_calc(k_fs_table[i], TIMCLK_HZ, OSC_ADC_CLK_HZ, &s_tb_table[i]);
    }
    osc_hw_init();
}

void osc_set_config(const osc_cfg_t *cfg)
{
    s_cfg = *cfg;
    if (s_cfg.fs_idx >= FS_COUNT) s_cfg.fs_idx = FS_COUNT - 1U;
    s_restart = true;
}

uint8_t osc_fs_count(void)               { return (uint8_t)FS_COUNT; }
uint32_t osc_fs_hz(uint8_t idx)          { return k_fs_table[idx < FS_COUNT ? idx : 0]; }
const osc_timebase_t *osc_timebase(uint8_t idx) { return &s_tb_table[idx < FS_COUNT ? idx : 0]; }
const char *osc_mode_name(osc_mode_t m)  { return (m < OSC_MODE_COUNT) ? k_mode_names[m] : "?"; }

/* ------------------------------------------------------------------------ */
static void start_capture(void)
{
    if (s_cfg.mode == OSC_MODE_ETS) {
        /* Siempre a ~2 MS/s, con el ARR elegido para cubrir todas las fases. */
        (void)osc_timebase_calc(TIMCLK_HZ / (s_ets_arr + 1UL), TIMCLK_HZ,
                                OSC_ADC_CLK_HZ, &s_tb);
    } else {
        s_tb = s_tb_table[s_cfg.fs_idx];
    }
    osc_hw_start(&s_tb, s_cap, OSC_N_CAPTURE);
}

static void send_frame(osc_mode_t mode, uint8_t flags, uint16_t eff_x10,
                       const uint16_t *pts, uint16_t n, uint16_t trig_index,
                       uint32_t dt_ps, float fs, const osc_meas_t *m, uint16_t cov)
{
    const uint32_t freq_mhz = (m->period_valid && m->period > 0.0f)
                            ? (uint32_t)(fs / m->period * 1000.0f) : 0U;
    if (m->period_valid) flags |= OSC_F_FREQ_VALID;

    osc_frame_build(&s_hdr, mode, flags, eff_x10, pts, n, trig_index, dt_ps,
                    (uint32_t)fs, freq_mhz, s_cfg.level, m, cov);
    display_scope(&s_hdr, pts);

    uart_write_blocking(&s_hdr, sizeof s_hdr);              /* 38 bytes  */
    uart_send_dma(pts, (uint32_t)n * sizeof(uint16_t));     /* sin bloquear */
}

/* Tiempo real: trigger por software sobre la captura, con 25 % de pretrigger. */
static bool process_realtime(void)
{
    osc_meas_t m;
    osc_measure(s_cap, OSC_N_CAPTURE, &m);

    const uint32_t pre = OSC_W_REAL / 4U;
    const int32_t t = osc_find_trigger(s_cap, pre, OSC_N_CAPTURE - (OSC_W_REAL - pre),
                                       s_cfg.level, s_cfg.rising);
    uint32_t start = 0U;
    uint8_t flags = 0U;
    if (t >= 0) {
        start = (uint32_t)t - pre;
        flags |= OSC_F_TRIGGERED;
    } else if (s_cfg.mode == OSC_MODE_NORMAL) {
        return false;                          /* esperar un disparo */
    }

    const float fs = (float)s_tb.fs_hz;
    send_frame(s_cfg.mode == OSC_MODE_ETS ? OSC_MODE_AUTO : s_cfg.mode, flags,
               s_tb.eff_bits_x10, &s_cap[start], OSC_W_REAL,
               (t >= 0) ? (uint16_t)pre : 0U,
               (uint32_t)(1000000000000ULL / s_tb.fs_hz), fs, &m, 0U);
    return true;
}

/* Tiempo equivalente: ver la explicación en osc_core.c. */
static bool process_ets(void)
{
    osc_meas_t m;
    osc_measure(s_cap, OSC_N_CAPTURE, &m);

    /* Hace falta una señal periódica con al menos 4 períodos en la captura;
     * si no, se muestra en tiempo real (para eso no hace falta ETS).       */
    if (!m.period_valid || m.period > (float)(OSC_N_CAPTURE / 4U)) {
        return process_realtime();
    }

    /* ¿Otra Fs cercana cubre mejor las fases? -> recapturar con esa. */
    uint32_t cov;
    const uint16_t best = osc_ets_best_arr(m.period, s_ets_arr, OSC_ETS_ARR_MIN,
                                           OSC_ETS_ARR_MAX, OSC_N_CAPTURE, &cov);
    if (best != s_ets_arr && s_retunes < 2U) {
        s_ets_arr = best;
        s_retunes++;
        return false;
    }
    s_retunes = 0U;

    cov = osc_ets_reconstruct(s_cap, OSC_N_CAPTURE, m.period, m.t0, s_ets);
    const float fs    = (float)TIMCLK_HZ / (float)(s_ets_arr + 1U);
    const float dt_ps = m.period / fs / (float)OSC_ETS_BINS * 1e12f;
    /* Cada punto promedia ~N/B = 8 muestras: +1,5 bits de resolución. */
    send_frame(OSC_MODE_ETS, OSC_F_TRIGGERED | ((cov >= 240U) ? OSC_F_ETS_OK : 0U),
               135U, s_ets, OSC_W_ETS, 0U, (uint32_t)dt_ps, fs, &m,
               (uint16_t)(cov * 1000U / OSC_ETS_BINS));
    return true;
}

/* ------------------------------------------------------------------------ */
void osc_task(void)
{
    if (s_restart) {
        s_restart = false;
        if (s_state == ST_CAPTURING) osc_hw_stop();
        if (s_state != ST_SENDING) s_state = ST_IDLE;
        s_retunes = 0U;
    }

    switch (s_state) {
    case ST_IDLE:
        if (!s_cfg.run || uart_tx_busy()) return;
        start_capture();
        s_state = ST_CAPTURING;
        break;

    case ST_CAPTURING:
        if (!osc_hw_done()) return;
        osc_normalize(s_cap, OSC_N_CAPTURE, s_tb.sw_shift);   /* a 16 bits */
        {
            const bool sent = (s_cfg.mode == OSC_MODE_ETS) ? process_ets()
                                                           : process_realtime();
            s_state = sent ? ST_SENDING : ST_IDLE;
        }
        break;

    case ST_SENDING:
        /* El DMA de la UART lee s_cap/s_ets: no capturar hasta que termine. */
        if (!uart_tx_busy()) s_state = ST_IDLE;
        break;
    }
}
