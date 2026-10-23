/**
 * @file    osc_hw.c
 * @brief   Adquisición del osciloscopio por hardware.
 *
 *  ┌──────┐ TRGO (Fs) ┌──────────────────────┐ DR (16 b) ┌──────────────┐
 *  │ TIM6 ├──────────►│ ADC1 canal 1 (PA1)   ├──────────►│ GPDMA1 ch1   ├──► buffer
 *  └──────┘           │ sobremuestreo x1-256 │  1 pedido │ bloque único │    2048 x 16 b
 *                     └──────────────────────┘ por dato  └──────┬───────┘
 *                                                               │ TC
 *                                                     ISR: para TIM6 y el ADC
 *
 * Igual que en el DDS, el CPU no toca ninguna muestra durante la captura:
 * el timer fija el instante exacto de cada conversión (sin jitter) y el
 * DMA guarda el resultado. Con sobremuestreo, un solo disparo hace 2^n
 * conversiones seguidas y el ADC entrega el promedio (más bits).
 */
#include "osc_hw.h"
#include "board.h"
#include "dds_config.h"

/* TIM6 dispara el ADC. Si el DDS usa TIM6, el osciloscopio pasa a TIM7. */
#if DDS_TIMER == DDS_TIMER_TIM6
  #define OSC_TIM          TIM7
  #define OSC_TIM_RCC_EN   RCC_APB1LENR_TIM7EN
  #define OSC_ADC_EXTSEL   17UL        /* adc_ext_trg17 = TIM7_TRGO */
#else
  #define OSC_TIM          TIM6
  #define OSC_TIM_RCC_EN   RCC_APB1LENR_TIM6EN
  #define OSC_ADC_EXTSEL   13UL        /* adc_ext_trg13 = TIM6_TRGO */
#endif

#define OSC_DMA_CH         GPDMA1_Channel1
#define GPDMA1_REQ_ADC1    0UL

/* Bits de ADC_CR que se ponen en 1 por software y los borra el hardware:
 * no hay que reescribirlos al modificar otros bits del registro.           */
#define ADC_CR_RS_BITS  (ADC_CR_ADCAL | ADC_CR_JADSTP | ADC_CR_ADSTP | \
                         ADC_CR_JADSTART | ADC_CR_ADSTART | ADC_CR_ADDIS | ADC_CR_ADEN)

volatile uint32_t osc_dma_errors;
static volatile bool s_done;

static inline void adc_cr_set(uint32_t bits)
{
    ADC1->CR = (ADC1->CR & ~ADC_CR_RS_BITS) | bits;
}

/* ------------------------------------------------------------------------ */
void osc_hw_init(void)
{
    RCC->AHB2ENR  |= RCC_AHB2ENR_ADCEN;
    RCC->AHB1ENR  |= RCC_AHB1ENR_GPDMA1EN;
    RCC->APB1LENR |= OSC_TIM_RCC_EN;
    (void)RCC->APB1LENR;

    gpio_set_analog(GPIOA, 1U);                       /* PA1 = ADC1_INP1 */

    /* Reloj del ADC: núcleo = HCLK (250 MHz), modo asíncrono con PRESC /8. */
    RCC->CCIPR5 &= ~RCC_CCIPR5_ADCDACSEL;
    ADC12_COMMON->CCR = (ADC12_COMMON->CCR & ~(ADC_CCR_CKMODE | ADC_CCR_PRESC))
                      | (OSC_ADC_PRESC_CODE << ADC_CCR_PRESC_Pos);

    /* Regulador interno del ADC: salir de deep power-down y esperar 20 us. */
    ADC1->CR = (ADC1->CR & ~(ADC_CR_RS_BITS | ADC_CR_DEEPPWD)) | ADC_CR_ADVREGEN;
    delay_us(20U);

    /* Calibración en modo simple (no diferencial). */
    ADC1->CR &= ~(ADC_CR_RS_BITS | ADC_CR_ADCALDIF);
    adc_cr_set(ADC_CR_ADCAL);
    while (ADC1->CR & ADC_CR_ADCAL) { }
    delay_us(1U);

    /* Habilitar y esperar ADRDY. */
    ADC1->ISR = ADC_ISR_ADRDY;
    adc_cr_set(ADC_CR_ADEN);
    while ((ADC1->ISR & ADC_ISR_ADRDY) == 0U) { }

    /* 12 bits, alineado a la derecha, disparo por flanco ascendente de
     * TRGO, DMA en modo circular (los pedidos no se cortan), y si se pisa un
     * dato se conserva el último (OVRMOD = 1).                              */
    ADC1->CFGR = (ADC1->CFGR & ADC_CFGR_JQDIS)
               | ADC_CFGR_DMAEN | ADC_CFGR_DMACFG | ADC_CFGR_OVRMOD
               | (OSC_ADC_EXTSEL << ADC_CFGR_EXTSEL_Pos)
               | (1UL << ADC_CFGR_EXTEN_Pos);
    ADC1->SQR1 = (0UL << ADC_SQR1_L_Pos) | (OSC_ADC_CHANNEL << ADC_SQR1_SQ1_Pos);

    OSC_TIM->CR1 = 0U;
    OSC_TIM->CR2 = (OSC_TIM->CR2 & ~TIM_CR2_MMS) | (2UL << TIM_CR2_MMS_Pos);   /* TRGO = update */

    NVIC_SetPriority(GPDMA1_Channel1_IRQn, 1U);       /* debajo del DDS */
    NVIC_EnableIRQ(GPDMA1_Channel1_IRQn);
}

