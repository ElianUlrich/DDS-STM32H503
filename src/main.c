/**
 * @file    main.c
 * @brief   Generador de funciones DDS - STM32H503CBT6
 *
 *  - Reloj de muestreo: TIM2 a Fs = 1 MS/s (250 MHz / 250).
 *  - Salidas: DAC1_OUT1 (PA4) y DAC1_OUT2 (PA5), modo dual, por GPDMA1.
 *  - Encoder 1 (TIM1): frecuencia 0,1 Hz .. 400 kHz, resolución 0,233 mHz.
 *  - Encoder 2 (TIM3): forma de onda (7 formas) y modo del canal 2.
 *  - Interpolación lineal automática en frecuencias bajas.
 */
#include "board.h"
#include "dds_core.h"
#include "dds_hw.h"
#include "encoder.h"
#include "ui.h"

int main(void)
{
    board_init();                                   /* 250 MHz, SysTick, UART */
    dds_core_init(WAVE_SENO, DDS_F_DEFAULT_MHZ);    /* tablas + palabra M     */
    encoder_init();                                 /* TIM1, TIM3, pulsadores */
    ui_init();
    dds_hw_start();                                 /* TIM2 + DAC + GPDMA     */

    for (;;) {
        ui_task();
        __WFI();      /* duerme hasta la próxima IRQ (DMA o SysTick) */
    }
}
