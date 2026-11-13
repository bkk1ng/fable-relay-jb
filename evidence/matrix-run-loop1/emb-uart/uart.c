/**
 * uart.c — Interrupt-driven UART driver for ARM Cortex-M4
 *
 * Register-level, no HAL, no CMSIS.
 *
 * Assumed register map (PL011-compatible, common on STM32/LPC/etc — remap
 * the struct fields and offsets for your exact SoC):
 *
 *   Offset  Register
 *   0x000   DR        Data register (read = RX, write = TX)
 *   0x004   RSR/ECR   Receive status / error clear
 *   0x018   FR        Flag register
 *   0x020   ILPR      IrDA low-power counter (unused here)
 *   0x024   IBRD      Integer baud-rate divisor
 *   0x028   FBRD      Fractional baud-rate divisor
 *   0x02C   LCR_H     Line control
 *   0x030   CR        Control register
 *   0x034   IFLS      Interrupt FIFO level select
 *   0x038   IMSC      Interrupt mask set/clear
 *   0x03C   RIS       Raw interrupt status
 *   0x040   MIS       Masked interrupt status
 *   0x044   ICR       Interrupt clear register
 *
 * Adapt field names / offsets for your SoC if its UART is not PL011.
 */

#include "uart.h"

/* ── Flag register (FR) bits ─────────────────────────────────────────────── */
#define FR_TXFF     (1U << 5)   /* TX FIFO full  */
#define FR_RXFE     (1U << 4)   /* RX FIFO empty */
#define FR_BUSY     (1U << 3)   /* UART busy     */
#define FR_TXFE     (1U << 7)   /* TX FIFO empty */

/* ── Line control (LCR_H) bits ───────────────────────────────────────────── */
#define LCR_H_WLEN8 (3U << 5)   /* 8-bit word length */
#define LCR_H_FEN   (1U << 4)   /* Enable FIFOs      */

/* ── Control register (CR) bits ─────────────────────────────────────────── */
#define CR_RXE      (1U << 9)   /* Receive enable    */
#define CR_TXE      (1U << 8)   /* Transmit enable   */
#define CR_UARTEN   (1U << 0)   /* UART enable       */

/* ── Interrupt mask (IMSC) / clear (ICR) bits ────────────────────────────── */
#define INT_TX      (1U << 5)   /* TX interrupt      */
#define INT_RX      (1U << 4)   /* RX interrupt      */

/* ── Raw register layout at UART_BASE ───────────────────────────────────── */
typedef struct {
    volatile uint32_t DR;       /* 0x000 */
    volatile uint32_t RSR;      /* 0x004 */
    volatile uint32_t _pad0[4]; /* 0x008–0x014 */
    volatile uint32_t FR;       /* 0x018 */
    volatile uint32_t _pad1;    /* 0x01C */
    volatile uint32_t ILPR;     /* 0x020 */
    volatile uint32_t IBRD;     /* 0x024 */
    volatile uint32_t FBRD;     /* 0x028 */
    volatile uint32_t LCR_H;    /* 0x02C */
    volatile uint32_t CR;       /* 0x030 */
    volatile uint32_t IFLS;     /* 0x034 */
    volatile uint32_t IMSC;     /* 0x038 */
    volatile uint32_t RIS;      /* 0x03C */
    volatile uint32_t MIS;      /* 0x040 */
    volatile uint32_t ICR;      /* 0x044 */
} uart_regs_t;

/* Single static instance at the fixed peripheral address. */
static uart_regs_t * const UART = (uart_regs_t *)UART_BASE;

/* ── Transmit ring buffer ─────────────────────────────────────────────────── */
#define TX_BUF_MASK  (UART_TX_BUF_SIZE - 1U)

/* Verify power-of-two at compile time. */
typedef char _uart_buf_size_check[(UART_TX_BUF_SIZE & TX_BUF_MASK) == 0 ? 1 : -1];

static struct {
    uint8_t  buf[UART_TX_BUF_SIZE];
    uint32_t head;  /* next write index (producer side) */
    uint32_t tail;  /* next read index  (consumer/ISR)  */
} tx_rb;

