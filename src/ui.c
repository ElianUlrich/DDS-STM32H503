/**
 * @file    ui.c
 * @brief   Encoder FRECUENCIA: gira = ± paso, pulsador = cambia el paso
 *                              (0,1 Hz -> 1 -> 10 -> ... -> 100 kHz -> 0,1 Hz).
 *          Encoder FORMA:      gira = forma de onda siguiente/anterior,
 *                              pulsador = modo del canal 2.
 */
#include <stddef.h>
#include "ui.h"
#include "encoder.h"
#include "dds_hw.h"
#include "board.h"

/* Pasos del encoder de frecuencia, en mHz. */
static const uint32_t k_steps_mhz[] = {
    100UL, 1000UL, 10000UL, 100000UL, 1000000UL, 10000000UL, 100000000UL
};
#define N_STEPS  (sizeof(k_steps_mhz) / sizeof(k_steps_mhz[0]))

static ui_state_t s_ui;

/* ------------------------------------------------------------------------ */
/*  Pantalla: implementaciones vacías por defecto (weak)                    */
/* ------------------------------------------------------------------------ */
__attribute__((weak)) void display_init(void) { }
__attribute__((weak)) void display_update(const ui_state_t *s) { (void)s; }

/* ------------------------------------------------------------------------ */
static char *put_u32(char *p, uint32_t v, uint32_t min_digits)
{
    char tmp[10];
    uint32_t n = 0;
    do { tmp[n++] = (char)('0' + v % 10U); v /= 10U; } while (v || n < min_digits);
    while (n) *p++ = tmp[--n];
    return p;
}

static char *put_str(char *p, const char *s)
{
    while (*s) *p++ = *s++;
    return p;
}

void ui_format_freq(uint32_t f_mhz, char *out)
{
    char *p = put_u32(out, f_mhz / 1000U, 1U);
    *p++ = '.';
    p = put_u32(p, f_mhz % 1000U, 3U);
    p = put_str(p, " Hz");
    *p = '\0';
}

static void debug_print(void)
{
#if DDS_DEBUG_UART
    char line[160], f[24];
    char *p = line;
    p = put_str(p, dds_wave_name(s_ui.wave));
    p = put_str(p, " | f=");
    ui_format_freq(s_ui.freq_mhz, f);       p = put_str(p, f);
    p = put_str(p, " (real ");
    ui_format_freq(s_ui.freq_real_mhz, f);  p = put_str(p, f);
    p = put_str(p, ") | paso ");
    ui_format_freq(s_ui.step_mhz, f);       p = put_str(p, f);
    p = put_str(p, " | CH2 ");
    p = put_str(p, dds_ch2_name(s_ui.ch2));
    p = put_str(p, s_ui.interp ? " | interp ON" : " | interp OFF");
    const uint32_t load = dds_hw_isr_load_permille();
    p = put_str(p, " | CPU ");
    p = put_u32(p, load / 10U, 1U); *p++ = '.'; p = put_u32(p, load % 10U, 1U);
    p = put_str(p, "%\r\n");
    *p = '\0';
    dbg_puts(line);
#endif
}

static void refresh(void)
{
    s_ui.freq_real_mhz = dds_actual_frequency_mhz();
    s_ui.step_mhz      = k_steps_mhz[s_ui.step_idx];
    s_ui.interp        = dds_interp_active() ? 1U : 0U;
    display_update(&s_ui);
    debug_print();
}

/* ------------------------------------------------------------------------ */
void ui_init(void)
{
    s_ui.freq_mhz = DDS_F_DEFAULT_MHZ;
    s_ui.step_idx = 3U;                     /* 100 Hz */
    s_ui.wave     = WAVE_SENO;
    s_ui.ch2      = CH2_CUADRATURA;

    dds_set_frequency_mhz(s_ui.freq_mhz);
    dds_set_ch2_mode(s_ui.ch2);

    display_init();
    dbg_puts("\r\nDDS STM32H503 - Fs = 1 MS/s, tabla 4096 x 12 bits\r\n");
    refresh();
}

void ui_task(void)
{
    bool changed = false;

    /* ---- Encoder de FRECUENCIA ---- */
    const int32_t df = encoder_take_detents(ENC_FREQ);
    if (df != 0) {
        int64_t f = (int64_t)s_ui.freq_mhz + (int64_t)df * (int64_t)k_steps_mhz[s_ui.step_idx];
        if (f < (int64_t)DDS_F_MIN_MHZ) f = DDS_F_MIN_MHZ;
        if (f > (int64_t)DDS_F_MAX_MHZ) f = DDS_F_MAX_MHZ;
        if ((uint32_t)f != s_ui.freq_mhz) {
            s_ui.freq_mhz = (uint32_t)f;
            dds_set_frequency_mhz(s_ui.freq_mhz);    /* fase continua */
            changed = true;
        }
    }
    if (encoder_button_event(ENC_FREQ)) {
        s_ui.step_idx = (uint8_t)((s_ui.step_idx + 1U) % N_STEPS);
        changed = true;
    }

    /* ---- Encoder de FORMA DE ONDA ---- */
    const int32_t dw = encoder_take_detents(ENC_WAVE);
    if (dw != 0) {
        int32_t w = ((int32_t)s_ui.wave + dw) % (int32_t)WAVE_COUNT;
        if (w < 0) w += (int32_t)WAVE_COUNT;
        s_ui.wave = (dds_wave_t)w;
        dds_set_waveform(s_ui.wave);                 /* doble buffer */
        changed = true;
    }
    if (encoder_button_event(ENC_WAVE)) {
        s_ui.ch2 = (dds_ch2_mode_t)((s_ui.ch2 + 1U) % CH2_COUNT);
        dds_set_ch2_mode(s_ui.ch2);
        changed = true;
    }

    if (changed) refresh();
}
