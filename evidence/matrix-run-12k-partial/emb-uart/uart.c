/**
 * uart.c — interrupt-driven UART driver for ARM Cortex-M4
 *
 * Target: STM32F4-class (or any SoC whose USART1 register layout matches).
 * No HAL, no CMSIS.  Every register access goes through the volatile structs
 * defined below.  Adjust BASE_ADDR and bit positions for your specific SoC.
 *
 * Assumed pin / clock setup (caller's responsibility before uart_init):
 *   - USART1 peripheral clock enabled in RCC_APB2ENR (bit 4 on STM32F4).
 *   - TX pin configured as AF push-pull, RX pin as AF input.
 *   - NVIC: enable IRQ37 (USART1_IRQn on STM32F4), set priority.
 */

#include "uart.h"

#include <stddef.h>
#include <stdint.h>

/* ── register map ─────────────────────────────────────────────────────────── */

/*
 * STM32F4 USART register layout (RM0090 §30.6).
 * All offsets are from the peripheral base address.
 */
typedef struct {
    volatile uint32_t SR;    /* 0x00  Status register              */
    volatile uint32_t DR;    /* 0x04  Data register                */
    volatile uint32_t BRR;   /* 0x08  Baud-rate register           */
    volatile uint32_t CR1;   /* 0x0C  Control register 1           */
    volatile uint32_t CR2;   /* 0x10  Control register 2           */
    volatile uint32_t CR3;   /* 0x14  Control register 3           */
    volatile uint32_t GTPR;  /* 0x18  Guard time and prescaler     */
} USART_Regs;

/* USART1 base address — STM32F4.  Override with -DUART_BASE=0x... */
#ifndef UART_BASE
#define UART_BASE  0x40011000UL
#endif

/* Pointer to the register block.  Casting a literal is standard C for
 * memory-mapped I/O on bare-metal targets. */
static USART_Regs * const USART1 = (USART_Regs *)UART_BASE;

/* ── SR bit positions ─────────────────────────────────────────────────────── */
#define SR_PE    (1UL <<  0)   /* Parity error            */
#define SR_FE    (1UL <<  1)   /* Framing error           */
#define SR_NF    (1UL <<  2)   /* Noise detected          */
#define SR_ORE   (1UL <<  3)   /* Overrun error           */
#define SR_IDLE  (1UL <<  4)   /* Idle line detected      */
#define SR_RXNE  (1UL <<  5)   /* Read data reg not empty */
#define SR_TC    (1UL <<  6)   /* Transmission complete   */
#define SR_TXE   (1UL <<  7)   /* Transmit data reg empty */
#define SR_LBD   (1UL <<  8)   /* LIN break detected      */
#define SR_CTS   (1UL <<  9)   /* CTS flag                */

/* ── CR1 bit positions ────────────────────────────────────────────────────── */
#define CR1_SBK    (1UL <<  0)  /* Send break                  */
#define CR1_RWU    (1UL <<  1)  /* Receiver wakeup             */
#define CR1_RE     (1UL <<  2)  /* Receiver enable             */
#define CR1_TE     (1UL <<  3)  /* Transmitter enable          */
#define CR1_IDLEIE (1UL <<  4)  /* IDLE interrupt enable       */
#define CR1_RXNEIE (1UL <<  5)  /* RXNE interrupt enable       */
#define CR1_TCIE   (1UL <<  6)  /* TC interrupt enable         */
#define CR1_TXEIE  (1UL <<  7)  /* TXE interrupt enable        */
#define CR1_PEIE   (1UL <<  8)  /* PE interrupt enable         */
#define CR1_PS     (1UL <<  9)  /* Parity selection            */
#define CR1_PCE    (1UL << 10)  /* Parity control enable       */
#define CR1_WAKE   (1UL << 11)  /* Wakeup method               */
#define CR1_M      (1UL << 12)  /* Word length (0=8, 1=9 bits) */
#define CR1_UE     (1UL << 13)  /* USART enable                */
#define CR1_OVER8  (1UL << 15)  /* Oversampling mode           */

