/**
 * @file    board.h
 * @brief   Relojes (250 MHz), SysTick, GPIO y UART de depuración.
 */
#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>
#include <stdbool.h>
#include "stm32h5xx.h"

void     board_init(void);
uint32_t board_millis(void);

/* Configura un pin en función alternativa (af = 0..15). */
void     gpio_set_af(GPIO_TypeDef *port, uint32_t pin, uint32_t af, uint32_t pull);
/* Configura un pin como entrada con pull (0 = nada, 1 = pull-up, 2 = pull-down). */
void     gpio_set_input(GPIO_TypeDef *port, uint32_t pin, uint32_t pull);
void     gpio_set_analog(GPIO_TypeDef *port, uint32_t pin);

/* USART2 (PA2) a 2 Mbaud: texto de estado + tramas del osciloscopio.
 * dbg_puts() y uart_write_blocking() esperan a que termine el DMA.        */
void     dbg_puts(const char *s);
void     uart_write_blocking(const void *data, uint32_t len);
void     uart_send_dma(const void *data, uint32_t len);   /* no bloquea */
bool     uart_tx_busy(void);

void     delay_us(uint32_t us);

/* Hook del tick de 1 ms (lo implementa encoder.c). */
void     board_tick_1ms_hook(void);

#endif /* BOARD_H */
