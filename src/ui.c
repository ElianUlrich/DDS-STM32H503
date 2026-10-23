/**
 * @file    ui.c
 * @brief   Interfaz con dos encoders y dos páginas.
 *
 *  PÁGINA DDS
 *    Encoder FRECUENCIA: gira = ± paso, pulsar = cambia el paso
 *                        (0,1 Hz -> 1 -> 10 -> ... -> 100 kHz -> 0,1 Hz).
 *    Encoder FORMA:      gira = forma de onda, pulsar = modo del canal 2.
 *
 *  PÁGINA OSCILOSCOPIO
 *    Encoder FRECUENCIA: gira = frecuencia de muestreo (horario = más rápido),
 *                        pulsar = modo AUTO -> NORMAL -> ETS,
 *                        mantener = RUN / STOP.
 *    Encoder FORMA:      gira = nivel de trigger (pasos de 33 mV),
 *                        pulsar = flanco ascendente / descendente.
 *
 *  Mantener apretado el encoder FORMA cambia de página.
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
#define N_STEPS        (sizeof(k_steps_mhz) / sizeof(k_steps_mhz[0]))
#define TRIG_STEP      655        /* 1 % de la escala de 16 bits = 33 mV */

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
    char line[180], f[24];
    char *p = line;
    if (s_ui.page == UI_PAGE_DDS) {
        p = put_str(p, "DDS | ");
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
        *p++ = '%';
    } else {
        const uint32_t mv = (uint32_t)s_ui.osc.level * OSC_VREF_MV / OSC_FULL_SCALE;
        p = put_str(p, "OSC | ");
        p = put_str(p, osc_mode_name(s_ui.osc.mode));
        p = put_str(p, " | Fs ");
        p = put_u32(p, s_ui.osc_fs_hz, 1U);
        p = put_str(p, " S/s | ");
        p = put_u32(p, s_ui.osc_eff_bits_x10 / 10U, 1U); *p++ = '.';
        p = put_u32(p, s_ui.osc_eff_bits_x10 % 10U, 1U);
        p = put_str(p, " bits | trigger ");
        p = put_str(p, s_ui.osc.rising ? "+" : "-");
        p = put_str(p, " ");
        p = put_u32(p, mv / 1000U, 1U); *p++ = '.'; p = put_u32(p, mv % 1000U, 3U);
        p = put_str(p, " V | ");
        p = put_str(p, s_ui.osc.run ? "RUN" : "STOP");
    }
    p = put_str(p, "\r\n");
    *p = '\0';
    dbg_puts(line);
#endif
}

static void refresh(void)
{
    s_ui.freq_real_mhz = dds_actual_frequency_mhz();
    s_ui.step_mhz      = k_steps_mhz[s_ui.step_idx];
    s_ui.interp        = dds_interp_active() ? 1U : 0U;
    s_ui.osc_fs_hz     = (s_ui.osc.mode == OSC_MODE_ETS) ? 2000000UL : osc_fs_hz(s_ui.osc.fs_idx);
    s_ui.osc_eff_bits_x10 = (s_ui.osc.mode == OSC_MODE_ETS)
                          ? 135U : osc_timebase(s_ui.osc.fs_idx)->eff_bits_x10;
    display_update(&s_ui);
    debug_print();
}

/* ------------------------------------------------------------------------ */
void ui_init(void)
{
    s_ui.page     = UI_PAGE_DDS;
    s_ui.freq_mhz = DDS_F_DEFAULT_MHZ;
    s_ui.step_idx = 3U;                     /* 100 Hz */
    s_ui.wave     = WAVE_SENO;
    s_ui.ch2      = CH2_CUADRATURA;
    s_ui.osc      = (osc_cfg_t){ .run = false, .mode = OSC_MODE_AUTO, .fs_idx = 0U,
                                 .level = OSC_FULL_SCALE / 2U, .rising = true };

    dds_set_frequency_mhz(s_ui.freq_mhz);
    dds_set_ch2_mode(s_ui.ch2);
    osc_set_config(&s_ui.osc);

    display_init();
    dbg_puts("\r\nDDS + osciloscopio STM32H503 - mantener FORMA para cambiar de pagina\r\n");
    refresh();
}

static bool task_dds(void)
{
    bool changed = false;

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
    return changed;
}

static bool task_osc(void)
{
    bool changed = false;
    osc_cfg_t *c = &s_ui.osc;

    /* Tabla ordenada de más rápida a más lenta: horario = índice menor. */
    const int32_t df = encoder_take_detents(ENC_FREQ);
    if (df != 0) {
        int32_t i = (int32_t)c->fs_idx - df;
        if (i < 0) i = 0;
        if (i >= (int32_t)osc_fs_count()) i = (int32_t)osc_fs_count() - 1;
        if ((uint8_t)i != c->fs_idx) { c->fs_idx = (uint8_t)i; changed = true; }
    }
    if (encoder_button_event(ENC_FREQ)) {
        c->mode = (osc_mode_t)((c->mode + 1U) % OSC_MODE_COUNT);
        changed = true;
    }
    if (encoder_button_long(ENC_FREQ)) {
        c->run = !c->run;
        changed = true;
    }

    const int32_t dw = encoder_take_detents(ENC_WAVE);
    if (dw != 0) {
        int32_t l = (int32_t)c->level + dw * TRIG_STEP;
        if (l < 0) l = 0;
        if (l > 65535) l = 65535;
        c->level = (uint16_t)l;
        changed = true;
    }
    if (encoder_button_event(ENC_WAVE)) {
        c->rising = !c->rising;
        changed = true;
    }

    if (changed) osc_set_config(c);
    return changed;
}

void ui_task(void)
{
    bool changed = false;

    if (encoder_button_long(ENC_WAVE)) {
        s_ui.page = (s_ui.page == UI_PAGE_DDS) ? UI_PAGE_OSC : UI_PAGE_DDS;
        s_ui.osc.run = (s_ui.page == UI_PAGE_OSC);   /* adquiere en su página */
        osc_set_config(&s_ui.osc);
        (void)encoder_take_detents(ENC_FREQ);        /* descartar giros viejos */
        (void)encoder_take_detents(ENC_WAVE);
        changed = true;
    }

    changed |= (s_ui.page == UI_PAGE_DDS) ? task_dds() : task_osc();

    if (s_ui.page == UI_PAGE_DDS) (void)encoder_button_long(ENC_FREQ);   /* sin uso */

    if (changed) refresh();
}
