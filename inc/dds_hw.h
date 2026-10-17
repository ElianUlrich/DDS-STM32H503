/**
 * @file    dds_hw.h
 * @brief   Hardware del DDS: TIM2 (reloj de muestreo) -> DAC1 dual <- GPDMA1.
 */
#ifndef DDS_HW_H
#define DDS_HW_H

#include <stdint.h>
#include <stdbool.h>

/* Configura TIM2, DAC1 (ambos canales) y GPDMA1 canal 0, pre-carga el buffer
 * y arranca la generación. Llamar después de dds_core_init().              */
void     dds_hw_start(void);

/* Carga de CPU de la ISR (peor caso medido, en décimas de %). */
uint32_t dds_hw_isr_load_permille(void);

/* true si el DAC reportó un underrun de DMA (no debería pasar nunca). */
bool     dds_hw_underrun(void);

/* Contador de errores de transferencia del GPDMA. */
extern volatile uint32_t dds_dma_errors;

#endif /* DDS_HW_H */
