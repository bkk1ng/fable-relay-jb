/**
 * uart.h — Interrupt-driven UART driver for ARM Cortex-M4 (STM32F4xx)
 *
 * No HAL, no CMSIS. Register structs at fixed addresses.
 * Targets USART1 on PA9 (TX) / PA10 (RX), AF7.
 *
 * Clock assumptions (adjust PCLK2_HZ in uart.c to match your system):
 *   Default: 16 MHz (HSI, no PLL)
 */

#ifndef UART_H
#define UART_H

#include <stdint.h>

/* -------------------------------------------------------------------------
 * Ring buffer
 * Size must be a power of two; 256 bytes gives an index-wrap mask trick.
 * ------------------------------------------------------------------------- */
#define UART_BUF_SIZE   256u
#define UART_BUF_MASK   (UART_BUF_SIZE - 1u)

typedef struct {
    uint8_t          data[UART_BUF_SIZE];
    volatile uint8_t head;   /* written by producer */
    volatile uint8_t tail;   /* written by consumer */
} RingBuf;

/* -------------------------------------------------------------------------
 * USART register layout  (RM0090 §30.6, identical for USART1–3, UART4–5)
 * ------------------------------------------------------------------------- */
typedef struct {
    volatile uint32_t SR;    /* 0x00  Status register                  */
    volatile uint32_t DR;    /* 0x04  Data register                    */
    volatile uint32_t BRR;   /* 0x08  Baud-rate register               */
    volatile uint32_t CR1;   /* 0x0C  Control register 1               */
    volatile uint32_t CR2;   /* 0x10  Control register 2               */
    volatile uint32_t CR3;   /* 0x14  Control register 3               */
    volatile uint32_t GTPR;  /* 0x18  Guard time / prescaler           */
} USART_Regs;

/* SR bits */
#define USART_SR_RXNE   (1u << 5)   /* Receive data register not empty */
#define USART_SR_TC     (1u << 6)   /* Transmission complete           */
#define USART_SR_TXE    (1u << 7)   /* Transmit data register empty    */
#define USART_SR_ORE    (1u << 3)   /* Overrun error                   */

/* CR1 bits */
#define USART_CR1_RE    (1u << 2)   /* Receiver enable                 */
#define USART_CR1_TE    (1u << 3)   /* Transmitter enable              */
#define USART_CR1_RXNEIE (1u << 5)  /* RXNE interrupt enable           */
#define USART_CR1_TCIE  (1u << 6)   /* TC interrupt enable             */
#define USART_CR1_TXEIE (1u << 7)   /* TXE interrupt enable            */
#define USART_CR1_UE    (1u << 13)  /* USART enable                    */

/* -------------------------------------------------------------------------
 * RCC register layout  (partial — only the fields we touch)
 * ------------------------------------------------------------------------- */
typedef struct {
    volatile uint32_t CR;           /* 0x00 */
    volatile uint32_t PLLCFGR;      /* 0x04 */
    volatile uint32_t CFGR;         /* 0x08 */
    volatile uint32_t CIR;          /* 0x0C */
    volatile uint32_t AHB1RSTR;     /* 0x10 */
    volatile uint32_t AHB2RSTR;     /* 0x14 */
    volatile uint32_t AHB3RSTR;     /* 0x18 */
    uint32_t          _res0;        /* 0x1C */
    volatile uint32_t APB1RSTR;     /* 0x20 */
    volatile uint32_t APB2RSTR;     /* 0x24 */
    uint32_t          _res1[2];     /* 0x28 */
    volatile uint32_t AHB1ENR;      /* 0x30 */
    volatile uint32_t AHB2ENR;      /* 0x34 */
    volatile uint32_t AHB3ENR;      /* 0x38 */
    uint32_t          _res2;        /* 0x3C */
    volatile uint32_t APB1ENR;      /* 0x40 */
    volatile uint32_t APB2ENR;      /* 0x44 */
} RCC_Regs;

#define RCC_AHB1ENR_GPIOAEN  (1u << 0)   /* GPIOA clock enable  */
#define RCC_APB2ENR_USART1EN (1u << 4)   /* USART1 clock enable */

