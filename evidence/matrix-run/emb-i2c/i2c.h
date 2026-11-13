/*
 * i2c.h — register-level polling I2C master, ARM Cortex-M4 / STM32F4
 *
 * Targets the STM32F4 I2C peripheral (RM0090 §27).
 * No HAL, no CMSIS device headers — only <stdint.h>.
 *
 * Usage sketch
 * ------------
 *   // 1. Configure clocks, GPIO AF, I2C peripheral (CR2 FREQ, CCR, TRISE,
 *   //    CR1 PE) before calling any function here.
 *   // 2. Pass a pointer to one of the I2C1/2/3 macros as the first argument.
 */

#ifndef I2C_H
#define I2C_H

#include <stdint.h>

/* -------------------------------------------------------------------------
 * Peripheral register map  (RM0090 rev 19, §27.6)
 * ------------------------------------------------------------------------- */
typedef struct {
    volatile uint32_t CR1;    /* 0x00  Control register 1              */
    volatile uint32_t CR2;    /* 0x04  Control register 2              */
    volatile uint32_t OAR1;   /* 0x08  Own address register 1          */
    volatile uint32_t OAR2;   /* 0x0C  Own address register 2          */
    volatile uint32_t DR;     /* 0x10  Data register                   */
    volatile uint32_t SR1;    /* 0x14  Status register 1               */
    volatile uint32_t SR2;    /* 0x18  Status register 2               */
    volatile uint32_t CCR;    /* 0x1C  Clock control register          */
    volatile uint32_t TRISE;  /* 0x20  Rise time register              */
    volatile uint32_t FLTR;   /* 0x24  Filter register                 */
} I2C_Regs;

/* Base addresses */
#define I2C1_BASE  0x40005400UL
#define I2C2_BASE  0x40005800UL
#define I2C3_BASE  0x40005C00UL

#define I2C1  ((I2C_Regs *)I2C1_BASE)
#define I2C2  ((I2C_Regs *)I2C2_BASE)
#define I2C3  ((I2C_Regs *)I2C3_BASE)

/* -------------------------------------------------------------------------
 * CR1 bit positions
 * ------------------------------------------------------------------------- */
#define I2C_CR1_PE       (1UL <<  0)  /* Peripheral enable                 */
#define I2C_CR1_START    (1UL <<  8)  /* Generate start condition           */
#define I2C_CR1_STOP     (1UL <<  9)  /* Generate stop condition            */
#define I2C_CR1_ACK     (1UL << 10)  /* Acknowledge enable                 */
#define I2C_CR1_POS     (1UL << 11)  /* ACK/PEC position (2-byte Rx)       */
#define I2C_CR1_SWRST   (1UL << 15)  /* Software reset                     */

/* -------------------------------------------------------------------------
 * SR1 bit positions
 * ------------------------------------------------------------------------- */
#define I2C_SR1_SB       (1UL <<  0)  /* Start bit generated                */
#define I2C_SR1_ADDR     (1UL <<  1)  /* Address sent / matched             */
#define I2C_SR1_BTF      (1UL <<  2)  /* Byte transfer finished             */
#define I2C_SR1_RXNE     (1UL <<  6)  /* Receive data register not empty    */
#define I2C_SR1_TXE      (1UL <<  7)  /* Transmit data register empty       */
#define I2C_SR1_BERR     (1UL <<  8)  /* Bus error                          */
#define I2C_SR1_ARLO     (1UL <<  9)  /* Arbitration lost                   */
#define I2C_SR1_AF       (1UL << 10)  /* Acknowledge failure                */
#define I2C_SR1_OVR      (1UL << 11)  /* Overrun / underrun                 */
#define I2C_SR1_PECERR   (1UL << 12)  /* PEC error in reception             */
#define I2C_SR1_TIMEOUT  (1UL << 14)  /* Timeout / Tlow error               */
#define I2C_SR1_ERR_MASK (I2C_SR1_BERR | I2C_SR1_ARLO | I2C_SR1_AF  | \
                          I2C_SR1_OVR  | I2C_SR1_TIMEOUT)

/* -------------------------------------------------------------------------
 * SR2 bit positions
 * ------------------------------------------------------------------------- */
#define I2C_SR2_BUSY     (1UL <<  1)  /* Bus busy                           */

/* -------------------------------------------------------------------------
 * Error codes
 * ------------------------------------------------------------------------- */
typedef enum {
    I2C_OK           =  0,
    I2C_ERR_TIMEOUT  = -1,  /* Flag never set within I2C_TIMEOUT iterations  */
    I2C_ERR_NACK     = -2,  /* Address or data was not acknowledged           */
    I2C_ERR_ARBLOST  = -3,  /* Arbitration lost on multi-master bus           */
    I2C_ERR_BUS      = -4,  /* BERR, OVR, or TIMEOUT flag set in SR1         */
} I2C_Status;

