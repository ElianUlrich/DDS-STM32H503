# Generador DDS con STM32H503CBT6

Generador de funciones por síntesis digital directa (DDS) con los **dos canales
del DAC**, **TIM2** como reloj de muestreo, **GPDMA** circular y **dos encoders
rotativos** (forma de onda y frecuencia). La interfaz de la pantalla ya está
prevista: el motor DDS no hay que tocarlo para agregarla.

El diseño completo, con los diagramas de flujo, está en
[`doc/dds_diseno.pdf`](doc/dds_diseno.pdf) (fuente LaTeX en `doc/dds_diseno.tex`).

## Características

| | |
|---|---|
| Muestreo | TIM2 → TRGO → DAC, **Fs = 1 MS/s** exacto (250 MHz / 250) |
| Salidas | PA4 (CH1) y PA5 (CH2), modo dual: una escritura de 32 bits carga los dos |
| Resolución en frecuencia | Fs / 2³² = **0,233 mHz** (acumulador de fase de 32 bits) |
| Rango | 0,1 Hz – 400 kHz (0,4·Fs) |
| Tabla | 4096 × 12 bits en `uint16_t`, con doble buffer en RAM (cambio de forma sin glitch) |
| Interpolación | lineal, automática por debajo de Fs/N = 244 Hz |
| Formas de onda | seno, triangular, sierra ↑, sierra ↓, cuadrada, seno rectificado, arbitraria (ECG de ejemplo) |
| Canal 2 | cuadratura (90°), invertido, sync TTL o igual a CH1 |
| CPU | ~5 % a 1 MS/s en camino directo, ~10 % interpolando (lo mide el DWT y sale por UART) |
| Memoria | 18,5 KB de flash / 20 KB de RAM |

## Uso

| Control | Acción |
|---|---|
| Girar encoder **FRECUENCIA** | ± paso |
| Pulsar encoder **FRECUENCIA** | cambia el paso: 0,1 → 1 → 10 → 100 Hz → 1 k → 10 k → 100 kHz → 0,1 Hz… |
| Girar encoder **FORMA** | forma de onda siguiente / anterior |
| Pulsar encoder **FORMA** | modo del canal 2: 90° → invertido → sync → igual |

Mientras no esté la pantalla, el estado sale por **USART2 (PA2, 115200 8N1)**:

```
SENO | f=1000.000 Hz (real 1000.000 Hz) | paso 100.000 Hz | CH2 CUADRATURA 90 | interp OFF | CPU 4.8%
```

## Conexionado (LQFP48)

| Pin | Función | Conexión |
|---|---|---|
| PA4 | DAC1_OUT1 | salida CH1 → filtro reconstructor |
| PA5 | DAC1_OUT2 | salida CH2 → filtro reconstructor |
| PA8 / PA9 | TIM1_CH1 / CH2 (AF1) | encoder FRECUENCIA A / B |
| PA10 | entrada con pull-up | pulsador FRECUENCIA |
| PA6 / PA7 | TIM3_CH1 / CH2 (AF2) | encoder FORMA A / B |
| PB0 | entrada con pull-up | pulsador FORMA |
| PA2 | USART2_TX (AF7) | depuración (opcional) |
| PB6 / PB7 | I2C1 | *reservado para la pantalla I²C* |
| PB13 / PB15 / PB12 / PB14 / PB10 | SPI2 + GPIO | *reservado para la pantalla SPI* |

Los encoders van con el común a GND. Se recomienda una red RC de 10 kΩ / 10 nF en A y B.

## Compilar y grabar

### Opción A: Makefile (arm-none-eabi-gcc)

```bash
make cmsis      # baja los headers CMSIS oficiales de ST (una sola vez)
make            # -> build/dds.elf, .hex, .bin
make flash      # graba con STM32_Programmer_CLI por SWD
make test       # prueba el núcleo DDS en la PC (no necesita placa)
```

### Opción B: STM32CubeIDE

1. *File → New → Makefile Project with Existing Code*, apuntando a esta carpeta y con el toolchain *MCU ARM GCC*.
2. Correr `make cmsis` una vez (o copiar `cmsis-device-h5` y `cmsis-core` dentro de `cmsis/`).
3. Para depurar: *Debug Configurations → STM32 C/C++ Application*, eligiendo `build/dds.elf`.

