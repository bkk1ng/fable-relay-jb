/*
 * uart.c — interrupt-driven UART driver for ARM Cortex-M4
 * Register-level, no HAL, no CMSIS.
 *
 * Target assumptions (adjust the three sections marked ADAPT):
 *   - Generic PL011-compatible UART (also covers many STM32 / TM4C layouts
 *     with minor register renaming; the struct fields call out the mapping).
 *   - UART base address: 0x40011000  (ADAPT)
 *   - Peripheral clock:  16 000 000 Hz (ADAPT)
 *   - IRQ number in NVIC: 37           (ADAPT)
 *
 * Ring buffer size must be a power of two so the index wrap is a cheap mask.
 */

#include "uart.h"
#include <stdint.h>
#include <stddef.h>

/* -------------------------------------------------------------------------
 * ADAPT — target-specific constants
 * ---------------------------------------------------------------------- */
#define UART_BASE       0x40011000UL    /* UART peripheral base address     */
#define PCLK_HZ         16000000UL      /* Peripheral / UART input clock Hz */
#define UART_IRQ_NUM    37              /* IRQ number in the NVIC (0-based) */

/* -------------------------------------------------------------------------
 * Ring-buffer configuration
 * ---------------------------------------------------------------------- */
#define TX_BUF_SIZE     256U    /* Must be a power of two */
#define RX_BUF_SIZE     256U    /* Must be a power of two */
#define TX_BUF_MASK     (TX_BUF_SIZE - 1U)
#define RX_BUF_MASK     (RX_BUF_SIZE - 1U)

/* -------------------------------------------------------------------------
 * UART register map
 *
 * Modelled on the PL011; rename fields as needed for your MCU.
 * Every field is volatile so the compiler never caches a register read.
 * ---------------------------------------------------------------------- */
typedef struct {
    volatile uint32_t DR;       /* 0x000  Data register (R: received byte, W: transmit byte) */
    volatile uint32_t RSR_ECR;  /* 0x004  Receive status / error clear                       */
    volatile uint32_t _pad0[4]; /* 0x008-0x014 reserved                                      */
    volatile uint32_t FR;       /* 0x018  Flag register                                       */
    volatile uint32_t _pad1[1]; /* 0x01C  reserved                                            */
    volatile uint32_t ILPR;     /* 0x020  IrDA low-power counter (unused)                     */
    volatile uint32_t IBRD;     /* 0x024  Integer baud-rate divisor                           */
    volatile uint32_t FBRD;     /* 0x028  Fractional baud-rate divisor                        */
    volatile uint32_t LCRH;     /* 0x02C  Line control register                               */
    volatile uint32_t CR;       /* 0x030  Control register                                    */
    volatile uint32_t IFLS;     /* 0x034  Interrupt FIFO level select                         */
    volatile uint32_t IMSC;     /* 0x038  Interrupt mask set/clear                            */
    volatile uint32_t RIS;      /* 0x03C  Raw interrupt status                                */
    volatile uint32_t MIS;      /* 0x040  Masked interrupt status                             */
    volatile uint32_t ICR;      /* 0x044  Interrupt clear register                            */
} uart_regs_t;

/* Flag register bits */
#define FR_TXFF     (1U << 5)   /* TX FIFO full       */
#define FR_RXFE     (1U << 4)   /* RX FIFO empty      */
#define FR_BUSY     (1U << 3)   /* UART busy          */
#define FR_TXFE     (1U << 7)   /* TX FIFO empty      */

/* Line control register bits */
#define LCRH_WLEN8  (3U << 5)   /* 8-bit word length  */
#define LCRH_FEN    (1U << 4)   /* FIFO enable        */

/* Control register bits */
#define CR_UARTEN   (1U << 0)   /* UART enable        */
#define CR_TXE      (1U << 8)   /* TX enable          */
#define CR_RXE      (1U << 9)   /* RX enable          */

/* Interrupt mask bits (IMSC / ICR) */
#define INT_RXIM    (1U << 4)   /* RX interrupt       */
#define INT_TXIM    (1U << 5)   /* TX interrupt       */
#define INT_RTIM    (1U << 6)   /* Receive timeout    */

