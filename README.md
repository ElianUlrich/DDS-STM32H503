# Generador DDS y osciloscopio con STM32H503CBT6

Un solo microcontrolador hace de **generador de funciones** y de
**osciloscopio de 0 a 3,3 V**:

- **Generador DDS:** usa los dos canales del DAC, con **TIM2** como reloj de
  muestreo y **GPDMA** circular.
- **Osciloscopio:** usa el ADC con **TIM6**. Tiene **sobremuestreo** por hardware
  (hasta 16 bits) y **tiempo equivalente** para señales periódicas (hasta
  ~200 MS/s equivalentes).
- **Manejo:** **dos encoders rotativos**; la pulsación larga cambia de página.

La interfaz de la pantalla ya está prevista: el motor DDS y la adquisición no
hay que tocarlos para agregarla.

El diseño completo, con los diagramas de flujo, está en
[`doc/dds_diseno.pdf`](doc/dds_diseno.pdf) (fuente LaTeX en `doc/dds_diseno.tex`).

## Características

**Generador**

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

**Osciloscopio**

| | |
|---|---|
| Entrada | PA1 → ADC1, **0 a 3,3 V** |
| Tiempo real | 1 kS/s a **2 MS/s**, disparado por TIM6 (sin jitter), 2048 muestras por captura |
| Resolución | 12 bits a 2 MS/s; **sobremuestreo automático hasta 16 bits** en las bases lentas |
| Tiempo equivalente (ETS) | señales periódicas de hasta ~900 kHz: 256 puntos por período, 0,4 LSB de error de forma |
| Trigger | por software, flanco ± y nivel ajustable; modos AUTO, NORMAL y ETS |
| Mediciones | frecuencia (error < 1 ppm), Vmín, Vmáx, Vpp, Vmedio, Vrms |
| Salida | USART2 a 2 Mbaud por DMA → visor en la PC (`tools/osciloscopio.py`) |

| | |
|---|---|
| CPU | DDS: ~5 % a 1 MS/s (~10 % interpolando), medido con el DWT |
| Memoria | 28,4 KB de flash / 28,5 KB de RAM (de 128 KB / 32 KB) |

## ¿ADC en paralelo? El H503 tiene uno solo

Poner varios ADC en paralelo sobre la misma entrada, disparados con un desfase de
Ts/N (*time-interleaving*), multiplica por N la velocidad de muestreo. Pero el
**STM32H503 tiene un único ADC**. Para el modo intercalado haría falta, por
ejemplo, un STM32H563, que tiene dos.

Con un solo ADC se aprovecha el mismo principio de dos maneras:

- **Tiempo equivalente (modo ETS):** si la señal es periódica, un solo ADC va
  tomando cada muestra en una fase distinta de períodos sucesivos. Funciona así:
  - el firmware mide el período con precisión de ppm (regresión sobre todos los
    cruces) y ordena las 2048 muestras por fase en 256 casilleros;
  - si la relación Fs/f deja fases sin cubrir, cambia sola la frecuencia de
    muestreo (ARR 124…131) para repartirlas;
  - el resultado: un seno de 730 kHz muestreado a 2 MS/s (2,7 muestras por
    período) se ve como si se hubiera muestreado cada 5 ns.
- **Sobremuestreo:** en las bases lentas, el ADC promedia 2ⁿ conversiones por
  cada muestra y gana resolución en amplitud: 14 bits a 100 kS/s, 16 bits a
  5 kS/s.

## Uso

Mantener apretado el encoder **FORMA** cambia entre las dos páginas. El
generador sigue funcionando mientras se usa el osciloscopio: se puede medir la
salida del DDS uniendo PA4 con PA1.

**Página DDS**

| Control | Acción |
|---|---|
| Girar **FRECUENCIA** | ± paso |
| Pulsar **FRECUENCIA** | cambia el paso: 0,1 → 1 → 10 → 100 Hz → 1 k → 10 k → 100 kHz → 0,1 Hz… |
| Girar **FORMA** | forma de onda siguiente / anterior |
| Pulsar **FORMA** | modo del canal 2: 90° → invertido → sync → igual |

**Página osciloscopio** (empieza a adquirir al entrar)

