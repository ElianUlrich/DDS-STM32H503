/**
 * @file    dds_hw.c
 * @brief   TIM2 -> TRGO -> DAC1 (CH1 + CH2 simultáneos) <- GPDMA1 circular.
 *
 *  ┌──────┐ TRGO (Fs) ┌─────────────┐  DHR12RD (32 bits)  ┌──────────────┐
 *  │ TIM2 ├──────────►│ DAC1 CH1+CH2├◄────────────────────┤ GPDMA1 ch0   │
 *  └──────┘           └──┬───────┬──┘   1 transferencia   │ circular     │
 *                     PA4│    PA5│      por muestra       └──────┬───────┘
 *                                                                │ HT / TC
 *                                     ┌──────────────┐          ▼
 *                                     │ buffer RAM   │◄── ISR: dds_fill()
 *                                     │ 2 x 256 x 32b│    llena la mitad libre
 *                                     └──────────────┘
 *
 * Ambos canales usan el mismo trigger y se cargan con UNA sola escritura de
 * 32 bits en DAC_DHR12RD (modo dual): salen exactamente en el mismo instante
 * y el DMA hace la mitad de transferencias que con dos canales separados.
 */
#include "dds_hw.h"
#include "dds_core.h"
#include "dds_config.h"
#include "board.h"

/* ---- Selección del timer de muestreo ---- */
#if DDS_TIMER == DDS_TIMER_TIM2
  #define DDS_TIM          TIM2
  #define DDS_TIM_RCC_EN   RCC_APB1LENR_TIM2EN
  #define DDS_DAC_TSEL     2UL          /* dac_chx_trg2 = TIM2_TRGO */
#elif DDS_TIMER == DDS_TIMER_TIM6
  #define DDS_TIM          TIM6
  #define DDS_TIM_RCC_EN   RCC_APB1LENR_TIM6EN
  #define DDS_DAC_TSEL     5UL          /* dac_chx_trg5 = TIM6_TRGO */
#else
  #error "DDS_TIMER no soportado"
#endif

#define GPDMA1_REQ_DAC1_CH1  2UL        /* RM0492: solicitud GPDMA de DAC1_CH1 */
#define DMA_CH               GPDMA1_Channel0

/* Buffer ping-pong. Cada palabra: CH1 en [11:0], CH2 en [27:16]. */
static uint32_t s_buf[DDS_BUF_SAMPLES] __attribute__((aligned(32)));

/* Nodo de lista enlazada que recarga el canal al terminar cada bloque
 * (así se hace el modo circular en el GPDMA). Orden fijado por hardware
 * para los registros con bit de actualización: CBR1, CSAR, CLLR.          */
static uint32_t s_lli[3] __attribute__((aligned(4)));

volatile uint32_t dds_dma_errors;
static volatile uint32_t s_isr_cycles_max;

/* ------------------------------------------------------------------------ */
static void timer_init(void)
{
    RCC->APB1LENR |= DDS_TIM_RCC_EN;
    (void)RCC->APB1LENR;

    DDS_TIM->CR1 = 0U;
    DDS_TIM->PSC = 0U;                            /* 250 MHz sin prescaler */
    DDS_TIM->ARR = DDS_TIM_ARR;                   /* Fs = 250 MHz/(ARR+1)   */
    DDS_TIM->CR2 = (DDS_TIM->CR2 & ~TIM_CR2_MMS)
                 | (2UL << TIM_CR2_MMS_Pos);      /* TRGO = evento update  */
    DDS_TIM->EGR = TIM_EGR_UG;                    /* carga PSC/ARR         */
}

static void dac_init(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_DAC1EN;
    (void)RCC->AHB2ENR;

    gpio_set_analog(GPIOA, 4U);                   /* DAC1_OUT1 */
    gpio_set_analog(GPIOA, 5U);                   /* DAC1_OUT2 */

    DAC1->CR = 0U;
    /* HFSEL = 10: interfaz de alta frecuencia, OBLIGATORIO con HCLK > 160 MHz.
     * MODE1 = MODE2 = 000: modo normal, buffer de salida ON, al pin.
     * (Para > 1 MS/s usá 010 = buffer OFF y un op-amp externo.)            */
    DAC1->MCR = (2UL << DAC_MCR_HFSEL_Pos);

    DAC1->CR = (DDS_DAC_TSEL << DAC_CR_TSEL1_Pos) | DAC_CR_TEN1 | DAC_CR_DMAEN1
             | (DDS_DAC_TSEL << DAC_CR_TSEL2_Pos) | DAC_CR_TEN2;
    /* Solo CH1 pide DMA: cada transferencia escribe los dos canales. */

    DAC1->DHR12RD = ((uint32_t)(DDS_CODE_MAX / 2U) << 16) | (DDS_CODE_MAX / 2U);
    DAC1->CR |= DAC_CR_EN1 | DAC_CR_EN2;

    for (uint32_t t = 0; t < 100000U; t++) {      /* espera DACxRDY */
        if ((DAC1->SR & (DAC_SR_DAC1RDY | DAC_SR_DAC2RDY)) ==
            (DAC_SR_DAC1RDY | DAC_SR_DAC2RDY)) break;
    }
}

