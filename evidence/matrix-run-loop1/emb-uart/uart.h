/**
 * uart.h — Interrupt-driven UART driver for ARM Cortex-M4
 *
 * Register-level, no HAL, no CMSIS. Assumes:
 *   - UART0 base at 0x4000C000 (adjust UART_BASE for your SoC)
 *   - APB/PCLK frequency defined by PCLK_HZ
 *   - Caller has enabled the peripheral clock and configured GPIO AF before uart_init()
 *   - IRQn and NVIC setup done by caller (or add uart_irq_enable() if preferred)
 */

#ifndef UART_H
#define UART_H

#include <stdint.h>
#include <stddef.h>

/* ── Peripheral base ──────────────────────────────────────────────────────── */
#define UART_BASE       0x4000C000UL

/* ── Clock used to compute baud divisor ──────────────────────────────────── */
#ifndef PCLK_HZ
#  define PCLK_HZ       16000000UL   /* 16 MHz — override in your build flags */
#endif

/* ── Transmit ring buffer size (must be a power of two) ──────────────────── */
#ifndef UART_TX_BUF_SIZE
#  define UART_TX_BUF_SIZE  256U
#endif

/* ── Public API ───────────────────────────────────────────────────────────── */

/**
 * uart_init - Initialise UART at the requested baud rate.
 *
 * Computes the integer + fractional baud divisor from PCLK_HZ, enables
 * the FIFO, and enables the TX-empty interrupt.  The peripheral must already
 * have its clock gate open and its GPIO pins muxed to the UART alternate
 * function before this is called.
 *
 * @baud: desired baud rate (e.g. 115200)
 */
void uart_init(uint32_t baud);

/**
 * uart_write - Queue one byte for transmission (non-blocking).
 *
 * Copies the byte into the TX ring buffer and arms the TX-empty interrupt.
 * Returns 0 on success, -1 if the buffer is full (caller must retry or drop).
 *
 * Safe to call from task context.  Do NOT call from an ISR with a priority
 * that could pre-empt the UART IRQ itself, or protect externally.
 *
 * @c: byte to transmit
 * @return: 0 on success, -1 if buffer full
 */
int uart_write(uint8_t c);

/**
 * uart_read - Read one received byte (polling, blocks until a byte arrives).
 *
 * For a fully non-blocking RX you would add an RX ring buffer fed by the ISR;
 * this implementation reads the data register directly, which is fine for
 * low-volume reception.
 *
 * @return: received byte (cast to int so -1 can signal "no byte" if you
 *          convert this to non-blocking later)
 */
int uart_read(void);

/**
 * uart_isr - Transmit/receive interrupt service routine.
 *
 * Wire this to the UART IRQ vector in your interrupt vector table, e.g.:
 *
 *   void UART0_IRQHandler(void) { uart_isr(); }
 */
void uart_isr(void);

#endif /* UART_H */
