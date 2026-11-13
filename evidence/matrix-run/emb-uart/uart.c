/*
 * uart.c — interrupt-driven UART driver for ARM Cortex-M4
 * Register-level only: no HAL, no CMSIS.
 *
 * Target: generic ARM Cortex-M4 with a memory-mapped UART.
 * The register layout below is a common PL011-style UART; adjust
 * offsets/bit positions to match your specific SoC datasheet.
 *
 * Build (arm-none-eabi-gcc):
 *   arm-none-eabi-gcc -mcpu=cortex-m4 -mthumb -Os -Wall \
 *       -ffreestanding -nostdlib -c uart.c -o uart.o
 *
 * Linker must place the UART ISR handlers (UART0_IRQHandler) at the
 * correct vector-table slot for your MCU.
 */

#include "uart.h"

/* ------------------------------------------------------------------ */
/* Memory barriers                                                     */
/* ------------------------------------------------------------------ */

/* Cortex-M4 is order-preserving for device memory, but the DSB ensures
 * that a register write is observed by the peripheral before we mask or
 * unmask an interrupt that depends on it. */
#define DMB()  __asm volatile ("dmb" ::: "memory")
#define DSB()  __asm volatile ("dsb" ::: "memory")
#define ISB()  __asm volatile ("isb" ::: "memory")

/* ------------------------------------------------------------------ */
/* Register definitions (no CMSIS)                                     */
/* ------------------------------------------------------------------ */

/*
 * UART register map — PL011-style.
 *
 *   0x000  UARTDR       Data register
 *   0x004  UARTFR        Flag register
 *   0x018  UARTILPR      IrDA (unused)
 *   0x020  UARTIBRD      Integer baud-rate divisor
 *   0x024  UARTFBRD      Fractional baud-rate divisor
 *   0x028  UARTLCR_H     Line control
 *   0x02C  UARTCR        Control
 *   0x030  UARTIFLS      Interrupt FIFO level select
 *   0x034  UARTIMSC      Interrupt mask set/clear
 *   0x038  UARTRIS       Raw interrupt status
 *   0x03C  UARTMIS       Masked interrupt status
 *   0x040  UARTICR       Interrupt clear
 */

#define UART0_BASE          0x4000C000u   /* adjust to your SoC */

/* UARTFR bits */
#define UARTFR_TXFE         (1u << 7)    /* TX FIFO empty        */
#define UARTFR_RXFF         (1u << 6)    /* RX FIFO full          */
#define UARTFR_TXFF         (1u << 5)    /* TX FIFO full          */
#define UARTFR_RXFE         (1u << 4)    /* RX FIFO empty         */
#define UARTFR_BUSY         (1u << 3)    /* UART busy transmitting */

/* UARTLCR_H bits */
#define UARTLCRH_FEN        (1u << 4)    /* FIFO enable           */
#define UARTLCRH_WLEN_8    (3u << 5)     /* 8 data bits           */

/* UARTCR bits */
#define UARTCR_UARTEN       (1u << 0)    /* UART enable           */
#define UARTCR_TXE          (1u << 8)    /* TX enable             */
#define UARTCR_RXE          (1u << 9)    /* RX enable             */

/* UARTIMSC / UARTICR shared bits */
#define UART_INT_RX        (1u << 4)     /* RX interrupt          */
#define UART_INT_TX        (1u << 5)     /* TX interrupt          */
#define UART_INT_RTM       (1u << 6)     /* Receive timeout        */

/* ------------------------------------------------------------------ */
/* NVIC — direct register access (no CMSIS)                            */
/* ------------------------------------------------------------------ */

#define NVIC_BASE           0xE000E100u
#define NVIC_ISER0          (*(volatile uint32_t *)(NVIC_BASE + 0x000)) /* set-enable */
#define NVIC_ICER0          (*(volatile uint32_t *)(NVIC_BASE + 0x080)) /* clear-enable */
#define NVIC_IPR_BASE       (*(volatile uint8_t  *)(NVIC_BASE + 0x300)) /* priority */

/*
 * UART0 interrupt number — this is MCU-specific. Adjust to match your
 * vector table. The value below is illustrative; many SoCs place UART0
 * at IRQ 5 or higher.
 */
#define UART0_IRQn          5u

static inline void nvic_enable_irq(uint32_t irq)
{
    NVIC_ISER0 = (1u << irq);
    /* Set lowest priority (0xFF >> 1 = 0x80 in the top bits of the byte) */
    ((volatile uint8_t *)&NVIC_IPR_BASE)[irq] = 0x80u;
}

static inline void nvic_disable_irq(uint32_t irq)
{
    NVIC_ICER0 = (1u << irq);
}

