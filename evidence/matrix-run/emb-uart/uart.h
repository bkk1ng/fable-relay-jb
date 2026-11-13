/*
 * uart.h — interrupt-driven UART driver for ARM Cortex-M4
 * Register-level only: no HAL, no CMSIS.
 *
 * Assumes a single UART peripheral mapped at UART0_BASE.
 * Adjust UART0_BASE and UART_IRQn for your specific SoC.
 */

#ifndef UART_H
#define UART_H

#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

/*
 * uart_init — configure baud rate, 8N1, enable TX/RX and their IRQs.
 *
 * pclk_hz  : peripheral clock feeding the UART (e.g. 48000000)
 * baud     : desired baud rate (e.g. 115200)
 *
 * The divisor is computed at runtime:
 *   integer  part = pclk_hz / (16 * baud)
 *   fraction part = round( (pclk_hz / (16.0 * baud) - integer) * 64 )
 */
void uart_init(uint32_t pclk_hz, uint32_t baud);

/*
 * uart_write — copy up to `len` bytes into the TX ring buffer.
 *
 * Returns the number of bytes actually queued, which may be less than
 * `len` if the buffer is partially full.
 * Returns -1 if the buffer is completely full (no bytes were written).
 *
 * Non-blocking.  Kicks off transmission if the UART TX FIFO is idle.
 */
int uart_write(const uint8_t *buf, size_t len);

/*
 * uart_read — copy up to `len` bytes out of the RX ring buffer.
 *
 * Returns the number of bytes copied (0 if the buffer is empty).
 * Non-blocking.
 */
int uart_read(uint8_t *buf, size_t len);

#endif /* UART_H */
