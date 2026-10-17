/**
 * @file    dds_core.h
 * @brief   Núcleo DDS: tablas, palabra de sintonía y relleno de bloques.
 *
 * Este módulo es C puro (no toca registros), así que se puede compilar y
 * probar en la PC (ver test/test_host.c).
 */
#ifndef DDS_CORE_H
#define DDS_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include "dds_config.h"

typedef enum {
    WAVE_SENO = 0,
    WAVE_TRIANGULAR,
    WAVE_SIERRA_ASC,
    WAVE_SIERRA_DESC,
    WAVE_CUADRADA,
    WAVE_SENO_RECT,      /* |sen|, onda completa rectificada              */
    WAVE_ARB,            /* arbitraria grabada en flash (wave_arb.h)      */
    WAVE_COUNT
} dds_wave_t;

/* Qué saca el canal 2 (PA5) respecto del canal 1 (PA4). */
typedef enum {
    CH2_CUADRATURA = 0,  /* misma forma, desfasada 90°                    */
    CH2_INVERTIDA,       /* 4095 - CH1 (salida diferencial)               */
    CH2_SYNC,            /* cuadrada TTL 0/3,3 V en fase: trigger del osc. */
    CH2_IGUAL,           /* copia exacta de CH1                           */
    CH2_COUNT
} dds_ch2_mode_t;

/* Inicializa las tablas y deja el motor listo (no arranca el hardware). */
void        dds_core_init(dds_wave_t wave, uint32_t f_mhz);

/* --- Llamar desde el lazo principal (contexto de baja prioridad) --- */
void        dds_set_frequency_mhz(uint32_t f_mhz);
void        dds_set_waveform(dds_wave_t wave);
void        dds_set_ch2_mode(dds_ch2_mode_t mode);

uint32_t    dds_tuning_word(uint32_t f_mhz);
uint32_t    dds_actual_frequency_mhz(void);
bool        dds_interp_active(void);
const char *dds_wave_name(dds_wave_t wave);
const char *dds_ch2_name(dds_ch2_mode_t mode);

/* --- Llamar desde la ISR del DMA: genera n muestras empaquetadas --- */
void        dds_fill(uint32_t *dst, uint32_t n);

/* Reinicia la fase (por ejemplo, para sincronizar con otro equipo). */
void        dds_reset_phase(void);

#endif /* DDS_CORE_H */
