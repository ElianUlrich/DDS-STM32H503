/**
 * @file    osc_hw.h
 * @brief   Hardware del osciloscopio: TIM6 -> TRGO -> ADC1 -> GPDMA1 canal 1.
 */
#ifndef OSC_HW_H
#define OSC_HW_H

#include <stdint.h>
#include <stdbool.h>
#include "osc_core.h"

void     osc_hw_init(void);

/* Arranca una adquisición de n muestras con la base de tiempo tb. */
void     osc_hw_start(const osc_timebase_t *tb, uint16_t *buf, uint32_t n);

/* true cuando el DMA terminó de llenar el buffer. */
bool     osc_hw_done(void);

/* Detiene timer, ADC y DMA (para cambiar de configuración en el medio). */
void     osc_hw_stop(void);

extern volatile uint32_t osc_dma_errors;

#endif /* OSC_HW_H */