/*
 * Spin-loop iteration count per flag wait.
 * Override at compile time with -DI2C_TIMEOUT=<n>.
 * At 168 MHz with one I2C_TIMEOUT iteration ≈ 2–4 cycles, 100000 gives
 * roughly 2–4 ms — enough for a 100 kHz byte but not so long it hangs
 * forever on a dead bus.
 */
#ifndef I2C_TIMEOUT
#define I2C_TIMEOUT  100000UL
#endif

/* -------------------------------------------------------------------------
 * Low-level single-operation helpers
 * ------------------------------------------------------------------------- */

/*
 * i2c_start — assert a START (or repeated START) condition.
 *             Returns when SB is set in SR1.
 */
I2C_Status i2c_start(I2C_Regs *i2c);

/*
 * i2c_stop — assert a STOP condition and spin until the bus goes idle
 *            (SR2.BUSY clears).
 */
I2C_Status i2c_stop(I2C_Regs *i2c);

/*
 * i2c_send_addr — write 7-bit address + R/W bit into DR, wait for ADDR,
 *                 then clear the flag by reading SR1 + SR2.
 *                 rw = 0 → write, rw = 1 → read.
 *
 * NOTE: for 1-byte and 2-byte receives the caller must manipulate ACK/POS
 *       before or immediately after clearing ADDR (see i2c_read).
 */
I2C_Status i2c_send_addr(I2C_Regs *i2c, uint8_t addr7, uint8_t rw);

/*
 * i2c_write_byte — wait for TXE, then load data into DR.
 *                  Does NOT wait for BTF; call wait after the last byte.
 */
I2C_Status i2c_write_byte(I2C_Regs *i2c, uint8_t data);

/*
 * i2c_read_byte — wait for RXNE and read DR.
 *                 Set last != 0 on the second-to-last byte so ACK is
 *                 cleared before the final byte is clocked in (the caller
 *                 must have already cleared ACK before calling with last
 *                 for the 1- and 2-byte special cases handled in i2c_read).
 */
I2C_Status i2c_read_byte(I2C_Regs *i2c, uint8_t *data);

/* -------------------------------------------------------------------------
 * Composite transfer helpers
 * ------------------------------------------------------------------------- */

/*
 * i2c_write — START, addr+W, write len bytes from buf, STOP.
 */
I2C_Status i2c_write(I2C_Regs *i2c, uint8_t addr7,
                     const uint8_t *buf, uint32_t len);

/*
 * i2c_read — START, addr+R, read len bytes into buf with correct ACK
 *             handling for 1-, 2-, and N-byte cases, STOP.
 */
I2C_Status i2c_read(I2C_Regs *i2c, uint8_t addr7,
                    uint8_t *buf, uint32_t len);

/*
 * i2c_write_read — START, addr+W, write wlen bytes, repeated START,
 *                  addr+R, read rlen bytes, STOP.
 *                  Standard register-read pattern.
 */
I2C_Status i2c_write_read(I2C_Regs *i2c, uint8_t addr7,
                          const uint8_t *wbuf, uint32_t wlen,
                          uint8_t *rbuf,  uint32_t rlen);

/* -------------------------------------------------------------------------
 * BMP280 helpers
 * ------------------------------------------------------------------------- */
#define BMP280_ADDR_LOW   0x76U   /* SDO pulled low  */
#define BMP280_ADDR_HIGH  0x77U   /* SDO pulled high */

/* BMP280 trim registers 0x88–0x9F (DS001 §4.2.2) */
typedef struct {
    uint16_t dig_T1;
    int16_t  dig_T2;
    int16_t  dig_T3;
    uint16_t dig_P1;
    int16_t  dig_P2;
    int16_t  dig_P3;
    int16_t  dig_P4;
    int16_t  dig_P5;
    int16_t  dig_P6;
    int16_t  dig_P7;
    int16_t  dig_P8;
    int16_t  dig_P9;
} BMP280_Calib;

/*
 * bmp280_read_calibration — read all 24 calibration bytes from registers
 * 0x88–0x9F and populate *calib.  Call once at startup.
 */
I2C_Status bmp280_read_calibration(I2C_Regs *i2c, uint8_t addr7,
                                   BMP280_Calib *calib);

/*
 * bmp280_read_temp_compensated — read 20-bit ADC_T from 0xFA/0xFB/0xFC,
 * apply the BMP280 integer compensation formula (DS001 §4.2.3), and return
 * temperature in units of 0.01 °C (e.g. 2534 → 25.34 °C) in *temp_centideg.
 *
 * If t_fine_out is non-NULL it receives the t_fine intermediate value,
 * which the companion pressure compensation formula requires.
 */
I2C_Status bmp280_read_temp_compensated(I2C_Regs *i2c, uint8_t addr7,
                                        const BMP280_Calib *calib,
                                        int32_t *temp_centideg,
                                        int32_t *t_fine_out);

#endif /* I2C_H */
