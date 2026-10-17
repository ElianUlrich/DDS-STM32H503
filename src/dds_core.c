/**
 * @file    dds_core.c
 * @brief   Núcleo DDS: acumulador de fase, tablas en RAM y relleno de bloques.
 *
 *  Acumulador de fase de 32 bits:
 *      fase[n+1] = fase[n] + M          (desborde natural = módulo 2^32)
 *      M = round(f_out · 2^32 / Fs)     (palabra de sintonía)
 *
 *  La fase se divide así (tabla de 4096 muestras):
 *      | 31 ........ 20 | 19 ........ 8 | 7 .... 0 |
 *      |  índice (12 b) | fracción (12) | descarte |
 *
 *  - Camino rápido (frecuencias medias/altas): y = T[índice]
 *  - Camino interpolado (frecuencias bajas):   y = T[i] + (T[i+1]-T[i])·frac
 *
 *  El canal 2 sale del mismo acumulador, así que ambos DAC quedan siempre
 *  enganchados en fase.
 */
#include <math.h>
#include <stddef.h>
#include <string.h>
#include "dds_core.h"
#include "wave_arb.h"

#define DDS_INLINE   static inline __attribute__((always_inline))
#define DDS_PHASE_90 0x40000000UL

/* Tabla + atributos de la forma de onda que contiene. */
typedef struct {
    uint16_t   tab[DDS_TABLE_SIZE + 1U];  /* +1: muestra guarda = tab[0]   */
    uint8_t    interp;                    /* ¿admite interpolación?        */
    uint8_t    wave;
} dds_table_t;

/* Doble buffer de tablas: la ISR lee "s_active" mientras el lazo principal
 * genera la próxima forma de onda en la otra; después se intercambia el
 * puntero (operación atómica de 32 bits) -> cambio de forma sin glitches
 * y sin perder la continuidad de fase. 2 x 8 KB = 16 KB de los 32 KB de RAM. */
static dds_table_t            s_tables[2];
static dds_table_t * volatile s_active = &s_tables[0];

static volatile uint32_t s_inc;          /* palabra de sintonía M           */
static volatile uint8_t  s_ch2  = CH2_CUADRATURA;
static volatile uint8_t  s_reset_req;
static uint32_t          s_phase;        /* solo la toca la ISR             */

static const char *const k_wave_names[WAVE_COUNT] = {
    "SENO", "TRIANGULAR", "SIERRA+", "SIERRA-", "CUADRADA", "SENO RECT", "ARBITRARIA"
};
static const char *const k_ch2_names[CH2_COUNT] = {
    "CUADRATURA 90", "INVERTIDA", "SYNC TTL", "IGUAL"
};

/* ------------------------------------------------------------------------ */
/*  Generación de tablas                                                    */
/* ------------------------------------------------------------------------ */
static uint16_t clamp_code(float v)
{
    if (v < (float)DDS_CODE_MIN) v = (float)DDS_CODE_MIN;
    if (v > (float)DDS_CODE_MAX) v = (float)DDS_CODE_MAX;
    return (uint16_t)(v + 0.5f);
}