/* ── ring-buffer state ────────────────────────────────────────────────────── */

/*
 * Index mask relies on the buffer sizes being powers of two.
 * A compile-time assertion catches a misconfiguration.
 */
#define TX_MASK  (UART_TX_BUF_SIZE - 1U)
#define RX_MASK  (UART_RX_BUF_SIZE - 1U)

typedef struct {
    uint8_t  buf[UART_TX_BUF_SIZE];
    volatile uint32_t head;   /* written by uart_write (foreground)      */
    volatile uint32_t tail;   /* consumed by ISR (interrupt context)     */
} TxRing;

typedef struct {
    uint8_t  buf[UART_RX_BUF_SIZE];
    volatile uint32_t head;   /* written by ISR (interrupt context)      */
    volatile uint32_t tail;   /* consumed by uart_read (foreground)      */
} RxRing;

static TxRing tx_ring;
static RxRing rx_ring;

/* ── compile-time power-of-two checks ────────────────────────────────────── */

typedef char tx_size_must_be_power_of_two
    [(UART_TX_BUF_SIZE & TX_MASK) == 0 ? 1 : -1];
typedef char rx_size_must_be_power_of_two
    [(UART_RX_BUF_SIZE & RX_MASK) == 0 ? 1 : -1];

/* ── helpers ──────────────────────────────────────────────────────────────── */

/*
 * Memory barrier — compiler fence only.
 * On Cortex-M4 all data accesses are strongly-ordered for device memory,
 * so a __DSB() / __DMB() is not required here, but we still prevent the
 * compiler from reordering the index update past the data store.
 */
#define COMPILER_BARRIER()  __asm__ volatile("" ::: "memory")

static inline uint32_t tx_used(void)
{
    return (tx_ring.head - tx_ring.tail) & TX_MASK;
}

static inline uint32_t tx_free(void)
{
    /* One slot is always reserved so head==tail unambiguously means empty. */
    return (UART_TX_BUF_SIZE - 1U) - tx_used();
}

static inline int tx_empty(void)
{
    return tx_ring.head == tx_ring.tail;
}

static inline uint32_t rx_used(void)
{
    return (rx_ring.head - rx_ring.tail) & RX_MASK;
}

/* ── public API ───────────────────────────────────────────────────────────── */

void uart_init(uint32_t baud)
{
    /* Disable USART while reconfiguring. */
    USART1->CR1 = 0;

    /*
     * BRR for integer (OVER8=0) mode:
     *   USARTDIV = PCLK / baud
     * The mantissa occupies bits [15:4] and the fraction bits [3:0].
     * With OVER8=0 the fraction is in units of 1/16.
     *
     * Simple integer divisor — no fractional part.  For higher accuracy
     * at low baud rates compute fraction = ((PCLK % baud) * 16) / baud
     * and OR it into the lower nibble.
     */
    uint32_t div       = UART_PCLK_HZ / baud;
    uint32_t mantissa  = div >> 4;         /* integer part  */
    uint32_t fraction  = div & 0xFU;       /* fractional nibble */
    USART1->BRR = (mantissa << 4) | fraction;

    /* 8-N-1, no parity, oversampling ×16. */
    USART1->CR2 = 0;   /* 1 stop bit (STOP[1:0] = 00) */
    USART1->CR3 = 0;   /* no flow control, no DMA      */

    /* Enable TX, RX, RXNE interrupt, and the peripheral itself.
     * TXE interrupt is enabled on demand (when we have data to send). */
    USART1->CR1 = CR1_TE | CR1_RE | CR1_RXNEIE | CR1_UE;

    /* Clear ring buffers. */
    tx_ring.head = 0;
    tx_ring.tail = 0;
    rx_ring.head = 0;
    rx_ring.tail = 0;
}