static void dma_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPDMA1EN;
    (void)RCC->AHB1ENR;

    DMA_CH->CCR  = 0U;
    DMA_CH->CFCR = DMA_CFCR_TCF | DMA_CFCR_HTF | DMA_CFCR_DTEF | DMA_CFCR_ULEF
                 | DMA_CFCR_USEF | DMA_CFCR_SUSPF | DMA_CFCR_TOF;

    /* Memoria (32 bits, incrementa) -> periférico (32 bits, fijo). */
    DMA_CH->CTR1 = (2UL << DMA_CTR1_SDW_LOG2_Pos) | DMA_CTR1_SINC
                 | (2UL << DMA_CTR1_DDW_LOG2_Pos);
    /* Solicitud por hardware del DAC1_CH1, el periférico es el destino.
     * TCEM = 00: evento TC al final de cada bloque.                        */
    DMA_CH->CTR2 = (GPDMA1_REQ_DAC1_CH1 << DMA_CTR2_REQSEL_Pos) | DMA_CTR2_DREQ;

    const uint32_t bytes = DDS_BUF_SAMPLES * sizeof(uint32_t);
    const uint32_t llr   = ((uint32_t)s_lli & DMA_CLLR_LA)
                         | DMA_CLLR_UB1 | DMA_CLLR_USA | DMA_CLLR_ULL;

    s_lli[0] = bytes;                 /* CBR1: BNDT                      */
    s_lli[1] = (uint32_t)s_buf;       /* CSAR: vuelve al inicio          */
    s_lli[2] = llr;                   /* CLLR: se apunta a sí mismo      */

    DMA_CH->CBR1  = bytes;
    DMA_CH->CSAR  = (uint32_t)s_buf;
    DMA_CH->CDAR  = (uint32_t)&DAC1->DHR12RD;
    DMA_CH->CLBAR = (uint32_t)s_lli & 0xFFFF0000UL;
    DMA_CH->CLLR  = llr;

    DMA_CH->CCR = (3UL << DMA_CCR_PRIO_Pos)               /* prioridad alta */
                | DMA_CCR_HTIE | DMA_CCR_TCIE
                | DMA_CCR_DTEIE | DMA_CCR_ULEIE | DMA_CCR_USEIE;

    NVIC_SetPriority(GPDMA1_Channel0_IRQn, 0U);           /* la más alta    */
    NVIC_EnableIRQ(GPDMA1_Channel0_IRQn);
}

/* ------------------------------------------------------------------------ */
void dds_hw_start(void)
{
    /* Buffer completo pre-calculado antes de arrancar. */
    dds_fill(&s_buf[0], DDS_BUF_SAMPLES);

    timer_init();
    dac_init();
    dma_init();

    DMA_CH->CCR  |= DMA_CCR_EN;
    DDS_TIM->CR1 |= TIM_CR1_CEN;          /* ¡arranca la síntesis! */
}

/* ------------------------------------------------------------------------ */
/*  ISR: el DMA terminó una mitad -> recalcular esa mitad                   */
/* ------------------------------------------------------------------------ */
void GPDMA1_Channel0_IRQHandler(void)
{
    const uint32_t t0 = DWT->CYCCNT;
    const uint32_t sr = DMA_CH->CSR;

    if (sr & DMA_CSR_HTF) {               /* el DMA ya lee la 2.ª mitad */
        DMA_CH->CFCR = DMA_CFCR_HTF;
        dds_fill(&s_buf[0], DDS_HALF_SAMPLES);
    }
    if (sr & DMA_CSR_TCF) {               /* el DMA volvió a la 1.ª mitad */
        DMA_CH->CFCR = DMA_CFCR_TCF;
        dds_fill(&s_buf[DDS_HALF_SAMPLES], DDS_HALF_SAMPLES);
    }
    if (sr & (DMA_CSR_DTEF | DMA_CSR_ULEF | DMA_CSR_USEF)) {
        DMA_CH->CFCR = DMA_CFCR_DTEF | DMA_CFCR_ULEF | DMA_CFCR_USEF;
        dds_dma_errors++;
    }

    const uint32_t dt = DWT->CYCCNT - t0;
    if (dt > s_isr_cycles_max) s_isr_cycles_max = dt;
}

uint32_t dds_hw_isr_load_permille(void)
{
    /* ciclos disponibles por media-transferencia = HALF · SYSCLK / Fs */
    const uint32_t budget = DDS_HALF_SAMPLES * (SYSCLK_HZ / DDS_FS_HZ);
    return (s_isr_cycles_max * 1000U) / budget;
}

bool dds_hw_underrun(void)
{
    return (DAC1->SR & DAC_SR_DMAUDR1) != 0U;
}
