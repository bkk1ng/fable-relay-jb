/**
 * i2c.h — Polling I2C master, register-level, STM32F4 Cortex-M4.
 * No HAL.  7-bit addressing.  All flag waits are bounded by I2C_TIMEOUT.
 *
 * Assumed peripheral clock: APB1.  Call i2c_init() (not declared here) or
 * configure CR2/CCR/TRISE yourself before using these helpers.
 */

#ifndef I2C_H
#define I2C_H

#include <stdint.h>

/* -----------------------------------------------------------------------
 * Register map
 * --------------------------------------------------------------------- */
typedef struct {
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t OAR1;
    volatile uint32_t OAR2;
    volatile uint32_t DR;
    volatile uint32_t SR1;
    volatile uint32_t SR2;
    volatile uint32_t CCR;
    volatile uint32_t TRISE;
    volatile uint32_t FLTR;
} I2C_Regs;

#define I2C1  ((I2C_Regs *)0x40005400UL)
#define I2C2  ((I2C_Regs *)0x40005800UL)
#define I2C3  ((I2C_Regs *)0x40005C00UL)

/* CR1 ----------------------------------------------------------------- */
#define I2C_CR1_PE      (1U <<  0)   /* peripheral enable                */
#define I2C_CR1_START   (1U <<  8)   /* start generation                 */
#define I2C_CR1_STOP    (1U <<  9)   /* stop generation                  */
#define I2C_CR1_ACK     (1U << 10)   /* acknowledge enable               */
#define I2C_CR1_POS     (1U << 11)   /* ACK/PEC position (2-byte rx)     */
#define I2C_CR1_SWRST   (1U << 15)   /* software reset                   */

/* SR1 ----------------------------------------------------------------- */
#define I2C_SR1_SB      (1U <<  0)   /* start bit generated              */
#define I2C_SR1_ADDR    (1U <<  1)   /* address sent / matched           */
#define I2C_SR1_BTF     (1U <<  2)   /* byte transfer finished           */
#define I2C_SR1_RXNE    (1U <<  6)   /* RX data register not empty       */
#define I2C_SR1_TXE     (1U <<  7)   /* TX data register empty           */
#define I2C_SR1_BERR    (1U <<  8)   /* bus error                        */
#define I2C_SR1_ARLO    (1U <<  9)   /* arbitration lost                 */
#define I2C_SR1_AF      (1U << 10)   /* acknowledge failure              */

/* SR2 ----------------------------------------------------------------- */
#define I2C_SR2_BUSY    (1U <<  1)   /* bus busy                         */

/* R/W bit appended to the 7-bit address -------------------------------- */
#define I2C_WRITE  0U
#define I2C_READ   1U

/* -----------------------------------------------------------------------
 * Timeout
 * Tune to your APB1 clock.  At 42 MHz one loop iteration ≈ a few cycles,
 * so 200 000 gives roughly 1–2 ms margin at 100 kHz SCL.
 * --------------------------------------------------------------------- */
#define I2C_TIMEOUT  200000U

/* -----------------------------------------------------------------------
 * Error codes
 * --------------------------------------------------------------------- */
typedef enum {
    I2C_OK          =  0,
    I2C_ERR_TIMEOUT = -1,   /* flag did not set/clear in time           */
    I2C_ERR_AF      = -2,   /* NACK received (acknowledge failure)      */
    I2C_ERR_ARLO    = -3,   /* arbitration lost                         */
    I2C_ERR_BUS     = -4,   /* bus error (misplaced START/STOP)         */
} I2C_Status;

/* -----------------------------------------------------------------------
 * BMP280
 * --------------------------------------------------------------------- */
#define BMP280_ADDR_LOW   0x76U   /* SDO tied to GND */
#define BMP280_ADDR_HIGH  0x77U   /* SDO tied to VCC */

/* Register addresses */
#define BMP280_REG_CALIB00   0x88U   /* T1 LSB — start of calib block   */
#define BMP280_REG_CTRL_MEAS 0xF4U   /* osrs_t, osrs_p, mode            */
#define BMP280_REG_TEMP_MSB  0xFAU   /* 0xFA..0xFC = 20-bit raw temp    */

/* ctrl_meas: osrs_t=001 (×1), osrs_p=001 (×1), mode=11 (normal) */
#define BMP280_CTRL_NORMAL   0x27U

/** Trimming parameters for temperature (from NVM, 0x88–0x8D). */
typedef struct {
    uint16_t dig_T1;   /* unsigned */
    int16_t  dig_T2;
    int16_t  dig_T3;
} BMP280_CalibTemp;

/* -----------------------------------------------------------------------
 * Primitive helpers
 *
 * These operate on a fully initialised I2C peripheral (PE=1).
 * Callers own the transaction state; pair every i2c_start with i2c_stop.
 * --------------------------------------------------------------------- */

/**
 * Generate a START (or repeated START) condition and wait for SB.
 */
I2C_Status i2c_start(I2C_Regs *i2c);

/**
 * Transmit a 7-bit address with the R/W bit appended, then wait for the
 * ADDR flag and clear it by reading SR1+SR2.
 *
 * @param addr7  7-bit device address (not pre-shifted)
 * @param rw     I2C_WRITE (0) or I2C_READ (1)
 *
 * Before calling in read mode for a 1-byte transfer, the caller MUST
 * clear CR1_ACK and set CR1_STOP immediately after this function returns
 * (the hardware sends NACK + STOP before the byte arrives).
 */
I2C_Status i2c_send_addr(I2C_Regs *i2c, uint8_t addr7, uint8_t rw);

/**
 * Write one byte and wait for BTF (byte transferred + ACKed).
 * Returns I2C_ERR_AF if the slave NACKs.
 */
I2C_Status i2c_write_byte(I2C_Regs *i2c, uint8_t data);

/**
 * Read one byte from the bus.
 *
 * @param out  destination byte
 * @param ack  1 → ACK the byte (more data to follow)
 *             0 → NACK the byte (last byte of transfer)
 *
 * When ack == 0, clear CR1_ACK and set CR1_STOP BEFORE calling this
 * function (i.e. before the hardware releases the byte).  The helpers
 * bmp280_read_calibration / bmp280_read_temp_compensated do this for you.
 */
I2C_Status i2c_read_byte(I2C_Regs *i2c, uint8_t *out);

/**
 * Generate a STOP condition.  Does NOT poll for bus-idle; the next
 * i2c_start will block until BUSY clears.
 */
void i2c_stop(I2C_Regs *i2c);

/* -----------------------------------------------------------------------
 * BMP280 compound helpers
 * --------------------------------------------------------------------- */

/**
 * Read the six temperature-calibration bytes from NVM (0x88–0x8D) and
 * configure the sensor for normal-mode operation (ctrl_meas = 0x27).
 * Call once after power-on before reading temperature.
 */
I2C_Status bmp280_read_calibration(I2C_Regs *i2c, uint8_t addr7,
                                   BMP280_CalibTemp *cal);

/**
 * Read the 20-bit raw temperature register, apply the BMP280 integer
 * compensation formula, and return the result in units of 0.01 °C
 * (e.g. 2534 → 25.34 °C).
 *
 * Returns INT32_MIN on any I2C error.
 */
int32_t bmp280_read_temp_compensated(I2C_Regs *i2c, uint8_t addr7,
                                     const BMP280_CalibTemp *cal);

#endif /* I2C_H */
