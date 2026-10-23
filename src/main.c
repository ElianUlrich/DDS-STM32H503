/**
 * @file    main.c
 * @brief   Generador de funciones DDS + osciloscopio - STM32H503CBT6
 *
 *  GENERADOR
 *  - Reloj de muestreo: TIM2 a Fs = 1 MS/s (250 MHz / 250).
 *  - Salidas: DAC1_OUT1 (PA4) y DAC1_OUT2 (PA5), modo dual, por GPDMA1.
 *  - Frecuencia 0,1 Hz .. 400 kHz, resolución 0,233 mHz, 7 formas de onda.
 *  - Interpolación lineal automática en frecuencias bajas.
 *
 *  OSCILOSCOPIO (0 a 3,3 V)
 *  - Entrada PA1 -> ADC1, disparado por TIM6, hasta 2 MS/s en tiempo real.
 *  - Sobremuestreo por hardware en bases lentas: hasta 16 bits.
 *  - Tiempo equivalente (ETS) para señales periódicas: hasta ~200 MS/s
 *    equivalentes con un solo ADC.
 *  - Tramas por USART2 a 2 Mbaud -> tools/osciloscopio.py en la PC.
 *
 *  Encoder 1 (TIM1) y encoder 2 (TIM3) manejan las dos páginas.
 */
#include "board.h"
#include "dds_core.h"
#include "dds_hw.h"
#include "encoder.h"
#include "osc.h"
#include "ui.h"

int main(void)
{
    board_init();                                   /* 250 MHz, SysTick, UART */
    dds_core_init(WAVE_SENO, DDS_F_DEFAULT_MHZ);    /* tablas + palabra M     */
    encoder_init();                                 /* TIM1, TIM3, pulsadores */
    osc_init();                                     /* ADC1 + TIM6 + GPDMA    */
    ui_init();
    dds_hw_start();                                 /* TIM2 + DAC + GPDMA     */

    for (;;) {
        ui_task();
        osc_task();
        __WFI();      /* duerme hasta la próxima IRQ (DMA o SysTick) */
    }
}
