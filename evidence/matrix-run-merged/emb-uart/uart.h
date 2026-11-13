/**
 * uart.h — interrupt-driven UART driver for ARM Cortex-M4 (STM32F4xx)
 *
 * Target: STM32F407 (or any STM32F4xx with USART1 at 0x40011000)
 * No HAL, no CMSIS — raw register access only.
 *
 * Caller responsibilities before uart_init():
 *   1. Enable USART1 APB2 clock:    *(uint32_t*)0x40023844 |= (1u << 4)
 *   2. Enable GPIOA AHB1 clock:     *(uint32_t*)0x40023830 |= (1u << 0)
 *   3. Set PA9 (TX) and PA10 (RX) to AF7, push-pull, no pull, high speed
 *      using the GPIOA register block at 0x40020000.
 *
 * Build with --std=c11 or later; tested with arm-none-eabi-gcc.
 */

#ifndef UART_H
#define UART_H

#include <stdint.h>

/* -------------------------------------------------------------------------
 * Ring-buffer capacity — must be a power of two, 2–32768.
 * ------------------------------------------------------------------------- */
#define UART_TX_BUF_SIZE  256u
#define UART_RX_BUF_SIZE  256u

/* -------------------------------------------------------------------------
 * USART register map  (RM0090 §30.6)
 * ------------------------------------------------------------------------- */
typedef struct {
    volatile uint32_t SR;    /*!< 0x00  Status register                  */
    volatile uint32_t DR;    /*!< 0x04  Data register                    */
    volatile uint32_t BRR;   /*!< 0x08  Baud-rate register               */
    volatile uint32_t CR1;   /*!< 0x0C  Control register 1               */
    volatile uint32_t CR2;   /*!< 0x10  Control register 2               */
    volatile uint32_t CR3;   /*!< 0x14  Control register 3               */
    volatile uint32_t GTPR;  /*!< 0x18  Guard-time & prescaler register  */
} USART_Regs;

/* Peripheral base addresses (RM0090 Table 1) */
#define USART1_BASE  0x40011000UL
#define USART2_BASE  0x40004400UL
#define USART3_BASE  0x40004800UL

#define USART1  ((USART_Regs *)(USART1_BASE))
#define USART2  ((USART_Regs *)(USART2_BASE))
#define USART3  ((USART_Regs *)(USART3_BASE))

/* -------------------------------------------------------------------------
 * SR bit-field positions
 * ------------------------------------------------------------------------- */
#define USART_SR_PE     (1u <<  0)  /* Parity error                */
#define USART_SR_FE     (1u <<  1)  /* Framing error               */
#define USART_SR_NF     (1u <<  2)  /* Noise detected              */
#define USART_SR_ORE    (1u <<  3)  /* Overrun error               */
#define USART_SR_IDLE   (1u <<  4)  /* IDLE line detected          */
#define USART_SR_RXNE   (1u <<  5)  /* RX register not empty       */
#define USART_SR_TC     (1u <<  6)  /* Transmission complete       */
#define USART_SR_TXE    (1u <<  7)  /* TX register empty           */

/* -------------------------------------------------------------------------
 * CR1 bit-field positions
 * ------------------------------------------------------------------------- */
#define USART_CR1_RE      (1u <<  2)  /* Receiver enable             */
#define USART_CR1_TE      (1u <<  3)  /* Transmitter enable          */
#define USART_CR1_IDLEIE  (1u <<  4)  /* IDLE interrupt enable       */
#define USART_CR1_RXNEIE  (1u <<  5)  /* RXNE interrupt enable       */
#define USART_CR1_TCIE    (1u <<  6)  /* TC interrupt enable         */
#define USART_CR1_TXEIE   (1u <<  7)  /* TXE interrupt enable        */
#define USART_CR1_PEIE    (1u <<  8)  /* PE interrupt enable         */
#define USART_CR1_PCE     (1u << 10)  /* Parity control enable       */
#define USART_CR1_M       (1u << 12)  /* Word length: 0=8-bit        */
#define USART_CR1_UE      (1u << 13)  /* USART enable                */
#define USART_CR1_OVER8   (1u << 15)  /* Oversampling: 0=16x, 1=8x  */

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/**
 * uart_init - initialise USART1 for 8-N-1 at the requested baud rate.
 *
 * Computes BRR from UART_PCLK_HZ (default 84 MHz; override via -D or in
 * uart.c) using scaled integer arithmetic — no floating point required.
 * Clears both ring buffers, enables RE+TE+RXNEIE, then enables the NVIC IRQ.
 * TXEIE is left disabled until the first uart_write() call.
 *
 * @param baud  Target baud rate, e.g. 115200.
 */
void uart_init(uint32_t baud);

/**
 * uart_write - enqueue one byte for transmission (non-blocking).
 *
 * Returns immediately.  Does NOT sleep or spin.
 *
 * @param byte  Byte to transmit.
 * @return  0 on success, -1 if the TX ring buffer is full.
 */
int uart_write(uint8_t byte);

/**
 * uart_read - dequeue one received byte (non-blocking).
 *
 * Returns immediately.  Does NOT sleep or spin.
 *
 * @return  Received byte in [0, 255], or -1 if the RX buffer is empty.
 */
int uart_read(void);

#endif /* UART_H */
