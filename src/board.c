/**
 * @file    board.c
 * @brief   Reloj a 250 MHz desde HSI + PLL1, SysTick de 1 ms, GPIO y UART.
 *
 * Se usa el HSI interno (64 MHz) para no depender del cristal de la placa.
 * Si tu placa tiene HSE, cambiá PLL1SRC a 3 y ajustá PLL1M para que la
 * entrada del PLL quede entre 4 y 8 MHz.
 */
#include "board.h"
#include "dds_config.h"

static volatile uint32_t s_ms;

/* ------------------------------------------------------------------------ */
static void clock_init_250mhz(void)
{
    /* 1) Regulador en escala VOS0 (obligatoria para > 200 MHz). */
    PWR->VOSCR = (PWR->VOSCR & ~PWR_VOSCR_VOS) | PWR_VOSCR_VOS;   /* VOS = 11 */
    while ((PWR->VOSSR & PWR_VOSSR_VOSRDY) == 0U) { }

    /* 2) Flash: 5 estados de espera y WRHIGHFREQ = 2 para 210..250 MHz. */
    FLASH->ACR = (FLASH->ACR & ~(FLASH_ACR_LATENCY | FLASH_ACR_WRHIGHFREQ))
               | (5U << FLASH_ACR_LATENCY_Pos) | FLASH_ACR_WRHIGHFREQ_1
               | FLASH_ACR_PRFTEN;
    while ((FLASH->ACR & FLASH_ACR_LATENCY) != (5U << FLASH_ACR_LATENCY_Pos)) { }

    /* 3) HSI a 64 MHz (HSIDIV = /1). */
    RCC->CR |= RCC_CR_HSION;
    while ((RCC->CR & RCC_CR_HSIRDY) == 0U) { }
    RCC->CR &= ~RCC_CR_HSIDIV;
    while ((RCC->CR & RCC_CR_HSIDIVF) == 0U) { }

    /* 4) PLL1: 64 MHz / 16 = 4 MHz (rango 4-8 MHz)
     *          4 MHz x 125   = 500 MHz (VCO rango ancho 192-836 MHz)
     *          500 MHz / 2   = 250 MHz (salida P -> SYSCLK)              */
    RCC->CR &= ~RCC_CR_PLL1ON;
    while ((RCC->CR & RCC_CR_PLL1RDY) != 0U) { }
    RCC->PLL1CFGR = (1UL  << RCC_PLL1CFGR_PLL1SRC_Pos)    /* fuente = HSI   */
                  | (2UL  << RCC_PLL1CFGR_PLL1RGE_Pos)    /* 4..8 MHz       */
                  | (16UL << RCC_PLL1CFGR_PLL1M_Pos)
                  | RCC_PLL1CFGR_PLL1PEN;
    RCC->PLL1DIVR = ((125UL - 1UL) << RCC_PLL1DIVR_PLL1N_Pos)
                  | ((2UL - 1UL)   << RCC_PLL1DIVR_PLL1P_Pos)
                  | ((2UL - 1UL)   << RCC_PLL1DIVR_PLL1Q_Pos)
                  | ((2UL - 1UL)   << RCC_PLL1DIVR_PLL1R_Pos);
    RCC->CR |= RCC_CR_PLL1ON;
    while ((RCC->CR & RCC_CR_PLL1RDY) == 0U) { }

    /* 5) AHB, APB1, APB2, APB3 sin división -> timers a 250 MHz. */
    RCC->CFGR2 &= ~(RCC_CFGR2_HPRE | RCC_CFGR2_PPRE1 | RCC_CFGR2_PPRE2 | RCC_CFGR2_PPRE3);

    /* 6) SYSCLK = PLL1P. */
    RCC->CFGR1 = (RCC->CFGR1 & ~RCC_CFGR1_SW) | (3UL << RCC_CFGR1_SW_Pos);
    while ((RCC->CFGR1 & RCC_CFGR1_SWS) != (3UL << RCC_CFGR1_SWS_Pos)) { }

    SystemCoreClock = SYSCLK_HZ;

    /* 7) Caché de instrucciones (8 KB): el lazo DDS corre sin esperas. */
    while ((ICACHE->SR & ICACHE_SR_BUSYF) != 0U) { }
    ICACHE->CR |= ICACHE_CR_EN;
}

/* ------------------------------------------------------------------------ */
void gpio_set_af(GPIO_TypeDef *port, uint32_t pin, uint32_t af, uint32_t pull)
{
    port->MODER   = (port->MODER & ~(3UL << (2U * pin))) | (2UL << (2U * pin));
    port->PUPDR   = (port->PUPDR & ~(3UL << (2U * pin))) | (pull << (2U * pin));
    port->OSPEEDR = (port->OSPEEDR & ~(3UL << (2U * pin))) | (1UL << (2U * pin));
    port->AFR[pin >> 3] = (port->AFR[pin >> 3] & ~(0xFUL << (4U * (pin & 7U))))
                        | (af << (4U * (pin & 7U)));
}

