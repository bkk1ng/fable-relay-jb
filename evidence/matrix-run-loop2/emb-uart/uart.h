/*
 * uart.h — interrupt-driven UART driver for ARM Cortex-M4
 * Register-level, no HAL, no CMSIS.
 *
 * Assumes a generic UART peripheral at a base address defined in uart.c.
 * Adjust UART_BASE, PCLK_HZ, and the IRQ name for your specific MCU.
 */

#ifndef UART_H
#define UART_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize UART with 8N1, no hardware flow control.
 * baud   — desired baud rate (e.g. 115200)
 *
 * Configures the baud-rate divisor, enables the TX/RX FIFOs,
 * enables the RX and TX interrupts, and unmasks the UART IRQ in
 * the NVIC.  Call once before any uart_read / uart_write.
 */
void uart_init(uint32_t baud);

/*
 * Queue a byte for transmission.
 *
 * Returns  0 on success.
 * Returns -1 if the transmit ring buffer is full (non-blocking).
 *
 * If the UART TX interrupt is not already pending, the byte is also
 * kicked into the hardware FIFO immediately so the ISR fires.
 */
int uart_write(uint8_t byte);

/*
 * Read one byte from the receive ring buffer.
 *
 * Returns the byte value (0–255) on success.
 * Returns -1 if the receive buffer is empty (non-blocking).
 */
int uart_read(void);

/*
 * UART interrupt service routine.
 * Declared here so the vector table translation unit can reference it.
 * The weak alias / vector table wiring is done in uart.c.
 */
void UART_IRQHandler(void);

#ifdef __cplusplus
}
#endif

#endif /* UART_H */
