/**
 * @file    board.h
 * @brief   Relojes (250 MHz), SysTick, GPIO y UART de depuración.
 */
#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>
#include "stm32h5xx.h"

void     board_init(void);
uint32_t board_millis(void);

/* Configura un pin en función alternativa (af = 0..15). */
void     gpio_set_af(GPIO_TypeDef *port, uint32_t pin, uint32_t af, uint32_t pull);
/* Configura un pin como entrada con pull (0 = nada, 1 = pull-up, 2 = pull-down). */
void     gpio_set_input(GPIO_TypeDef *port, uint32_t pin, uint32_t pull);
void     gpio_set_analog(GPIO_TypeDef *port, uint32_t pin);

/* Depuración por USART2 (PA2). No bloquea más de lo que tarda en salir. */
void     dbg_puts(const char *s);

/* Hook del tick de 1 ms (lo implementa encoder.c). */
void     board_tick_1ms_hook(void);

#endif /* BOARD_H */