int uart_write(const uint8_t *buf, size_t len)
{
    if (len == 0) {
        return 0;
    }

    /* Snapshot the free space — tail may advance asynchronously, so this is
     * a conservative lower bound.  That is safe: we may queue fewer bytes
     * than the ISR frees between here and the copy, but we never overflow. */
    uint32_t space = tx_free();

    if (space == 0) {
        return -1;   /* buffer full, caller must retry */
    }

    /* Queue up to min(len, space) bytes. */
    uint32_t  to_copy = (uint32_t)len < space ? (uint32_t)len : space;
    uint32_t  head    = tx_ring.head;

    for (uint32_t i = 0; i < to_copy; i++) {
        tx_ring.buf[head & TX_MASK] = buf[i];
        head++;
    }

    /*
     * Publish the new head after the data is written.
     * The compiler barrier prevents reordering the stores to buf[]
     * with the store to tx_ring.head.
     */
    COMPILER_BARRIER();
    tx_ring.head = head;

    /*
     * Kick the TXE interrupt.  If the transmitter is already running the ISR
     * will drain the new bytes; if it went idle we restart it here.
     *
     * The write to CR1 is a read-modify-write.  On Cortex-M4 this is safe
     * from the foreground because the ISR only clears TXEIE — it never sets
     * bits other than the TXEIE clear.  A window exists where TXEIE is
     * cleared by the ISR between our read and write, but that is benign:
     * we set it again, and the ISR will see empty on the next TXE event and
     * clear it once more, causing exactly zero spurious bytes.
     */
    USART1->CR1 |= CR1_TXEIE;

    return (int)to_copy;
}

int uart_read(uint8_t *buf, size_t len)
{
    if (len == 0) {
        return 0;
    }

    uint32_t avail   = rx_used();
    uint32_t to_read = (uint32_t)len < avail ? (uint32_t)len : avail;
    uint32_t tail    = rx_ring.tail;

    for (uint32_t i = 0; i < to_read; i++) {
        buf[i] = rx_ring.buf[tail & RX_MASK];
        tail++;
    }

    COMPILER_BARRIER();
    rx_ring.tail = tail;

    return (int)to_read;
}

int uart_tx_busy(void)
{
    return !tx_empty();
}

/* ── ISR ──────────────────────────────────────────────────────────────────── */

/*
 * USART1_IRQHandler — wired to IRQ37 on STM32F4.
 *
 * We handle two sources in a single ISR:
 *
 *   RXNE  Receive data register not empty.
 *         Read DR once to clear the flag.  If the RX ring is full the byte
 *         is silently dropped — the alternative is leaving RXNE set, which
 *         re-enters the ISR immediately and stalls the core.
 *
 *   TXE   Transmit data register empty.
 *         Write the next byte from the TX ring, or disable TXEIE when the
 *         ring is drained so we stop getting TXE interrupts.
 *
 * The ISR reads SR first (which latches the flags) then accesses DR.
 * On STM32F4 reading SR followed by DR clears RXNE and TXE simultaneously.
 */
void USART1_IRQHandler(void)
{
    uint32_t sr = USART1->SR;

    /* ── receive ───────────────────────────────────────────────────────── */
    if (sr & SR_RXNE) {
        uint8_t byte = (uint8_t)(USART1->DR & 0xFFU);

        uint32_t next_head = (rx_ring.head + 1U) & RX_MASK;

        if (next_head != (rx_ring.tail & RX_MASK)) {
            /* There is room — store and advance head. */
            rx_ring.buf[rx_ring.head & RX_MASK] = byte;
            COMPILER_BARRIER();
            rx_ring.head = rx_ring.head + 1U;
        }
        /* else: ring full, byte dropped.  DR has been read; RXNE cleared. */
    }

    /* ── transmit ──────────────────────────────────────────────────────── */
    if (sr & SR_TXE) {
        if (!tx_empty()) {
            /* Pull next byte and advance tail. */
            uint8_t byte = tx_ring.buf[tx_ring.tail & TX_MASK];
            COMPILER_BARRIER();
            tx_ring.tail = tx_ring.tail + 1U;
            USART1->DR = byte;
        } else {
            /* Nothing left to send — disable TXE interrupt. */
            USART1->CR1 &= ~CR1_TXEIE;
        }
    }
}
