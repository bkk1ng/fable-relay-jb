/**
 * uart.h — interrupt-driven UART driver for ARM Cortex-M4 (STM32F4xx)
 *
 * Register-level, no HAL, no CMSIS.
 *
 * Before calling uart_init() the caller is responsible for:
 *   1. Enabling the USART1 peripheral clock
 *        RCC->APB2ENR |= (1U << 4);  // RCC_APB2ENR_USART1EN
 *   2. Configuring PA9 (TX) and PA10 (RX) for AF7
 *        GPIOA->MODER  set bits for alternate function
 *        GPIOA->AFR[1] set AF7 on pins 9 and 10
 *   3. Enabling the USART1 interrupt in the NVIC
 *        NVIC_EnableIRQ(USART1_IRQn);  // vector 37
 */

#ifndef UART_H
#define UART_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  Tune for your target                                                */
/* ------------------------------------------------------------------ */

/** APB2 peripheral clock feeding USART1/USART6 on STM32F407 @ 168 MHz
 *  system clock, with APB2 prescaler = 2.  Adjust to match your RCC
 *  configuration.                                                      */
#define UART_PCLK        84000000UL

/** Ring buffer sizes — each must be a power of two.                   */
#define UART_TX_BUF_SIZE 256U
#define UART_RX_BUF_SIZE 256U

/* ------------------------------------------------------------------ */
/*  USART peripheral register map  (RM0090 rev 19, §30.6)              */
/* ------------------------------------------------------------------ */

typedef struct {
    volatile uint32_t SR;    /* 0x00  Status register                  */
    volatile uint32_t DR;    /* 0x04  Data register                    */
    volatile uint32_t BRR;   /* 0x08  Baud-rate register               */
    volatile uint32_t CR1;   /* 0x0C  Control register 1               */
    volatile uint32_t CR2;   /* 0x10  Control register 2               */
    volatile uint32_t CR3;   /* 0x14  Control register 3               */
    volatile uint32_t GTPR;  /* 0x18  Guard-time and prescaler         */
} USART_Regs;

/* Peripheral base addresses */
#define USART1_BASE  0x40011000UL
#define USART2_BASE  0x40004400UL
#define USART3_BASE  0x40004800UL

#define USART1  ((USART_Regs *)(USART1_BASE))
#define USART2  ((USART_Regs *)(USART2_BASE))
#define USART3  ((USART_Regs *)(USART3_BASE))

/* SR bit positions */
#define USART_SR_PE    (1U << 0)   /* Parity error                     */
#define USART_SR_FE    (1U << 1)   /* Framing error                    */
#define USART_SR_NF    (1U << 2)   /* Noise detected                   */
#define USART_SR_ORE   (1U << 3)   /* Overrun error                    */
#define USART_SR_IDLE  (1U << 4)   /* Idle line detected               */
#define USART_SR_RXNE  (1U << 5)   /* Read data register not empty     */
#define USART_SR_TC    (1U << 6)   /* Transmission complete            */
#define USART_SR_TXE   (1U << 7)   /* Transmit data register empty     */
#define USART_SR_LBD   (1U << 8)   /* LIN break detection              */
#define USART_SR_CTS   (1U << 9)   /* CTS flag                         */

/* CR1 bit positions */
#define USART_CR1_SBK    (1U << 0)
#define USART_CR1_RWU    (1U << 1)
#define USART_CR1_RE     (1U << 2)   /* Receiver enable                */
#define USART_CR1_TE     (1U << 3)   /* Transmitter enable             */
#define USART_CR1_IDLEIE (1U << 4)
#define USART_CR1_RXNEIE (1U << 5)   /* RXNE interrupt enable          */
#define USART_CR1_TCIE   (1U << 6)   /* TC interrupt enable            */
#define USART_CR1_TXEIE  (1U << 7)   /* TXE interrupt enable           */
#define USART_CR1_PEIE   (1U << 8)
#define USART_CR1_PS     (1U << 9)
#define USART_CR1_PCE    (1U << 10)
#define USART_CR1_WAKE   (1U << 11)
#define USART_CR1_M      (1U << 12)  /* Word length: 0=8 bit, 1=9 bit  */
#define USART_CR1_UE     (1U << 13)  /* USART enable                   */
#define USART_CR1_OVER8  (1U << 15)  /* Oversampling: 0=×16, 1=×8      */

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

/**
 * uart_init - configure USART1 for interrupt-driven 8-N-1 operation.
 *
 * @baud  desired baud rate (e.g. 115200)
 */
void uart_init(uint32_t baud);

/**
 * uart_write - queue one byte for transmission (non-blocking).
 *
 * Returns 0 on success, -1 if the TX ring buffer is full.
 */
int uart_write(uint8_t c);

/**
 * uart_read - consume one byte from the RX ring buffer (non-blocking).
 *
 * Stores the byte at *c and returns 0 on success.
 * Returns -1 (and leaves *c unchanged) if no data is available.
 */
int uart_read(uint8_t *c);

#endif /* UART_H */
