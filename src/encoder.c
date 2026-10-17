/**
 * @file    encoder.c
 * @brief   Encoders en modo "encoder x4" de los timers: cuentan solos, sin
 *          interrupciones ni CPU. El filtro digital de entrada del timer
 *          elimina los rebotes cortos de los contactos.
 *
 *   Encoder FRECUENCIA   -> TIM1 (PA8 = CH1, PA9 = CH2), pulsador PA10
 *   Encoder FORMA DE ONDA -> TIM3 (PA6 = CH1, PA7 = CH2), pulsador PB0
 *
 * TIM1 no se usa para el DDS: es el único timer avanzado y su par PA8/PA9
 * queda libre y cómodo para un encoder.
 */
#include "encoder.h"
#include "board.h"
#include "dds_config.h"

typedef struct {
    TIM_TypeDef  *tim;
    int8_t        dir;
    uint16_t      last_cnt;
    int32_t       residue;          /* cuentas que todavía no hacen un click */
    GPIO_TypeDef *btn_port;
    uint8_t       btn_pin;
    uint8_t       btn_stable;       /* 1 = suelto, 0 = apretado              */
    uint8_t       btn_count;
    volatile uint8_t btn_event;
} enc_t;

static enc_t s_enc[ENC_COUNT] = {
    [ENC_FREQ] = { .tim = TIM1, .dir = ENC_FREQ_DIR, .btn_port = GPIOA, .btn_pin = 10U, .btn_stable = 1U },
    [ENC_WAVE] = { .tim = TIM3, .dir = ENC_WAVE_DIR, .btn_port = GPIOB, .btn_pin = 0U,  .btn_stable = 1U },
};

static void timer_encoder_mode(TIM_TypeDef *tim)
{
    tim->CR1   = (2UL << TIM_CR1_CKD_Pos);   /* tDTS = 4·tCK = 16 ns          */
    tim->ARR   = 0xFFFFU;
    /* CC1S = CC2S = 01 (TI1/TI2 como entradas) con filtro máximo:
     * ICxF = 1111 -> fSAMPLING = fDTS/32, N = 8  => ~4 us de filtro.        */
    tim->CCMR1 = (1UL << TIM_CCMR1_CC1S_Pos) | (0xFUL << TIM_CCMR1_IC1F_Pos)
               | (1UL << TIM_CCMR1_CC2S_Pos) | (0xFUL << TIM_CCMR1_IC2F_Pos);
    tim->CCER  = 0U;                         /* polaridad no invertida        */
    tim->SMCR  = (tim->SMCR & ~TIM_SMCR_SMS)
               | (3UL << TIM_SMCR_SMS_Pos);  /* SMS = 0011: encoder x4         */
    tim->CNT   = 0U;
    tim->CR1  |= TIM_CR1_CEN;
}

void encoder_init(void)
{
    RCC->APB2ENR  |= RCC_APB2ENR_TIM1EN;
    RCC->APB1LENR |= RCC_APB1LENR_TIM3EN;
    (void)RCC->APB1LENR;

    /* Entradas de cuadratura con pull-up interno (encoders a GND). */
    gpio_set_af(GPIOA, 8U, 1U, 1U);          /* TIM1_CH1 */
    gpio_set_af(GPIOA, 9U, 1U, 1U);          /* TIM1_CH2 */
    gpio_set_af(GPIOA, 6U, 2U, 1U);          /* TIM3_CH1 */
    gpio_set_af(GPIOA, 7U, 2U, 1U);          /* TIM3_CH2 */
    gpio_set_input(GPIOA, 10U, 1U);          /* pulsador FRECUENCIA */
    gpio_set_input(GPIOB, 0U, 1U);           /* pulsador FORMA      */

    timer_encoder_mode(TIM1);
    timer_encoder_mode(TIM3);
}

int32_t encoder_take_detents(enc_id_t id)
{
    enc_t *e = &s_enc[id];
    const uint16_t now = (uint16_t)e->tim->CNT;
    const int16_t  d   = (int16_t)(uint16_t)(now - e->last_cnt);   /* maneja el desborde */
    e->last_cnt = now;

    e->residue += d;
    const int32_t det = e->residue / ENC_COUNTS_PER_DETENT;   /* trunca hacia 0 */
    e->residue -= det * ENC_COUNTS_PER_DETENT;
    return det * e->dir;
}

bool encoder_button_event(enc_id_t id)
{
    enc_t *e = &s_enc[id];
    if (e->btn_event) { e->btn_event = 0U; return true; }
    return false;
}

/* Antirrebote por integración: el estado cambia solo si la entrada se mantuvo
 * distinta durante BTN_DEBOUNCE_MS muestras consecutivas de 1 ms.          */
void board_tick_1ms_hook(void)
{
    for (uint32_t i = 0; i < ENC_COUNT; i++) {
        enc_t *e = &s_enc[i];
        const uint8_t raw = (uint8_t)((e->btn_port->IDR >> e->btn_pin) & 1U);
        if (raw != e->btn_stable) {
            if (++e->btn_count >= BTN_DEBOUNCE_MS) {
                e->btn_stable = raw;
                e->btn_count  = 0U;
                if (raw == 0U) e->btn_event = 1U;   /* flanco de pulsación */
            }
        } else {
            e->btn_count = 0U;
        }
    }
}