void gpio_set_input(GPIO_TypeDef *port, uint32_t pin, uint32_t pull)
{
    port->MODER = port->MODER & ~(3UL << (2U * pin));
    port->PUPDR = (port->PUPDR & ~(3UL << (2U * pin))) | (pull << (2U * pin));
}

void gpio_set_analog(GPIO_TypeDef *port, uint32_t pin)
{
    port->PUPDR &= ~(3UL << (2U * pin));
    port->MODER |=  (3UL << (2U * pin));
}

/* ------------------------------------------------------------------------ */
static void uart_init(void)
{
#if DDS_DEBUG_UART
    RCC->AHB2ENR  |= RCC_AHB2ENR_GPIOAEN;
    RCC->APB1LENR |= RCC_APB1LENR_USART2EN;
    (void)RCC->APB1LENR;
    gpio_set_af(GPIOA, 2U, 7U, 0U);                       /* PA2 = USART2_TX */
    /* Reloj de núcleo por defecto = PCLK1 = 250 MHz -> 2 Mbaud exactos. */
    USART2->BRR = (SYSCLK_HZ + DDS_DEBUG_BAUD / 2U) / DDS_DEBUG_BAUD;
    USART2->CR3 = USART_CR3_DMAT;                         /* TX por DMA      */
    USART2->CR1 = USART_CR1_TE | USART_CR1_UE;
    RCC->AHB1ENR |= RCC_AHB1ENR_GPDMA1EN;
    (void)RCC->AHB1ENR;
#endif
}

/* ---- Transmisión por GPDMA1 canal 2 (tramas del osciloscopio) ---- */
#define UART_DMA_CH          GPDMA1_Channel2
#define GPDMA1_REQ_USART2_TX 24UL

bool uart_tx_busy(void)
{
#if DDS_DEBUG_UART
    return (UART_DMA_CH->CSR & DMA_CSR_IDLEF) == 0U;
#else
    return false;
#endif
}

void uart_write_blocking(const void *data, uint32_t len)
{
#if DDS_DEBUG_UART
    const uint8_t *p = (const uint8_t *)data;
    while (uart_tx_busy()) { }
    while (len--) {
        while ((USART2->ISR & USART_ISR_TXE_TXFNF) == 0U) { }
        USART2->TDR = *p++;
    }
#else
    (void)data; (void)len;
#endif
}

void uart_send_dma(const void *data, uint32_t len)
{
#if DDS_DEBUG_UART
    while (uart_tx_busy()) { }
    DMA_Channel_TypeDef *ch = UART_DMA_CH;
    ch->CFCR = DMA_CFCR_TCF | DMA_CFCR_HTF | DMA_CFCR_DTEF | DMA_CFCR_ULEF
             | DMA_CFCR_USEF | DMA_CFCR_SUSPF | DMA_CFCR_TOF;
    ch->CTR1 = DMA_CTR1_SINC;                        /* bytes, memoria -> TDR */
    ch->CTR2 = (GPDMA1_REQ_USART2_TX << DMA_CTR2_REQSEL_Pos) | DMA_CTR2_DREQ;
    ch->CBR1 = len;
    ch->CSAR = (uint32_t)data;
    ch->CDAR = (uint32_t)&USART2->TDR;
    ch->CLLR = 0U;
    ch->CCR  = DMA_CCR_EN;                           /* prioridad baja */
#else
    (void)data; (void)len;
#endif
}

void dbg_puts(const char *s)
{
    uint32_t n = 0U;
    while (s[n]) n++;
    uart_write_blocking(s, n);
}

void delay_us(uint32_t us)
{
    const uint32_t t0 = DWT->CYCCNT;
    const uint32_t cycles = us * (SYSCLK_HZ / 1000000U);
    while ((DWT->CYCCNT - t0) < cycles) { }
}

/* ------------------------------------------------------------------------ */
void board_init(void)
{
    clock_init_250mhz();

    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOBEN;
    (void)RCC->AHB2ENR;

    /* Contador de ciclos (DWT): mide la carga de la ISR y sirve de retardo. */
    DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;

    uart_init();

    /* SysTick a 1 kHz, prioridad baja (la ISR del DDS tiene prioridad 0). */
    SysTick_Config(SYSCLK_HZ / 1000U);
    NVIC_SetPriority(SysTick_IRQn, 8U);
}

uint32_t board_millis(void)
{
    return s_ms;
}

void SysTick_Handler(void)
{
    s_ms++;
    board_tick_1ms_hook();
}