/* -------------------------------------------------------------------------
 * NVIC register map (Cortex-M4 fixed addresses)
 * ---------------------------------------------------------------------- */
typedef struct {
    volatile uint32_t ISER[8];  /* 0xE000E100  Interrupt Set Enable Registers   */
    volatile uint32_t _pad[24];
    volatile uint32_t ICER[8];  /* 0xE000E180  Interrupt Clear Enable Registers */
} nvic_t;

#define NVIC   ((nvic_t *)0xE000E100UL)

/* -------------------------------------------------------------------------
 * Register accessor
 * ---------------------------------------------------------------------- */
#define UART    ((uart_regs_t *)UART_BASE)

/* -------------------------------------------------------------------------
 * Ring buffers and indices
 *
 * head = next write index, tail = next read index.
 * Buffer is empty when head == tail; full when (head+1) & mask == tail.
 * Indices are uint32_t; they never wrap in a way that breaks the mask logic.
 * ---------------------------------------------------------------------- */
static volatile uint8_t  tx_buf[TX_BUF_SIZE];
static volatile uint32_t tx_head;   /* written by uart_write, read by ISR   */
static volatile uint32_t tx_tail;   /* consumed by ISR only                 */

static volatile uint8_t  rx_buf[RX_BUF_SIZE];
static volatile uint32_t rx_head;   /* written by ISR only                  */
static volatile uint32_t rx_tail;   /* consumed by uart_read                */

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */
static inline int tx_buf_full(void)
{
    return (((tx_head + 1U) & TX_BUF_MASK) == (tx_tail & TX_BUF_MASK));
}

static inline int tx_buf_empty(void)
{
    return ((tx_head & TX_BUF_MASK) == (tx_tail & TX_BUF_MASK));
}

static inline int rx_buf_empty(void)
{
    return ((rx_head & RX_BUF_MASK) == (rx_tail & RX_BUF_MASK));
}

/*
 * Data memory barrier — prevents the compiler and CPU from reordering
 * volatile accesses across a critical-section boundary on Cortex-M4.
 */
static inline void dmb(void)
{
    __asm volatile ("dmb sy" ::: "memory");
}

/* -------------------------------------------------------------------------
 * uart_init
 *
 * Baud rate calculation (PL011 / TI-style):
 *   BRD          = PCLK / (16 * baud)
 *   IBRD         = floor(BRD)
 *   FBRD         = round(frac(BRD) * 64)   — 6-bit fractional divisor
 *
 * Integer-only derivation (no floating point):
 *   divisor_x64  = (PCLK * 4) / baud       == BRD * 64
 *   IBRD         = divisor_x64 >> 6
 *   FBRD         = divisor_x64 & 0x3F
 *
 * The peripheral clock gate must be enabled before calling this function;
 * that step is SoC-specific and not included here.
 * ---------------------------------------------------------------------- */
void uart_init(uint32_t baud)
{
    /* Disable UART before reconfiguring */
    UART->CR = 0U;

    /* Wait for any in-progress transmission to complete */
    while (UART->FR & FR_BUSY) {
        /* spin */
    }

    /* Flush receive FIFO by disabling it */
    UART->LCRH &= ~LCRH_FEN;

    /* Compute and load the baud-rate divisors */
    uint32_t divisor_x64 = (PCLK_HZ * 4U) / baud;
    UART->IBRD = divisor_x64 >> 6U;
    UART->FBRD = divisor_x64 & 0x3FU;

    /* 8N1, FIFO enabled — LCRH must be written AFTER IBRD/FBRD */
    UART->LCRH = LCRH_WLEN8 | LCRH_FEN;

    /* FIFO interrupt thresholds: TX fires at half-empty, RX at half-full */
    UART->IFLS = (0x2U << 3U) | 0x2U;

    /* Clear any stale pending interrupts */
    UART->ICR = 0x7FFU;

    /* Unmask RX, TX, and receive-timeout interrupts */
    UART->IMSC = INT_RXIM | INT_TXIM | INT_RTIM;

    /* Re-enable UART: TX + RX */
    UART->CR = CR_UARTEN | CR_TXE | CR_RXE;

    /* Enable the UART IRQ in the NVIC */
    NVIC->ISER[UART_IRQ_NUM >> 5U] = (1U << (UART_IRQ_NUM & 0x1FU));
}