/* ── uart_init ────────────────────────────────────────────────────────────── */
void uart_init(uint32_t baud)
{
    /*
     * Baud divisor formula (PL011):
     *
     *   divisor = PCLK / (16 * baud)   (integer part)
     *   fraction = ((PCLK % (16 * baud)) * 64 + (16 * baud) / 2) / (16 * baud)
     *
     * Integer arithmetic only; +0.5 rounding via the half-divisor addition.
     */
    uint32_t denom   = 16U * baud;
    uint32_t ibrd    = PCLK_HZ / denom;
    uint32_t fbrd    = (uint32_t)(((uint64_t)(PCLK_HZ % denom) * 64U + denom / 2U) / denom);

    /* 1. Disable the UART before changing any config registers. */
    UART->CR = 0U;

    /* 2. Wait for any in-progress transmission to finish. */
    while (UART->FR & FR_BUSY) { /* spin */ }

    /* 3. Flush the FIFOs by clearing FEN, then write divisors. */
    UART->LCR_H = 0U;
    UART->IBRD  = ibrd;
    UART->FBRD  = fbrd;

    /* 4. 8-N-1, FIFOs enabled. */
    UART->LCR_H = LCR_H_WLEN8 | LCR_H_FEN;

    /* 5. Clear any stale interrupts, then mask: RX + TX. */
    UART->ICR  = 0x7FFU;
    UART->IMSC = INT_RX | INT_TX;

    /* 6. Zero the ring buffer state. */
    tx_rb.head = 0U;
    tx_rb.tail = 0U;

    /* 7. Enable TX, RX, and the UART. */
    UART->CR = CR_UARTEN | CR_TXE | CR_RXE;
}

/* ── uart_write ───────────────────────────────────────────────────────────── */
int uart_write(uint8_t c)
{
    uint32_t next_head = (tx_rb.head + 1U) & TX_BUF_MASK;

    if (next_head == tx_rb.tail) {
        /* Buffer full — caller must retry or drop. */
        return -1;
    }

    tx_rb.buf[tx_rb.head] = c;

    /*
     * Barrier: the byte must land in buf[] before head advances.
     * On Cortex-M4 a compiler barrier is enough because the CPU is
     * in-order; a full DMB is included for safety on strongly-ordered
     * peripheral bus paths.
     */
    __asm volatile ("dmb" ::: "memory");

    tx_rb.head = next_head;

    /*
     * Arm the TX-empty interrupt so the ISR will drain the buffer.
     * If the UART is already in the middle of transmitting this is a
     * no-op because the interrupt fires again once the FIFO drains.
     */
    UART->IMSC |= INT_TX;

    return 0;
}

/* ── uart_read ────────────────────────────────────────────────────────────── */
int uart_read(void)
{
    /* Block until at least one byte is in the RX FIFO. */
    while (UART->FR & FR_RXFE) { /* spin */ }

    /*
     * DR[7:0] is the data byte.  DR[11:8] carries framing/parity/break
     * error flags; mask them out so we return only the data.
     */
    return (int)(UART->DR & 0xFFU);
}

/* ── uart_isr ─────────────────────────────────────────────────────────────── */
/*
 * Wire to the UART IRQ vector in your vector table, e.g.:
 *
 *   void UART0_IRQHandler(void) { uart_isr(); }
 *
 * The ISR handles two events:
 *
 *   TX: the TX FIFO has drained to the trigger level.  Fill it from the
 *       ring buffer; when the buffer empties, mask the TX interrupt to
 *       avoid spurious re-entry.
 *
 *   RX: a byte arrived.  Read it here if you later add an RX ring buffer;
 *       for now it is just consumed to clear the interrupt flag.
 */
void uart_isr(void)
{
    uint32_t mis = UART->MIS;

    /* ── TX FIFO trigger ── */
    if (mis & INT_TX) {
        /* Clear the TX interrupt first to avoid spurious re-arm. */
        UART->ICR = INT_TX;

        /*
         * Drain as many bytes from the ring buffer as the TX FIFO will
         * accept (FR_TXFF clears when there is room).
         */
        while (!(UART->FR & FR_TXFF)) {
            if (tx_rb.tail == tx_rb.head) {
                /* Buffer empty — nothing left to send; mask TX interrupt. */
                UART->IMSC &= ~INT_TX;
                break;
            }

            UART->DR   = tx_rb.buf[tx_rb.tail];
            tx_rb.tail = (tx_rb.tail + 1U) & TX_BUF_MASK;
        }
    }

    /* ── RX FIFO trigger ── */
    if (mis & INT_RX) {
        UART->ICR = INT_RX;

        /*
         * Drain whatever is in the RX FIFO.  Extend this into an RX ring
         * buffer the same way as TX if you need buffered receive.
         */
        while (!(UART->FR & FR_RXFE)) {
            (void)UART->DR;   /* consume byte — replace with RX ring push */
        }
    }
}
