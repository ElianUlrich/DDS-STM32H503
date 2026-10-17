/**
 * @file    ui.h
 * @brief   Lógica de la interfaz: encoders -> parámetros del DDS -> pantalla.
 */
#ifndef UI_H
#define UI_H

#include <stdint.h>
#include "dds_core.h"

/* Estado visible del generador. Es lo que va a dibujar la pantalla. */
typedef struct {
    uint32_t       freq_mhz;        /* frecuencia pedida [mHz]               */
    uint32_t       freq_real_mhz;   /* frecuencia sintetizada real [mHz]     */
    uint8_t        step_idx;        /* índice del paso (dígito con cursor)   */
    uint32_t       step_mhz;        /* paso actual del encoder [mHz]         */
    dds_wave_t     wave;
    dds_ch2_mode_t ch2;
    uint8_t        interp;          /* 1 = interpolación activa              */
} ui_state_t;

void ui_init(void);
void ui_task(void);                 /* llamar en el lazo principal           */

/* ======================================================================= */
/*  GANCHO PARA LA PANTALLA FUTURA                                         */
/*  Está definido como "weak" (vacío) en ui.c. Cuando agregues la pantalla, */
/*  creá display.c con estas dos funciones y el linker usa las tuyas:      */
/* ======================================================================= */
void display_init(void);
void display_update(const ui_state_t *s);

/* Formatea una frecuencia en mHz como texto ("1234.567 Hz"). */
void ui_format_freq(uint32_t f_mhz, char *out);

#endif /* UI_H */
