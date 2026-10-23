/**
 * @file    osc.h
 * @brief   Osciloscopio de 0 a 3,3 V: adquisición, procesamiento y envío.
 */
#ifndef OSC_H
#define OSC_H

#include <stdint.h>
#include <stdbool.h>
#include "osc_core.h"

typedef struct {
    bool       run;          /* adquiriendo                                  */
    osc_mode_t mode;         /* AUTO / NORMAL / ETS                          */
    uint8_t    fs_idx;       /* índice en la tabla de frecuencias de muestreo */
    uint16_t   level;        /* nivel de trigger (16 bits: 0..3,3 V)         */
    bool       rising;       /* flanco de trigger                            */
} osc_cfg_t;

void        osc_init(void);
void        osc_task(void);                     /* llamar en el lazo principal */
void        osc_set_config(const osc_cfg_t *cfg);
uint8_t     osc_fs_count(void);
uint32_t    osc_fs_hz(uint8_t idx);
const osc_timebase_t *osc_timebase(uint8_t idx);
const char *osc_mode_name(osc_mode_t mode);

/* GANCHO PARA LA PANTALLA: se llama con cada trama lista (weak, vacía).
 * pts tiene h->n muestras de 16 bits (0..65535 = 0..3,3 V), separadas
 * h->dt_ps picosegundos.                                                    */
void        display_scope(const osc_frame_hdr_t *h, const uint16_t *pts);

#endif /* OSC_H */