/* ------------------------------------------------------------------ */
/* Register struct at a fixed address                                   */
/* ------------------------------------------------------------------ */

/*
 * A flat struct overlaid on the peripheral memory window.  Using
 * reserved padding keeps every field at the offset the datasheet
 * expects without a packed attribute (ARM is byte-addressable and
 * the struct is naturally aligned).
 */
typedef struct {
    volatile uint32_t DR;          /* 0x00 Data register        */
    volatile uint32_t RSR_ECR;     /* 0x04 Receive status / err clear */
    volatile uint32_t RESERVED0[4];/* 0x08-0x14                */
    volatile uint32_t FR;          /* 0x18 Flag register        */
    volatile uint32_t RESERVED1;   /* 0x1C                      */
    volatile uint32_t ILPR;        /* 0x20 IrDA (unused)        */
    volatile uint32_t IBRD;        /* 0x24 Integer baud divisor  */
    volatile uint32_t FBRD;        /* 0x28 Fractional baud divisor */
    volatile uint32_t LCR_H;       /* 0x2C Line control         */
    volatile uint32_t CR;          /* 0x30 Control              */
    volatile uint32_t IFLS;        /* 0x34 Interrupt FIFO level  */
    volatile uint32_t IMSC;        /* 0x38 Interrupt mask        */
    volatile uint32_t RIS;         /* 0x3C Raw interrupt status  */
    volatile uint32_t MIS;         /* 0x40 Masked interrupt status */
    volatile uint32_t ICR;         /* 0x44 Interrupt clear       */
} UART_RegStruct;

/*
 * Static pointer to the register block at a fixed address.  The
 * cast is to the peripheral address so the compiler treats every
 * access as volatile device memory.
 */
static volatile UART_RegStruct *const UART0 =
    (volatile UART_RegStruct *)UART0_BASE;

/* ------------------------------------------------------------------ */
/* Ring buffers                                                         */
/* ------------------------------------------------------------------ */

#define UART_TX_BUF_SIZE  256u   /* power of two for mask wrap       */
#define UART_RX_BUF_SIZE  256u
#define UART_TX_BUF_MASK  (UART_TX_BUF_SIZE - 1u)
#define UART_RX_BUF_MASK  (UART_RX_BUF_SIZE - 1u)

static uint8_t  tx_buf[UART_TX_BUF_SIZE];
static volatile uint32_t tx_head;  /* producer (uart_write) writes here */
static volatile uint32_t tx_tail;  /* consumer (ISR) reads from here   */

static uint8_t  rx_buf[UART_RX_BUF_SIZE];
static volatile uint32_t rx_head;  /* producer (ISR) writes here      */
static volatile uint32_t rx_tail;  /* consumer (uart_read) reads here  */

/*
 * tx_busy tracks whether the TX interrupt is active.  When the ring
 * drains, the ISR disables the TX interrupt so the CPU isn't flooded.
 * uart_write re-enables it when new data arrives.
 */
static volatile uint8_t tx_busy;

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

static void tx_start(void)
{
    /* Enable the TX interrupt.  If the FIFO has space the peripheral
     * will fire immediately and the ISR will pump the ring. */
    UART0->IMSC |= UART_INT_TX;
    tx_busy = 1u;
}

