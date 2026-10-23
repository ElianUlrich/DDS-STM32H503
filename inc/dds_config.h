/**
 * @file    dds_config.h
 * @brief   Configuración central del generador DDS para STM32H503CBT6.
 *
 * Todo lo "ajustable" del proyecto está acá: frecuencia de muestreo,
 * tamaño de tabla, umbral de interpolación, límites de frecuencia y pines.
 */
#ifndef DDS_CONFIG_H
#define DDS_CONFIG_H

/* ======================================================================== */
/*  Relojes                                                                 */
/* ======================================================================== */
/* SYSCLK = HCLK = PCLK1 = PCLK2 = 250 MHz (máximo del H503, escala VOS0).    */
#define SYSCLK_HZ               250000000UL
/* Con prescaler APB = 1, el reloj de los timers es igual a PCLK.            */
#define TIMCLK_HZ               SYSCLK_HZ

/* ======================================================================== */
/*  Motor DDS                                                               */
/* ======================================================================== */
/* Frecuencia de muestreo del DAC (TRGO del timer).
 *   Fs = TIMCLK / (ARR + 1).  Divisores exactos de 250 MHz:
 *     1 000 000  -> ARR = 249  (recomendado: 1 MS/s es la velocidad típica
 *                               especificada para el DAC de STM32 con buffer)
 *     2 000 000  -> ARR = 124  (buffer OFF + amplificador externo)
 *     2 500 000  -> ARR =  99  (experimental)
 *     5 000 000  -> ARR =  49  (experimental, fuera de especificación)
 * Verificá "update rate" en la tabla "DAC characteristics" del DS14053.     */
#define DDS_FS_HZ               1000000UL
#define DDS_TIM_ARR             ((TIMCLK_HZ / DDS_FS_HZ) - 1UL)

#if (TIMCLK_HZ % DDS_FS_HZ) != 0
#error "DDS_FS_HZ debe dividir exactamente a TIMCLK_HZ"
#endif

/* Timer que dispara el DAC. En el H503 pueden disparar el DAC TIM1, TIM2,
 * TIM3, TIM6, TIM7 y LPTIM1/2. Por defecto TIM2 (ver README / PDF).
 *   DDS_TIMER_TIM2 -> TIM2 (TSEL = 2)
 *   DDS_TIMER_TIM6 -> TIM6 (TSEL = 5), libera TIM2 para otros usos.        */
#define DDS_TIMER_TIM2          2
#define DDS_TIMER_TIM6          6
#define DDS_TIMER               DDS_TIMER_TIM2

/* Acumulador de fase de 32 bits. Resolución en frecuencia = Fs / 2^32
 * (0,233 mHz a 1 MS/s).                                                     */
#define DDS_PHASE_BITS          32U

/* Tabla de forma de onda: 2^12 = 4096 muestras de 12 bits (en uint16_t).
 * Se guarda UNA muestra extra (índice 4096 = copia del índice 0) para que la
 * interpolación lea t[i+1] sin máscara de módulo.                            */
#define DDS_TABLE_BITS          12U
#define DDS_TABLE_SIZE          (1UL << DDS_TABLE_BITS)
#define DDS_PHASE_SHIFT         (DDS_PHASE_BITS - DDS_TABLE_BITS)   /* 20 */

/* Bits de fracción usados por la interpolación lineal (los 12 bits que
 * siguen al índice dentro del acumulador).                                  */
#define DDS_FRAC_BITS           12U
#define DDS_FRAC_SHIFT          (DDS_PHASE_SHIFT - DDS_FRAC_BITS)   /* 8  */
#define DDS_FRAC_MASK           ((1UL << DDS_FRAC_BITS) - 1UL)

/* Umbral de interpolación: se interpola cuando el paso de fase es menor que
 * DDS_INTERP_MAX_STEP muestras de tabla por muestra de salida, es decir:
 *   f_out < DDS_INTERP_MAX_STEP * Fs / N   (= 244 Hz con 1 MS/s y N = 4096).
 * Ahí cada valor de la tabla se repetiría varias veces (escalera).
 * Subilo (por ej. 16) si querés interpolar hasta más arriba: a 1 MS/s el
 * CPU tiene margen de sobra.                                                */
#ifndef DDS_INTERP_MAX_STEP
#define DDS_INTERP_MAX_STEP     1UL
#endif
#define DDS_INTERP_INC_MAX      (DDS_INTERP_MAX_STEP << DDS_PHASE_SHIFT)

/* Rango de códigos del DAC (12 bits).                                       */
#define DDS_CODE_MIN            0U
#define DDS_CODE_MAX            4095U

