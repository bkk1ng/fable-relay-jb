/**
 * uart.h — interrupt-driven UART driver for ARM Cortex-M4
 *
 * No HAL, no CMSIS. Register-level only.
 * Assumes a 16 MHz system clock by default; adjust UART_PCLK_HZ as needed.
 *
 * API:
 *   uart_init(baud)        — configure and enable UART with computed divisor
 *   uart_write(buf, len)   — non-blocking write; returns bytes queued, -1 if full
 *   uart_read(buf, len)    — non-blocking read; returns bytes available
 *   uart_tx_busy()         — 1 if the TX ring buffer has pending data
 */

#ifndef UART_H
#define UART_H

#include <stdint.h>
#include <stddef.h>

/* ── clock configuration ──────────────────────────────────────────────────── */

/**
 * Peripheral clock feeding the UART.  Override before including this header
 * or at compile time:  -DUART_PCLK_HZ=48000000UL
 */
#ifndef UART_PCLK_HZ
#define UART_PCLK_HZ  16000000UL
#endif

/* ── ring-buffer size ─────────────────────────────────────────────────────── */

/** Must be a power of two so the index mask trick works. */
#define UART_TX_BUF_SIZE  256U
#define UART_RX_BUF_SIZE  256U

/* ── public API ───────────────────────────────────────────────────────────── */

/**
 * Initialise UART1 at the requested baud rate.
 * Computes BRR = PCLK / baud (integer mode).
 * Enables the RXNE and TXE interrupt sources; the NVIC must be configured
 * by the caller (priority, enable) before interrupts fire.
 *
 * @param baud  Desired baud rate, e.g. 115200.
 */
void uart_init(uint32_t baud);

/**
 * Non-blocking write.
 * Copies up to @len bytes into the TX ring buffer and kicks the TXE interrupt.
 *
 * @param buf  Pointer to data to send.
 * @param len  Number of bytes.
 * @return     Number of bytes actually queued, or -1 if the buffer is full
 *             and not a single byte could be accepted.
 */
int uart_write(const uint8_t *buf, size_t len);

/**
 * Non-blocking read.
 * Drains up to @len bytes from the RX ring buffer into @buf.
 *
 * @param buf  Destination buffer.
 * @param len  Maximum bytes to read.
 * @return     Number of bytes actually read (0 if the buffer is empty).
 */
int uart_read(uint8_t *buf, size_t len);

/**
 * Returns 1 if there is unsent data in the TX ring buffer, 0 otherwise.
 * Useful for a flush/drain loop before entering low-power mode.
 */
int uart_tx_busy(void);

/* ── ISR — declare in your vector table as USART1_IRQHandler ─────────────── */

/**
 * USART1 interrupt service routine.
 * Handles both RXNE (receive-data-register not empty) and
 * TXE  (transmit-data-register empty).
 *
 * Wire this to IRQ37 on STM32F4-class devices, or adjust to your SoC's
 * vector offset.
 */
void USART1_IRQHandler(void);

#endif /* UART_H */