/* -------------------------------------------------------------------------
 * GPIO register layout  (RM0090 §8.4)
 * ------------------------------------------------------------------------- */
typedef struct {
    volatile uint32_t MODER;    /* 0x00  Mode register              */
    volatile uint32_t OTYPER;   /* 0x04  Output type register       */
    volatile uint32_t OSPEEDR;  /* 0x08  Output speed register      */
    volatile uint32_t PUPDR;    /* 0x0C  Pull-up/pull-down register */
    volatile uint32_t IDR;      /* 0x10  Input data register        */
    volatile uint32_t ODR;      /* 0x14  Output data register       */
    volatile uint32_t BSRR;     /* 0x18  Bit set/reset register     */
    volatile uint32_t LCKR;     /* 0x1C  Configuration lock         */
    volatile uint32_t AFR[2];   /* 0x20  Alternate function [0]=low,[1]=high */
} GPIO_Regs;

/* MODER field values (2 bits per pin) */
#define GPIO_MODER_INPUT  0u
#define GPIO_MODER_OUTPUT 1u
#define GPIO_MODER_AF     2u
#define GPIO_MODER_ANALOG 3u

/* OSPEEDR field values */
#define GPIO_SPEED_HIGH   3u

/* PUPDR field values */
#define GPIO_PUPDR_NONE   0u
#define GPIO_PUPDR_UP     1u

/* -------------------------------------------------------------------------
 * NVIC  (Cortex-M4 generic, ARMv7-M architecture)
 * USART1 IRQ = 37  =>  ISER[1], bit 5
 * ------------------------------------------------------------------------- */
typedef struct {
    volatile uint32_t ISER[8];   /* Interrupt Set Enable Registers   */
    uint32_t          _res0[24];
    volatile uint32_t ICER[8];   /* Interrupt Clear Enable Registers */
    uint32_t          _res1[24];
    volatile uint32_t ISPR[8];   /* Interrupt Set Pending Registers  */
    uint32_t          _res2[24];
    volatile uint32_t ICPR[8];   /* Interrupt Clear Pending          */
    uint32_t          _res3[24];
    volatile uint32_t IABR[8];   /* Interrupt Active Bit Registers   */
    uint32_t          _res4[56];
    volatile uint8_t  IPR[240];  /* Interrupt Priority Registers     */
} NVIC_Regs;

/* -------------------------------------------------------------------------
 * Peripheral base addresses  (STM32F405/407/415/417)
 * ------------------------------------------------------------------------- */
#define GPIOA_BASE  0x40020000u
#define RCC_BASE    0x40023800u
#define USART1_BASE 0x40011000u
#define NVIC_BASE   0xE000E100u

#define GPIOA   ((GPIO_Regs  *) GPIOA_BASE)
#define RCC     ((RCC_Regs   *) RCC_BASE)
#define USART1  ((USART_Regs *) USART1_BASE)
#define NVIC    ((NVIC_Regs  *) NVIC_BASE)

/* USART1 IRQ number per the STM32F4 vector table */
#define USART1_IRQn  37u

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/**
 * uart_init - Configure USART1, GPIO pins, and NVIC.
 *
 * @baud: Desired baud rate (e.g. 115200). The divisor is computed from
 *        PCLK2_HZ defined in uart.c — adjust that macro to match your
 *        actual APB2 clock.
 *
 * Call once before any read/write. Interrupts must be globally enabled
 * (CPSIE I) by the caller after initialisation.
 */
void uart_init(uint32_t baud);

/**
 * uart_write - Non-blocking single-byte transmit.
 *
 * Pushes @c into the TX ring buffer and arms the TXE interrupt.
 *
 * Returns:  0  on success.
 *          -1  if the TX ring buffer is full (byte dropped).
 */
int uart_write(uint8_t c);

/**
 * uart_read - Non-blocking single-byte receive.
 *
 * Pops one byte from the RX ring buffer into *@c.
 *
 * Returns:  0  if a byte was available.
 *          -1  if the RX buffer is empty.
 */
int uart_read(uint8_t *c);

#endif /* UART_H */