| Control | Acción |
|---|---|
| Girar **FRECUENCIA** | frecuencia de muestreo (horario = más rápido): 2 MS/s … 1 kS/s |
| Pulsar **FRECUENCIA** | modo AUTO → NORMAL → ETS |
| Mantener **FRECUENCIA** | RUN / STOP |
| Girar **FORMA** | nivel de trigger (pasos de 33 mV) |
| Pulsar **FORMA** | flanco ascendente / descendente |

### Visor en la PC

Hasta que esté la pantalla, las tramas y el estado salen por **USART2 (PA2,
2 Mbaud 8N1)**. Se necesita un adaptador que soporte 2 Mbaud: CP2102N, FT232R,
CH340 o el puerto virtual del ST-LINK V3. Para uno más lento, bajá
`DDS_DEBUG_BAUD` a 921600 en `dds_config.h`.

```bash
pip install pyserial matplotlib
python3 tools/osciloscopio.py --puerto COM5            # Windows
python3 tools/osciloscopio.py --puerto /dev/ttyACM0    # Linux
python3 tools/osciloscopio.py --puerto COM5 --csv captura.csv   # además guarda la última trama
```

El visor dibuja la señal con la frecuencia, las tensiones, el modo y la
resolución. Las líneas de estado del micro aparecen en la consola:

```
[micro] DDS | SENO | f=1000.000 Hz (real 1000.000 Hz) | paso 100.000 Hz | CH2 CUADRATURA 90 | interp OFF | CPU 4.8%
[micro] OSC | ETS | Fs 2000000 S/s | 13.5 bits | trigger + 1.650 V | RUN
```

## Conexionado (LQFP48)

| Pin | Función | Conexión |
|---|---|---|
| PA4 | DAC1_OUT1 | salida CH1 → filtro reconstructor |
| PA5 | DAC1_OUT2 | salida CH2 → filtro reconstructor |
| **PA1** | ADC1_INP1 | **entrada del osciloscopio, 0 a 3,3 V** |
| PA8 / PA9 | TIM1_CH1 / CH2 (AF1) | encoder FRECUENCIA A / B |
| PA10 | entrada con pull-up | pulsador FRECUENCIA |
| PA6 / PA7 | TIM3_CH1 / CH2 (AF2) | encoder FORMA A / B |
| PB0 | entrada con pull-up | pulsador FORMA |
| PA2 | USART2_TX (AF7) | estado + tramas del osciloscopio, 2 Mbaud |
| PB6 / PB7 | I2C1 | *reservado para la pantalla I²C* |
| PB13 / PB15 / PB12 / PB14 / PB10 | SPI2 + GPIO | *reservado para la pantalla SPI* |

- **Encoders:** van con el común a GND. Se recomienda una red RC de 10 kΩ / 10 nF en A y B.
- **Entrada del osciloscopio:**
  - **Protección:** 1 kΩ en serie y diodos Schottky a 3,3 V y a GND (BAT54S).
  - **Buffer:** a 2 MS/s el ADC muestrea durante solo 80 ns, así que conviene un
    operacional rail-to-rail rápido como seguidor (OPA350 o similar).
  - **Señales bipolares:** divisor resistivo más un offset de 1,65 V.

## Compilar y grabar

### Opción A: Makefile (arm-none-eabi-gcc)

```bash
make cmsis      # baja los headers CMSIS oficiales de ST (una sola vez)
make            # -> build/dds.elf, .hex, .bin
make flash      # graba con STM32_Programmer_CLI por SWD
make test       # prueba los núcleos DDS y osciloscopio y el visor en la PC (no necesita placa)
```

### Opción B: STM32CubeIDE

1. *File → New → Makefile Project with Existing Code*, apuntando a esta carpeta y con el toolchain *MCU ARM GCC*.
2. Correr `make cmsis` una vez (o copiar `cmsis-device-h5` y `cmsis-core` dentro de `cmsis/`).
3. Para depurar: *Debug Configurations → STM32 C/C++ Application*, eligiendo `build/dds.elf`.

Si preferís un proyecto STM32Cube generado por CubeMX, copiá `src/*.c` e `inc/*.h`.
Después excluí el `main.c` generado y borrá el `SysTick_Handler` de
`stm32h5xx_it.c`, porque `board.c` ya lo define. No configures los timers, el
DAC, el ADC ni el GPDMA en el `.ioc`: el código los configura por registros.

## Estructura