/* -------------------------------------------------------------------------
 * uart_write
 *
 * Enqueues one byte for transmission.
 * Returns  0 on success.
 * Returns -1 when the TX ring buffer is full (non-blocking).
 *
 * If the TX interrupt is currently masked (the ISR drained the buffer and
 * stopped), the byte is also kicked directly into the hardware FIFO and
 * the TX interrupt is re-enabled so the ISR takes over for the remainder.
 * ---------------------------------------------------------------------- */
int uart_write(uint8_t byte)
{
    if (tx_buf_full()) {
        return -1;
    }

    /* Write into the ring buffer */
    tx_buf[tx_head & TX_BUF_MASK] = byte;
    dmb();
    tx_head++;

    /*
     * If the TX interrupt is masked the ISR is idle.
     * Re-enable it and kick one byte if the hardware FIFO has room,
     * so the interrupt fires and drains the rest.
     */
    if (!(UART->IMSC & INT_TXIM)) {
        UART->IMSC |= INT_TXIM;

        if (!(UART->FR & FR_TXFF) && !tx_buf_empty()) {
            UART->DR = tx_buf[tx_tail & TX_BUF_MASK];
            dmb();
            tx_tail++;
        }
    }

    return 0;
}

/* -------------------------------------------------------------------------
 * uart_read
 *
 * Returns the next byte from the RX ring buffer (0-255), or -1 if empty.
 * Never blocks.
 * ---------------------------------------------------------------------- */
int uart_read(void)
{
    if (rx_buf_empty()) {
        return -1;
    }

    uint8_t byte = rx_buf[rx_tail & RX_BUF_MASK];
    dmb();
    rx_tail++;
    return (int)byte;
}

/* -------------------------------------------------------------------------
 * UART_IRQHandler
 *
 * Handles RX, receive-timeout, and TX events.
 *
 *   RX / receive-timeout:
 *     Drain the hardware FIFO into rx_buf until it empties.  When the
 *     software buffer is full, incoming bytes are silently dropped.
 *     Add an overrun counter here if your application needs to track that.
 *
 *   TX:
 *     Fill the hardware FIFO from tx_buf while both have capacity.
 *     When the software buffer is exhausted, mask the TX interrupt to
 *     prevent spurious re-entry until the next uart_write call.
 *
 * The function name must match your MCU's vector table weak alias.
 * If your toolchain expects a different name (e.g. UART0_Handler,
 * USART1_IRQHandler) rename it here or add an alias:
 *
 *   void USART1_IRQHandler(void) __attribute__((alias("UART_IRQHandler")));
 * ---------------------------------------------------------------------- */
void UART_IRQHandler(void)
{
    uint32_t mis = UART->MIS;

    /* --- RX and receive-timeout --- */
    if (mis & (INT_RXIM | INT_RTIM)) {
        while (!(UART->FR & FR_RXFE)) {
            uint8_t byte = (uint8_t)(UART->DR & 0xFFU);

            uint32_t next_head = (rx_head + 1U) & RX_BUF_MASK;
            if (next_head != (rx_tail & RX_BUF_MASK)) {
                rx_buf[rx_head & RX_BUF_MASK] = byte;
                dmb();
                rx_head++;
            }
            /* else: software RX overflow — byte is dropped */
        }
        UART->ICR = INT_RXIM | INT_RTIM;
    }

    /* --- TX --- */
    if (mis & INT_TXIM) {
        while (!(UART->FR & FR_TXFF) && !tx_buf_empty()) {
            UART->DR = tx_buf[tx_tail & TX_BUF_MASK];
            dmb();
            tx_tail++;
        }

        if (tx_buf_empty()) {
            /* Nothing left to send — mask TX interrupt until next write */
            UART->IMSC &= ~INT_TXIM;
        }

        UART->ICR = INT_TXIM;
    }
}