static void table_generate(dds_table_t *t, dds_wave_t wave)
{
    const float span = (float)(DDS_CODE_MAX - DDS_CODE_MIN);
    const float mid  = (float)DDS_CODE_MIN + span * 0.5f;
    const float amp  = span * 0.5f;
    const float two_pi = 6.283185307f;

    for (uint32_t i = 0; i < DDS_TABLE_SIZE; i++) {
        const float x = (float)i / (float)DDS_TABLE_SIZE;     /* 0 .. <1 */
        uint16_t v;

        switch (wave) {
        default:
        case WAVE_SENO:
            v = clamp_code(mid + amp * sinf(two_pi * x));
            break;
        case WAVE_TRIANGULAR: {           /* en fase con el seno */
            float tri = (x < 0.25f) ? 4.0f * x
                      : (x < 0.75f) ? 2.0f - 4.0f * x
                      :               4.0f * x - 4.0f;
            v = clamp_code(mid + amp * tri);
            break;
        }
        case WAVE_SIERRA_ASC:              /* exacta: 1 LSB por muestra */
            v = (uint16_t)(DDS_CODE_MIN +
                ((i * (DDS_CODE_MAX - DDS_CODE_MIN + 1U)) >> DDS_TABLE_BITS));
            break;
        case WAVE_SIERRA_DESC:
            v = (uint16_t)(DDS_CODE_MAX -
                ((i * (DDS_CODE_MAX - DDS_CODE_MIN + 1U)) >> DDS_TABLE_BITS));
            break;
        case WAVE_CUADRADA:
            v = (i < DDS_TABLE_SIZE / 2U) ? DDS_CODE_MAX : DDS_CODE_MIN;
            break;
        case WAVE_SENO_RECT:               /* un "lomo" por período */
            v = clamp_code((float)DDS_CODE_MIN + span * sinf(0.5f * two_pi * x));
            break;
        case WAVE_ARB:
            v = k_wave_arb[i];
            break;
        }
        t->tab[i] = v;
    }
    t->tab[DDS_TABLE_SIZE] = t->tab[0];    /* muestra guarda */

    /* Las formas con saltos bruscos NO se interpolan: la interpolación
     * convertiría el flanco en una rampa de 1/N del período (a 1 Hz serían
     * 244 us de tiempo de subida en la cuadrada).                          */
    t->interp = (wave == WAVE_SENO) || (wave == WAVE_TRIANGULAR) ||
                (wave == WAVE_SENO_RECT) || (wave == WAVE_ARB);
    t->wave   = (uint8_t)wave;
}

/* ------------------------------------------------------------------------ */
/*  API de control (lazo principal)                                         */
/* ------------------------------------------------------------------------ */
void dds_core_init(dds_wave_t wave, uint32_t f_mhz)
{
    table_generate(&s_tables[0], wave);
    s_active = &s_tables[0];
    s_phase  = 0;
    dds_set_frequency_mhz(f_mhz);
}

uint32_t dds_tuning_word(uint32_t f_mhz)
{
    /* M = round(f[mHz] · 2^32 / (Fs · 1000)) */
    const uint64_t den = (uint64_t)DDS_FS_HZ * 1000ULL;
    return (uint32_t)((((uint64_t)f_mhz << 32) + den / 2U) / den);
}

void dds_set_frequency_mhz(uint32_t f_mhz)
{
    s_inc = dds_tuning_word(f_mhz);      /* escritura atómica de 32 bits */
}

uint32_t dds_actual_frequency_mhz(void)
{
    /* f real = M · Fs / 2^32 (lo que efectivamente sale del DAC) */
    const uint64_t den = (uint64_t)DDS_FS_HZ * 1000ULL;
    return (uint32_t)(((uint64_t)s_inc * den + (1ULL << 31)) >> 32);
}

void dds_set_waveform(dds_wave_t wave)
{
    if (wave >= WAVE_COUNT) return;
    dds_table_t *back = (s_active == &s_tables[0]) ? &s_tables[1] : &s_tables[0];

    /* La ISR nunca lee "back": la generación (unos ms) no produce glitches.
     * Si la ISR interrumpe acá, sigue usando la tabla activa vieja.        */
    table_generate(back, wave);
    __asm volatile ("" ::: "memory");    /* que la tabla esté escrita antes */
    s_active = back;                     /* intercambio atómico             */
}

void dds_set_ch2_mode(dds_ch2_mode_t mode)
{
    if (mode < CH2_COUNT) s_ch2 = (uint8_t)mode;
}

void dds_reset_phase(void)
{
    s_reset_req = 1;
}

bool dds_interp_active(void)
{
    return s_active->interp && (s_inc < DDS_INTERP_INC_MAX);
}

const char *dds_wave_name(dds_wave_t wave)
{
    return (wave < WAVE_COUNT) ? k_wave_names[wave] : "?";
}

