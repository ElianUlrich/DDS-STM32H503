/**
 * @file    encoder.h
 * @brief   Dos encoders rotativos por hardware (TIM1 y TIM3) + pulsadores.
 */
#ifndef ENCODER_H
#define ENCODER_H

#include <stdint.h>
#include <stdbool.h>

typedef enum { ENC_FREQ = 0, ENC_WAVE, ENC_COUNT } enc_id_t;

void    encoder_init(void);

/* Devuelve los "clicks" girados desde la última llamada (+ horario). */
int32_t encoder_take_detents(enc_id_t id);

/* true una sola vez por cada pulsación (ya sin rebote). */
bool    encoder_button_event(enc_id_t id);

#endif /* ENCODER_H */
