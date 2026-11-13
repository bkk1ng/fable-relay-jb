/**
 * i2c.h — Bare-metal I2C master driver for ARM Cortex-M4
 *
 * Register-level, polling, 7-bit addressing, no HAL.
 * Assumes the I2C peripheral clock and GPIO alternate functions
 * are configured before calling i2c_init().
 *
 * Error codes:
 *   I2C_OK        (0)  — success
 *   I2C_ERR_TIMEOUT    — flag wait exceeded timeout
 *   I2C_ERR_ARBITRATION— bus arbitration lost
 *   I2C_ERR_BUS        — bus error (misplaced START/STOP)
 *   I2C_ERR_NACK       — NACK received from slave
 */

#ifndef I2C_H
#define I2C_H

#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/*  Error codes                                                         */
/* ------------------------------------------------------------------ */
#define I2C_OK               (0)
#define I2C_ERR_TIMEOUT      (-1)
#define I2C_ERR_ARBITRATION  (-2)
#define I2C_ERR_BUS          (-3)
#define I2C_ERR_NACK         (-4)

/* ------------------------------------------------------------------ */
/*  I2C peripheral register layout (matches STM32F4 / generic Cortex-M4) */
/* ------------------------------------------------------------------ */
typedef struct {
    volatile uint32_t CR1;      /* 0x00 Control register 1            */
    volatile uint32_t CR2;      /* 0x04 Control register 2            */
    volatile uint32_t OAR1;     /* 0x08 Own address register 1        */
    volatile uint32_t OAR2;     /* 0x0C Own address register 2        */
    volatile uint32_t DR;       /* 0x10 Data register                 */
    volatile uint32_t SR1;      /* 0x14 Status register 1             */
    volatile uint32_t SR2;      /* 0x18 Status register 2             */
    volatile uint32_t CCR;      /* 0x1C Clock control register        */
    volatile uint32_t TRISE;    /* 0x20 Rise-time register            */
    volatile uint32_t FLTR;     /* 0x24 Filter register (some devices)*/
} I2C_Regs;

/* ------------------------------------------------------------------ */
/*  CR1 bits                                                            */
/* ------------------------------------------------------------------ */
#define I2C_CR1_PE          (1u << 0)   /* Peripheral enable           */
#define I2C_CR1_SMBUS       (1u << 1)
#define I2C_CR1_START       (1u << 8)   /* Generate START condition    */
#define I2C_CR1_STOP        (1u << 9)   /* Generate STOP condition     */
#define I2C_CR1_ACK         (1u << 10)  /* Acknowledge enable          */
#define I2C_CR1_SWRST       (1u << 15)  /* Software reset              */

/* ------------------------------------------------------------------ */
/*  SR1 bits                                                            */
/* ------------------------------------------------------------------ */
#define I2C_SR1_SB          (1u << 0)   /* Start bit generated         */
#define I2C_SR1_ADDR        (1u << 1)   /* Address sent / matched      */
#define I2C_SR1_BTF         (1u << 2)   /* Byte transfer finished      */
#define I2C_SR1_RXNE        (1u << 6)   /* Receive buffer not empty    */
#define I2C_SR1_TXE         (1u << 7)   /* Transmit buffer empty       */
#define I2C_SR1_BERR        (1u << 8)   /* Bus error                   */
#define I2C_SR1_ARLO        (1u << 9)   /* Arbitration lost            */
#define I2C_SR1_AF          (1u << 10)  /* Acknowledge failure (NACK)  */
#define I2C_SR1_OVR         (1u << 11)
#define I2C_SR1_PECERR      (1u << 12)

/* SR2 bit used to clear ADDR */
#define I2C_SR2_MSL         (1u << 0)   /* Master/slave                */
#define I2C_SR2_BUSY        (1u << 1)   /* Bus busy                    */
#define I2C_SR2_TRA         (1u << 2)   /* Transmitter/receiver        */

/* ------------------------------------------------------------------ */
/*  Timeout (loop-count based; tune to match your system clock)        */
/* ------------------------------------------------------------------ */
#ifndef I2C_TIMEOUT_CNT
#define I2C_TIMEOUT_CNT  (100000u)
#endif

/* ------------------------------------------------------------------ */
/*  Configuration structure                                             */
/* ------------------------------------------------------------------ */
typedef struct {
    I2C_Regs *regs;         /* Base address of I2C peripheral          */
    uint32_t  pclk_hz;      /* APB1 peripheral clock frequency (Hz)    */
    uint32_t  speed_hz;     /* Desired SCL speed: 100000 or 400000     */
} I2C_Config;

/* ------------------------------------------------------------------ */
/*  Core driver API                                                     */
/* ------------------------------------------------------------------ */

/**
 * i2c_init — Configure CCR, TRISE, enable peripheral.
 * Call once after RCC and GPIO setup.
 */
int i2c_init(const I2C_Config *cfg);

/**
 * i2c_start — Generate a (re)START condition and wait for SB.
 */
int i2c_start(I2C_Regs *i2c);

/**
 * i2c_stop — Generate a STOP condition.
 */
void i2c_stop(I2C_Regs *i2c);

/**
 * i2c_address — Send the 7-bit address + R/W bit and wait for ADDR.
 * @addr: 7-bit device address (not pre-shifted)
 * @rw:   0 = write, 1 = read
 */
int i2c_address(I2C_Regs *i2c, uint8_t addr, uint8_t rw);

/**
 * i2c_write — Write @len bytes from @buf to the addressed device.
 * ADDR must already be cleared before calling.
 */
int i2c_write(I2C_Regs *i2c, const uint8_t *buf, size_t len);

/**
 * i2c_read — Read @len bytes into @buf.
 * Handles ACK/NACK sequencing and the BTF trick for the last two bytes.
 * ADDR must already be cleared before calling.
 */
int i2c_read(I2C_Regs *i2c, uint8_t *buf, size_t len);

/* ------------------------------------------------------------------ */
/*  Composite helper: register read-back (write reg addr, restart, read) */
/* ------------------------------------------------------------------ */

/**
 * i2c_write_read — Write @wlen bytes, generate repeated-START,
 * then read @rlen bytes from device @addr.
 */
int i2c_write_read(I2C_Regs *i2c, uint8_t addr,
                   const uint8_t *wbuf, size_t wlen,
                   uint8_t       *rbuf, size_t rlen);

/* ------------------------------------------------------------------ */
/*  BMP280                                                              */
/* ------------------------------------------------------------------ */
#define BMP280_I2C_ADDR_SDO_LOW   0x76u
#define BMP280_I2C_ADDR_SDO_HIGH  0x77u

/** Trimming coefficients from the BMP280 NVM (section 4.2.2). */
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
} BMP280_CalibData;

/**
 * bmp280_read_calibration — Read the 24 calibration bytes from NVM
 * and populate @cal.
 */
int bmp280_read_calibration(I2C_Regs *i2c, uint8_t addr,
                             BMP280_CalibData *cal);

/**
 * bmp280_read_temp_compensated — Read the raw 20-bit ADC temperature,
 * apply the BMP280 integer compensation formula (datasheet sec 4.2.3),
 * and return the result in units of 0.01 °C.
 *
 * Example: return value 2345 means 23.45 °C.
 *
 * @t_fine_out: if non-NULL, receives the t_fine value used for
 *              pressure compensation in a subsequent call.
 */
int32_t bmp280_read_temp_compensated(I2C_Regs *i2c, uint8_t addr,
                                      const BMP280_CalibData *cal,
                                      int32_t *t_fine_out);

#endif /* I2C_H */
