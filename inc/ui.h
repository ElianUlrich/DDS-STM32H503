/**
 * @file    ui.h
 * @brief   Lógica de la interfaz: encoders -> DDS / osciloscopio -> pantalla.
 *
 * Dos páginas; la pulsación LARGA del encoder FORMA cambia de una a otra.
 * El DDS sigue generando mientras se usa el osciloscopio (y viceversa).
 */
#ifndef UI_H
#define UI_H

#include <stdint.h>
#include "dds_core.h"
#include "osc.h"

typedef enum { UI_PAGE_DDS = 0, UI_PAGE_OSC } ui_page_t;

/* Estado visible del equipo. Es lo que va a dibujar la pantalla. */
typedef struct {
    ui_page_t      page;
    /* --- generador --- */
    uint32_t       freq_mhz;        /* frecuencia pedida [mHz]               */
    uint32_t       freq_real_mhz;   /* frecuencia sintetizada real [mHz]     */
    uint8_t        step_idx;        /* índice del paso (dígito con cursor)   */
    uint32_t       step_mhz;        /* paso actual del encoder [mHz]         */
    dds_wave_t     wave;
    dds_ch2_mode_t ch2;
    uint8_t        interp;          /* 1 = interpolación activa              */
    /* --- osciloscopio --- */
    osc_cfg_t      osc;
    uint32_t       osc_fs_hz;       /* frecuencia de muestreo elegida        */
    uint16_t       osc_eff_bits_x10;
} ui_state_t;

void ui_init(void);
void ui_task(void);                 /* llamar en el lazo principal           */

/* ======================================================================= */
/*  GANCHOS PARA LA PANTALLA FUTURA                                        */
/*  Están definidos como "weak" (vacíos). Cuando agregues la pantalla,     */
/*  creá display.c con estas funciones y el linker usa las tuyas:          */
/*    display_init(), display_update(estado) y display_scope(trama, datos) */
/*  (display_scope está declarada en osc.h).                               */
/* ======================================================================= */
void display_init(void);
void display_update(const ui_state_t *s);

/* Formatea una frecuencia en mHz como texto ("1234.567 Hz"). */
void ui_format_freq(uint32_t f_mhz, char *out);

#endif /* UI_H */