const char *dds_ch2_name(dds_ch2_mode_t mode)
{
    return (mode < CH2_COUNT) ? k_ch2_names[mode] : "?";
}

/* ------------------------------------------------------------------------ */
/*  Motor de síntesis (ISR)                                                 */
/* ------------------------------------------------------------------------ */
/* Búsqueda directa: truncamiento de fase a 12 bits. */
DDS_INLINE uint32_t lut(const uint16_t *t, uint32_t ph)
{
    return t[ph >> DDS_PHASE_SHIFT];
}

/* Interpolación lineal con 12 bits de fracción y redondeo:
 *   y = y0 + ((y1 - y0) · frac + 2^11) >> 12                               */
DDS_INLINE uint32_t lut_interp(const uint16_t *t, uint32_t ph)
{
    const uint32_t i  = ph >> DDS_PHASE_SHIFT;
    const int32_t  y0 = t[i];
    const int32_t  y1 = t[i + 1U];                      /* usa la guarda   */
    const int32_t  fr = (int32_t)((ph >> DDS_FRAC_SHIFT) & DDS_FRAC_MASK);
    return (uint32_t)(y0 + (((y1 - y0) * fr + (1 << (DDS_FRAC_BITS - 1U)))
                            >> DDS_FRAC_BITS));
}

/* Lazo genérico. "interp" y "ch2" son constantes en cada llamada, así que el
 * compilador genera 8 lazos especializados sin ningún "if" adentro.        */
DDS_INLINE uint32_t fill_loop(uint32_t *dst, uint32_t n, const uint16_t *t,
                              uint32_t ph, uint32_t inc,
                              const int interp, const int ch2)
{
    do {
        const uint32_t a = interp ? lut_interp(t, ph) : lut(t, ph);
        uint32_t b;
        switch (ch2) {
        case CH2_CUADRATURA:
            b = interp ? lut_interp(t, ph + DDS_PHASE_90) : lut(t, ph + DDS_PHASE_90);
            break;
        case CH2_INVERTIDA:
            b = (DDS_CODE_MAX + DDS_CODE_MIN) - a;
            break;
        case CH2_SYNC:
            b = (ph & 0x80000000UL) ? DDS_CODE_MIN : DDS_CODE_MAX;
            break;
        default:
            b = a;
            break;
        }
        *dst++ = a | (b << 16);      /* formato DAC_DHR12RD */
        ph += inc;
    } while (--n);
    return ph;
}

void dds_fill(uint32_t *dst, uint32_t n)
{
    /* Se leen los parámetros UNA vez por bloque: cualquier cambio desde el
     * lazo principal entra limpio en el borde del bloque.                  */
    const dds_table_t *tb  = s_active;
    const uint16_t    *t   = tb->tab;
    const uint32_t     inc = s_inc;
    const uint32_t     ch2 = s_ch2;
    const int interp = tb->interp && (inc < DDS_INTERP_INC_MAX);
    uint32_t ph = s_phase;

    if (s_reset_req) { s_reset_req = 0; ph = 0; }
    if (n == 0U) return;

    switch ((interp ? 4U : 0U) | ch2) {
    case 0: ph = fill_loop(dst, n, t, ph, inc, 0, CH2_CUADRATURA); break;
    case 1: ph = fill_loop(dst, n, t, ph, inc, 0, CH2_INVERTIDA);  break;
    case 2: ph = fill_loop(dst, n, t, ph, inc, 0, CH2_SYNC);       break;
    case 3: ph = fill_loop(dst, n, t, ph, inc, 0, CH2_IGUAL);      break;
    case 4: ph = fill_loop(dst, n, t, ph, inc, 1, CH2_CUADRATURA); break;
    case 5: ph = fill_loop(dst, n, t, ph, inc, 1, CH2_INVERTIDA);  break;
    case 6: ph = fill_loop(dst, n, t, ph, inc, 1, CH2_SYNC);       break;
    default:ph = fill_loop(dst, n, t, ph, inc, 1, CH2_IGUAL);      break;
    }
    s_phase = ph;
}
