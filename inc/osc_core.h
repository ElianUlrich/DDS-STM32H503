/**
 * @file    osc_core.h
 * @brief   Núcleo del osciloscopio: base de tiempo, trigger, mediciones y
 *          reconstrucción en tiempo equivalente (ETS). C puro, sin registros,
 *          se prueba en la PC con test/test_osc.c.
 *
 * Todas las muestras se manejan normalizadas a 16 bits:
 *     0 .. 65535  <->  0 .. 3,3 V      (V = código · 3,3 / 65536)
 * así el formato no cambia aunque el sobremuestreo dé 12, 14 o 16 bits.
 */
#ifndef OSC_CORE_H
#define OSC_CORE_H

#include <stdint.h>
#include <stdbool.h>

#define OSC_N_CAPTURE   2048U   /* muestras por adquisición (4 KB de RAM)        */
#define OSC_W_REAL      1024U   /* ventana enviada en tiempo real                */
#define OSC_ETS_BINS    256U    /* puntos por período en tiempo equivalente      */
#define OSC_W_ETS       (2U * OSC_ETS_BINS)   /* se muestran dos períodos        */
#define OSC_FULL_SCALE  65536U
#define OSC_VREF_MV     3300U

typedef enum {
    OSC_MODE_AUTO = 0,   /* tiempo real; si no hay trigger muestra igual        */
    OSC_MODE_NORMAL,     /* tiempo real; solo muestra si hubo trigger           */
    OSC_MODE_ETS,        /* tiempo equivalente: señales periódicas              */
    OSC_MODE_COUNT
} osc_mode_t;

/* Base de tiempo resuelta: timer + configuración del ADC. */
typedef struct {
    uint32_t fs_hz;          /* frecuencia de muestreo real                      */
    uint16_t psc;            /* prescaler del timer (valor de registro)          */
    uint16_t arr;            /* autorrecarga del timer (valor de registro)       */
    uint8_t  osr_log2;       /* sobremuestreo: 2^osr_log2 conversiones/muestra   */
    uint8_t  smp_code;       /* tiempo de muestreo del ADC (SMPx, 0..7)          */
    uint8_t  ovss;           /* corrimiento a la derecha por hardware            */
    uint8_t  sw_shift;       /* corrimiento a la izquierda para llegar a 16 bits */
    uint16_t eff_bits_x10;   /* resolución efectiva teórica (x10)                */
} osc_timebase_t;

/* Mediciones sobre una adquisición. */
typedef struct {
    uint16_t vmin, vmax, vavg, vrms;  /* códigos de 16 bits                      */
    bool     period_valid;
    float    period;                  /* período en muestras                     */
    float    t0;                      /* primer cruce ascendente (muestras)      */
    uint32_t ncross;                  /* cruces usados en el ajuste              */
} osc_meas_t;

/* Encabezado de cada trama enviada por la UART (little endian, 38 bytes).
 * Le siguen n muestras uint16_t. El visor de PC (tools/osciloscopio.py)
 * usa el mismo formato.                                                     */
#define OSC_MAGIC0   0xAAU
#define OSC_MAGIC1   0x55U
#define OSC_VERSION  1U
#define OSC_F_TRIGGERED   0x01U
#define OSC_F_FREQ_VALID  0x02U
#define OSC_F_ETS_OK      0x04U

typedef struct __attribute__((packed)) {
    uint8_t  magic[4];          /* 0xAA 0x55 'O' 'S'                             */
    uint8_t  version;
    uint8_t  mode;              /* osc_mode_t                                    */
    uint8_t  flags;             /* OSC_F_*                                       */
    uint8_t  eff_bits_x10;
    uint16_t n;                 /* cantidad de muestras que siguen               */
    uint16_t trig_index;        /* posición del trigger dentro de la ventana     */
    uint32_t dt_ps;             /* tiempo entre puntos [ps]                      */
    uint32_t fs_hz;             /* frecuencia de muestreo real del ADC           */
    uint32_t freq_mhz;          /* frecuencia medida [mHz]                       */
    uint16_t trig_level;
    uint16_t vmin, vmax, vavg, vrms;
    uint16_t coverage_permille; /* ETS: fracción de fases cubiertas (‰)          */
    uint16_t checksum;          /* suma de las n muestras (módulo 2^16)          */
} osc_frame_hdr_t;

/* --- Base de tiempo --- */
bool     osc_timebase_calc(uint32_t fs_hz, uint32_t timclk_hz, uint32_t adcclk_hz,
                           osc_timebase_t *tb);

/* --- Procesamiento --- */
void     osc_normalize(uint16_t *x, uint32_t n, uint8_t sw_shift);
void     osc_measure(const uint16_t *x, uint32_t n, osc_meas_t *m);
int32_t  osc_find_trigger(const uint16_t *x, uint32_t from, uint32_t to,
                          uint16_t level, bool rising);

/* --- Tiempo equivalente --- */
uint32_t osc_ets_coverage(float period, uint32_t n);
uint16_t osc_ets_best_arr(float period, uint16_t arr_now, uint16_t arr_min,
                          uint16_t arr_max, uint32_t n, uint32_t *coverage);
uint32_t osc_ets_reconstruct(const uint16_t *x, uint32_t n, float period, float t0,
                             uint16_t *out);

/* --- Trama --- */
void     osc_frame_build(osc_frame_hdr_t *h, osc_mode_t mode, uint8_t flags,
                         uint16_t eff_bits_x10, const uint16_t *pts, uint16_t n,
                         uint16_t trig_index, uint32_t dt_ps, uint32_t fs_hz,
                         uint32_t freq_mhz, uint16_t trig_level,
                         const osc_meas_t *m, uint16_t coverage_permille);

#endif /* OSC_CORE_H */