static void tx_stop(void)
{
    UART0->IMSC &= ~UART_INT_TX;
    tx_busy = 0u;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void uart_init(uint32_t pclk_hz, uint32_t baud)
{
    /*
     * Baud divisor (PL011 formula):
     *
     *   baud_divisor = pclk_hz / (16 * baud)
     *   ibrd         = floor(baud_divisor)
     *   fbrd         = round(frac(baud_divisor) * 64)
     *
     * Computed entirely at runtime from pclk_hz and baud.
     */

    /* Disable UART and all interrupts while we reconfigure. */
    UART0->CR = 0u;
    UART0->IMSC = 0u;
    UART0->ICR = 0x7FFu;   /* clear any pending interrupt flags */

    /* Compute divisors using 64-bit intermediate to avoid overflow. */
    uint64_t denom = (uint64_t)16u * (uint64_t)baud;
    uint64_t div_q = (uint64_t)pclk_hz * 64u;  /* scaled by 64 for fraction */
    uint64_t div   = div_q / denom;             /* = (pclk/(16*baud)) * 64 */
    uint32_t ibrd  = (uint32_t)(div / 64u);
    uint32_t fbrd  = (uint32_t)(div & 63u);

    /* Round fbrd to nearest (already fractional * 64, so 0..63 is exact). */
    /* PL011 rounding: fbrd = round(fraction * 64) is already captured. */

    UART0->IBRD = ibrd;
    UART0->FBRD = fbrd;

    /* 8N1, FIFOs enabled, no flow control. */
    UART0->LCR_H = UARTLCRH_WLEN_8 | UARTLCRH_FEN;

    /* Default FIFO interrupt trigger levels (1/8 RX, 1/8 TX for PL011
     * means IFLS = 0 for both fields — 1/8 full on TX, 1/8 full on RX).
     * Adjust if you want different trigger points. */
    UART0->IFLS = 0u;

    /* Enable RX, TX, and the UART itself. */
    UART0->CR = UARTCR_UARTEN | UARTCR_TXE | UARTCR_RXE;

    /* Clear ring-buffer indices. */
    tx_head = 0u;
    tx_tail = 0u;
    rx_head = 0u;
    rx_tail = 0u;
    tx_busy = 0u;

    /* Enable RX and receive-timeout interrupts (TX interrupt starts
     * on demand when uart_write is called). */
    UART0->ICR = 0x7FFu;
    UART0->IMSC = UART_INT_RX | UART_INT_RTM;

    DSB();

    /* Enable the UART interrupt in the NVIC. */
    nvic_enable_irq(UART0_IRQn);
}

int uart_write(const uint8_t *buf, size_t len)
{
    if (len == 0u || buf == NULL) {
        return 0;
    }

    /*
     * Check if the buffer is completely full before writing any byte.
     * If it is, return -1 as required.
     */
    uint32_t head = tx_head;
    uint32_t tail = tx_tail;
    uint32_t used = (head - tail) & UART_TX_BUF_MASK;
    /* When used == size-1 the buffer is full (one slot reserved to
     * distinguish full from empty). */

    if (used == (UART_TX_BUF_SIZE - 1u)) {
        /* Buffer completely full — no bytes written. */
        return -1;
    }

    /* Copy as many bytes as will fit. */
    size_t written = 0u;
    while (written < len) {
        head = tx_head;
        tail = tx_tail;
        used = (head - tail) & UART_TX_BUF_MASK;

        if (used == (UART_TX_BUF_SIZE - 1u)) {
            break;  /* ring is full now */
        }

        tx_buf[head] = buf[written];
        tx_head = (head + 1u) & UART_TX_BUF_MASK;
        written++;
    }

    if (written > 0u && !tx_busy) {
        tx_start();
    }

    return (int)written;
}

int uart_read(uint8_t *buf, size_t len)
{
    if (len == 0u || buf == NULL) {
        return 0;
    }

    size_t count = 0u;
    uint32_t head = rx_head;

    while (count < len) {
        uint32_t tail = rx_tail;
        if (tail == head) {
            break;  /* RX ring empty */
        }

        buf[count] = rx_buf[tail];
        rx_tail = (tail + 1u) & UART_RX_BUF_MASK;
        count++;
    }

    return (int)count;
}

/* ------------------------------------------------------------------ */
/* Interrupt handler — DMA-free, register-level                         */
/* ------------------------------------------------------------------ */

/*
 * This must match the symbol your vector table uses for the UART0
 * interrupt.  For a standard Cortex-M4 vector table the entry is the
 * function name with no decoration.  Adjust the name if your startup
 * file expects a different symbol.
 */
void UART0_IRQHandler(void)
{
    uint32_t mis = UART0->MIS;

    /* ---- TX: pump bytes from the ring into the TX FIFO ---- */
    if (mis & UART_INT_TX) {
        while (!(UART0->FR & UARTFR_TXFF)) {
            uint32_t head = tx_head;
            uint32_t tail = tx_tail;

            if (head == tail) {
                /* Ring empty — disable TX interrupt. */
                tx_stop();
                break;
            }

            UART0->DR = tx_buf[tail];
            tx_tail = (tail + 1u) & UART_TX_BUF_MASK;
        }
    }

    /* ---- RX: drain FIFO into the RX ring ---- */
    if (mis & (UART_INT_RX | UART_INT_RTM)) {
        while (!(UART0->FR & UARTFR_RXFE)) {
            uint32_t head = rx_head;
            uint32_t next = (head + 1u) & UART_RX_BUF_MASK;

            if (next == rx_tail) {
                /* RX ring full — drop the oldest unread byte to make room
                 * for the newest, or discard incoming.  Here we drop the
                 * incoming byte to avoid silently corrupting old data. */
                (void)UART0->DR;  /* read to clear the FIFO entry */
                continue;
            }

            rx_buf[head] = (uint8_t)UART0->DR;
            rx_head = next;
        }
    }

    /* Clear the interrupt flags we handled. */
    UART0->ICR = UART_INT_RX | UART_INT_TX | UART_INT_RTM;
}