```
inc/dds_config.h     <- TODO lo configurable: Fs, tabla, interpolación, pines, ADC, baudios
src/dds_core.c       núcleo DDS en C puro: tablas, palabra de sintonía, lazos de síntesis
src/dds_hw.c         TIM2 + DAC1 dual + GPDMA1 canal 0 circular (lista enlazada) + ISR
src/osc_core.c       núcleo del osciloscopio en C puro: base de tiempo, trigger, mediciones, ETS
src/osc_hw.c         TIM6 + ADC1 (sobremuestreo) + GPDMA1 canal 1
src/osc.c            máquina de estados del osciloscopio (capturar -> procesar -> enviar)
src/encoder.c        TIM1/TIM3 en modo encoder x4 + pulsadores (corta y larga)
src/ui.c             páginas DDS / osciloscopio + ganchos de la pantalla (weak)
src/board.c          reloj de 250 MHz (HSI + PLL1, VOS0), SysTick, GPIO, UART con DMA (canal 2)
tools/gen_tabla.py   graba formas de onda arbitrarias (fórmula o CSV) -> inc/wave_arb.h
tools/osciloscopio.py visor del osciloscopio en la PC
test/test_host.c     pruebas del núcleo DDS en la PC
test/test_osc.c      pruebas del núcleo del osciloscopio (ADC simulado con ruido)
doc/dds_diseno.tex   documento de diseño con diagramas de flujo (TikZ)
```

## Decisiones principales (detalle en el PDF)

- **TIM2 y no TIM1:** en el H503 los dos pueden disparar el DAC, a 250 MHz.
  TIM1 es el único timer avanzado y se aprovecha como encoder. Para usar TIM6,
  poné `#define DDS_TIMER DDS_TIMER_TIM6` y TIM2 queda libre (el osciloscopio
  pasa solo a TIM7).
- **Tabla de 4096 × 12 bits:** el SFDR por truncamiento de fase (≈ 72 dBc)
  coincide con el techo de un DAC de 12 bits. La sierra queda exacta (1 LSB por
  muestra) y con doble buffer entra en la RAM.
- **Interpolación lineal en baja frecuencia:** por debajo de 244 Hz elimina la
  escalera. Error medido de 0,99 LSB contra 2,96 LSB sin interpolar. No se
  aplica a la cuadrada ni a la sierra, para no redondear los flancos.
- **Máximo rendimiento:** el CPU nunca escribe el DAC ni lee el ADC. Los
  timers fijan los instantes y el DMA mueve los datos. El lazo de síntesis viene
  especializado en 8 variantes sin `if` internos y se ejecuta desde la ICACHE.
- **ADC a 31,25 MHz (HCLK/8):** 2 MS/s reales con margen. Para los 2,5 MS/s del
  datasheet hacen falta 37,5 MHz, por ejemplo con el PLL2.

## Para llevar la Fs del DDS más arriba

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
void display_update(const ui_state_t *s) { /* página, frecuencia, paso (cursor), forma, CH2, config del osciloscopio */ }
void display_scope(const osc_frame_hdr_t *h, const uint16_t *pts)
{ /* h->n puntos de 16 bits (0..65535 = 0..3,3 V), separados h->dt_ps; mediciones en h */ }
```

y agregalo a `SRCS` en el Makefile. Como están definidas `weak`, el linker usa
las tuyas automáticamente.

## Estado de verificación

- ✅ Compila sin warnings con arm-none-eabi-gcc 13.2 contra los headers CMSIS oficiales de ST.
- ✅ `make test`, todo en PC:
  - **Núcleo DDS:** palabra de sintonía, error de amplitud, interpolación, modos de CH2 y cambio de forma.
  - **Osciloscopio con ADC simulado:** base de tiempo y sobremuestreo, trigger, medición de frecuencia y tiempo equivalente de 50 a 850 kHz.
  - **Visor:** decodifica tramas fragmentadas y con basura en el medio.
- ⚠️ **No está probado sobre una placa real.** Los registros (RCC, PWR, FLASH,
  GPDMA, DAC, ADC, TIM, USART), los números de AF y pines, las solicitudes DMA y
  los disparos del ADC/DAC salen de los headers CMSIS, del HAL y de la base de
  pines oficiales de ST para el STM32H503CBTx. Aun así, conviene verificar la
  primera puesta en marcha con un osciloscopio de referencia.