/* ------------------------------------------------------------------------ */
void osc_hw_stop(void)
{
    OSC_TIM->CR1 = 0U;

    if (ADC1->CR & ADC_CR_ADSTART) {
        adc_cr_set(ADC_CR_ADSTP);
        while (ADC1->CR & ADC_CR_ADSTART) { }
    }
    if (OSC_DMA_CH->CCR & DMA_CCR_EN) {               /* abortar el canal */
        OSC_DMA_CH->CCR |= DMA_CCR_SUSP;
        while ((OSC_DMA_CH->CSR & DMA_CSR_SUSPF) == 0U) { }
        OSC_DMA_CH->CCR = DMA_CCR_RESET;
    }
}

void osc_hw_start(const osc_timebase_t *tb, uint16_t *buf, uint32_t n)
{
    osc_hw_stop();

    /* --- ADC: tiempo de muestreo y sobremuestreo de esta base de tiempo --- */
    ADC1->SMPR1 = (ADC1->SMPR1 & ~(7UL << ADC_SMPR1_SMP1_Pos))
                | ((uint32_t)tb->smp_code << ADC_SMPR1_SMP1_Pos);
    if (tb->osr_log2 > 0U) {
        /* TROVS = 0: un disparo -> 2^n conversiones seguidas -> 1 dato. */
        ADC1->CFGR2 = ADC_CFGR2_ROVSE
                    | ((uint32_t)(tb->osr_log2 - 1U) << ADC_CFGR2_OVSR_Pos)
                    | ((uint32_t)tb->ovss << ADC_CFGR2_OVSS_Pos);
    } else {
        ADC1->CFGR2 = 0U;
    }

    /* --- Timer: Fs = 250 MHz / ((PSC+1)(ARR+1)) --- */
    OSC_TIM->PSC = tb->psc;
    OSC_TIM->ARR = tb->arr;
    OSC_TIM->EGR = TIM_EGR_UG;        /* el ADC todavía no está arrancado */
    OSC_TIM->SR  = 0U;

    /* --- GPDMA: ADC1->DR (16 bits fijo) -> buffer (16 bits, incrementa) --- */
    DMA_Channel_TypeDef *ch = OSC_DMA_CH;
    ch->CFCR = DMA_CFCR_TCF | DMA_CFCR_HTF | DMA_CFCR_DTEF | DMA_CFCR_ULEF
             | DMA_CFCR_USEF | DMA_CFCR_SUSPF | DMA_CFCR_TOF;
    ch->CTR1 = (1UL << DMA_CTR1_SDW_LOG2_Pos) | (1UL << DMA_CTR1_DDW_LOG2_Pos)
             | DMA_CTR1_DINC;
    ch->CTR2 = (GPDMA1_REQ_ADC1 << DMA_CTR2_REQSEL_Pos);     /* el ADC es origen */
    ch->CBR1 = n * sizeof(uint16_t);
    ch->CSAR = (uint32_t)&ADC1->DR;
    ch->CDAR = (uint32_t)buf;
    ch->CLLR = 0U;
    ch->CCR  = (2UL << DMA_CCR_PRIO_Pos) | DMA_CCR_TCIE | DMA_CCR_DTEIE | DMA_CCR_USEIE;
    ch->CCR |= DMA_CCR_EN;

    /* --- Arranque: limpiar restos, armar el ADC y largar el timer --- */
    (void)ADC1->DR;
    ADC1->ISR = ADC_ISR_OVR | ADC_ISR_EOC | ADC_ISR_EOS;
    s_done = false;
    adc_cr_set(ADC_CR_ADSTART);       /* espera los TRGO */
    OSC_TIM->CR1 = TIM_CR1_CEN;
}

bool osc_hw_done(void)
{
    return s_done;
}

/* ------------------------------------------------------------------------ */
void GPDMA1_Channel1_IRQHandler(void)
{
    const uint32_t sr = OSC_DMA_CH->CSR;

    if (sr & (DMA_CSR_DTEF | DMA_CSR_ULEF | DMA_CSR_USEF)) {
        OSC_DMA_CH->CFCR = DMA_CFCR_DTEF | DMA_CFCR_ULEF | DMA_CFCR_USEF;
        osc_dma_errors++;
    }
    if (sr & (DMA_CSR_TCF | DMA_CSR_DTEF | DMA_CSR_ULEF | DMA_CSR_USEF)) {
        OSC_DMA_CH->CFCR = DMA_CFCR_TCF;
        OSC_TIM->CR1 = 0U;                 /* no más disparos             */
        adc_cr_set(ADC_CR_ADSTP);          /* el ADC se detiene solo      */
        s_done = true;
    }
}