Si preferís un proyecto STM32Cube generado por CubeMX, copiá `src/*.c` e `inc/*.h`.
Después excluí el `main.c` generado y borrá el `SysTick_Handler` de
`stm32h5xx_it.c`, porque `board.c` ya lo define. No configures TIM1/2/3, el DAC
ni el GPDMA en el `.ioc`: el código los configura por registros.

## Estructura

```
inc/dds_config.h   <- TODO lo configurable: Fs, tamaño de tabla, umbral de interpolación, pines
src/dds_core.c     núcleo DDS en C puro: tablas, palabra de sintonía, lazos de síntesis
src/dds_hw.c       TIM2 + DAC1 dual + GPDMA1 circular (lista enlazada) + ISR
src/encoder.c      TIM1/TIM3 en modo encoder x4 + antirrebote de pulsadores
src/ui.c           lógica de la interfaz + gancho de la pantalla (weak)
src/board.c        reloj de 250 MHz (HSI + PLL1, VOS0), SysTick, GPIO, UART
tools/gen_tabla.py graba formas de onda arbitrarias (fórmula o CSV) -> inc/wave_arb.h
test/test_host.c   pruebas del núcleo en la PC
doc/dds_diseno.tex documento de diseño con diagramas de flujo (TikZ)
```

## Decisiones principales (detalle en el PDF)

- **TIM2 y no TIM1:** en el H503 los dos pueden disparar el DAC, a 250 MHz.
  TIM1 es el único timer avanzado y se aprovecha como encoder. Para usar TIM6,
  poné `#define DDS_TIMER DDS_TIMER_TIM6` y TIM2 queda libre.
- **Tabla de 4096 × 12 bits:** el SFDR por truncamiento de fase (≈ 72 dBc)
  coincide con el techo de un DAC de 12 bits. La sierra queda exacta (1 LSB por
  muestra) y con doble buffer entra en la RAM.
- **Interpolación lineal en baja frecuencia:** por debajo de 244 Hz elimina la
  escalera. Error medido de 0,99 LSB contra 2,96 LSB sin interpolar. No se
  aplica a la cuadrada ni a la sierra, para no redondear los flancos.
- **Máximo rendimiento:** el CPU nunca escribe el DAC. Cada muestra de los dos
  canales es una sola transferencia DMA de 32 bits. El lazo de síntesis viene
  especializado en 8 variantes sin `if` internos y se ejecuta desde la ICACHE.

## Para llevar Fs más arriba

`DDS_FS_HZ` acepta 2 000 000, 2 500 000 y 5 000 000 (divisores exactos de
250 MHz). Por encima de 1 MS/s:

1. Cambiá `MODE1/MODE2` a `010` en `dac_init()` (buffer interno OFF).
2. Agregá un amplificador operacional rápido como buffer externo.
3. Revisá *update rate* y *settling time* en la tabla "DAC characteristics" del
   datasheet DS14053, que no pude consultar desde acá.

## Agregar la pantalla

Creá `src/display.c` con:

```c
#include "ui.h"
void display_init(void)                  { /* inicializar I2C1 o SPI2 y el controlador */ }
void display_update(const ui_state_t *s) { /* dibujar s->freq_mhz, s->step_idx (cursor), s->wave, s->ch2 ... */ }
```

y agregalo a `SRCS` en el Makefile. Como `ui.c` las define `weak`, el linker usa
las tuyas automáticamente.

## Estado de verificación

- ✅ Compila sin warnings con arm-none-eabi-gcc 13.2 contra los headers CMSIS oficiales de ST.
- ✅ `make test`: núcleo DDS verificado en PC (palabra de sintonía, error de amplitud, interpolación, modos de CH2, cambio de forma).
- ⚠️ **No está probado sobre una placa real.** Los registros (RCC, PWR, FLASH,
  GPDMA, DAC, TIM) y los números de AF/pines/solicitudes DMA se tomaron de los
  headers CMSIS, del HAL y de la base de pines oficiales de ST para el
  STM32H503CBTx. Aun así, conviene verificar la primera puesta en marcha con
  osciloscopio.