/* Buffer de DMA: 2 mitades (ping-pong). Cada muestra es un uint32_t con
 * CH1 en bits 11:0 y CH2 en bits 27:16 (registro DAC_DHR12RD).
 * 256 muestras por mitad -> una interrupción cada 256 us a 1 MS/s.          */
#define DDS_HALF_SAMPLES        256U
#define DDS_BUF_SAMPLES         (2U * DDS_HALF_SAMPLES)

/* ======================================================================== */
/*  Límites de la interfaz (frecuencias en milihertz)                       */
/* ======================================================================== */
#define DDS_F_MIN_MHZ           100UL                          /* 0,1 Hz   */
/* Máximo útil = 0,4·Fs (Nyquist con margen para el filtro reconstructor).  */
#define DDS_F_MAX_MHZ           ((DDS_FS_HZ / 10UL) * 4UL * 1000UL)
#define DDS_F_DEFAULT_MHZ       1000000UL                      /* 1 kHz    */

/* ======================================================================== */
/*  Pines (STM32H503CBT6, LQFP48)                                           */
/* ======================================================================== */
/*  PA4  DAC1_OUT1  (salida CH1)        analógico                           */
/*  PA5  DAC1_OUT2  (salida CH2)        analógico                           */
/*  PA8  TIM1_CH1   encoder FRECUENCIA A   AF1                              */
/*  PA9  TIM1_CH2   encoder FRECUENCIA B   AF1                              */
/*  PA10 GPIO       pulsador FRECUENCIA (cambia el paso)                    */
/*  PA6  TIM3_CH1   encoder FORMA DE ONDA A  AF2                            */
/*  PA7  TIM3_CH2   encoder FORMA DE ONDA B  AF2                            */
/*  PB0  GPIO       pulsador FORMA DE ONDA (cambia el modo de CH2)          */
/*  PA1  ADC1_INP1  ENTRADA DEL OSCILOSCOPIO 0..3,3 V      analógico         */
/*  PA2  USART2_TX  estado + tramas del osciloscopio, 2 Mbaud 8N1  AF7      */
/*  Reservados para la pantalla futura:                                     */
/*    I2C1: PB6 SCL / PB7 SDA (AF4)   ó   SPI2: PB13 SCK / PB15 MOSI (AF5), */
/*    PB12 CS, PB14 D/C, PB10 RST                                           */

/* Cuentas del timer por "click" del encoder (modo x4). Los KY-040 y la
 * mayoría de los encoders mecánicos con retén dan 4.                        */
#define ENC_COUNTS_PER_DETENT   4
/* Poné -1 si el encoder gira al revés.                                      */
#define ENC_FREQ_DIR            1
#define ENC_WAVE_DIR            1

/* Antirrebote de pulsadores: muestras de 1 ms estables.                     */
#define BTN_DEBOUNCE_MS         20U

/* Pulsación larga (cambia de página DDS <-> osciloscopio, run/stop).       */
#define BTN_LONG_MS             600U

/* USART2: texto de estado y tramas del osciloscopio (1 = habilitada).
 * 2 Mbaud = 250 MHz / 125 exactos. Lo soportan CP2102N, FT232R, CH340 y el
 * puerto virtual del ST-LINK V3. Con adaptadores lentos bajalo a 921600.    */
#define DDS_DEBUG_UART          1
#define DDS_DEBUG_BAUD          2000000UL

/* ======================================================================== */
/*  Osciloscopio                                                            */
/* ======================================================================== */
/* El STM32H503 tiene UN solo ADC (ADC1, 12 bits). Reloj del ADC:
 *   HCLK 250 MHz / 8 = 31,25 MHz  ->  conversión de 12 bits en 15 ciclos
 *   (2,5 de muestreo + 12,5) = 480 ns  ->  hasta 2 MS/s en tiempo real.
 * (Para 2,5 MS/s hacen falta 37,5 MHz, por ejemplo con el PLL2.)            */
#define OSC_ADC_PRESC_CODE      4U              /* PRESC = 0100 -> /8     */
#define OSC_ADC_CLK_HZ          (SYSCLK_HZ / 8UL)
#define OSC_ADC_CHANNEL         1U              /* PA1 = ADC1_INP1        */

/* Tiempo equivalente: Fs base 2 MS/s (ARR = 124) y hasta 7 pasos de
 * "desafinación" (ARR 125..131 = 1,98..1,89 MS/s) para cubrir todas las fases. */
#define OSC_ETS_ARR_MIN         124U
#define OSC_ETS_ARR_MAX         131U

#endif /* DDS_CONFIG_H */
