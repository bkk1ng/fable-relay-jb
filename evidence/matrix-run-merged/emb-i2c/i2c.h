/*
 * i2c.h — Polling I2C master, STM32F4 (Cortex-M4), register-level, no HAL.
 *
 * Supports I2C1/2/3, 7-bit addressing.
 * All flag-wait calls time out and return an error code.
 */

#ifndef I2C_H
#define I2C_H

#include <stddef.h>
#include <stdint.h>

/* --------------------------------------------------------------------------
 * Error codes
 * -------------------------------------------------------------------------- */
typedef enum {
    I2C_OK            = 0,
    I2C_ERR_TIMEOUT   = 1,   /* flag-wait loop expired            */
    I2C_ERR_NACK      = 2,   /* slave did not acknowledge         */
    I2C_ERR_BUS       = 3,   /* BERR or ARLO set                  */
    I2C_ERR_ARG       = 4,   /* bad argument (null pointer, etc.) */
} i2c_err_t;

/* --------------------------------------------------------------------------
 * Peripheral selection
 * -------------------------------------------------------------------------- */
typedef enum {
    I2C_1 = 0,
    I2C_2 = 1,
    I2C_3 = 2,
} i2c_num_t;

/* --------------------------------------------------------------------------
 * Low-level primitives
 *
 * These operate on the bus directly and do not issue start/stop on their
 * own. Use the composite helpers unless you need manual sequencing.
 * -------------------------------------------------------------------------- */

/* Issue a START (or repeated-START) condition and wait for SB. */
i2c_err_t i2c_start(i2c_num_t num);

/* Issue a STOP condition and wait until the bus-free flag clears. */
i2c_err_t i2c_stop(i2c_num_t num);

/*
 * Send the 7-bit address + R/W bit and wait for ADDR.
 *   rw = 0  → write
 *   rw = 1  → read
 * Clears ADDR by reading SR1 then SR2 before returning.
 */
i2c_err_t i2c_send_addr(i2c_num_t num, uint8_t addr7, uint8_t rw);

/* Wait for TXE, then write one byte to DR. */
i2c_err_t i2c_write_byte(i2c_num_t num, uint8_t data);

/*
 * Wait for RXNE, then read one byte from DR.
 *   ack = 1  → ACK the byte (more bytes to follow)
 *   ack = 0  → NACK (this is the last byte)
 * Set ack=0 *before* clearing ADDR for the final byte of a 1-byte transfer.
 */
i2c_err_t i2c_read_byte(i2c_num_t num, uint8_t *data, uint8_t ack);

/* --------------------------------------------------------------------------
 * Composite helpers
 * -------------------------------------------------------------------------- */

/*
 * Write `len` bytes to slave `addr7`.
 * START — ADDR(W) — data[0..len-1] — STOP
 */
i2c_err_t i2c_write(i2c_num_t num, uint8_t addr7,
                    const uint8_t *buf, size_t len);

/*
 * Read `len` bytes from slave `addr7`.
 * START — ADDR(R) — data[0..len-1] — STOP
 * Handles the STM32F4 1-byte and 2-byte receive quirks (POS, pre-ADDR NACK).
 */
i2c_err_t i2c_read(i2c_num_t num, uint8_t addr7,
                   uint8_t *buf, size_t len);

/*
 * Write then read with a repeated-START between phases (no STOP between).
 * START — ADDR(W) — wbuf[0..wlen-1] — rSTART — ADDR(R) — rbuf[0..rlen-1] — STOP
 */
i2c_err_t i2c_write_read(i2c_num_t num, uint8_t addr7,
                          const uint8_t *wbuf, size_t wlen,
                          uint8_t *rbuf,       size_t rlen);

/* --------------------------------------------------------------------------
 * BMP280 helpers
 *
 * Call bmp280_read_calibration() once after power-on before reading
 * temperature. The calibration values are stored in a static struct
 * indexed by I2C peripheral number, so each bus can have its own sensor.
 * -------------------------------------------------------------------------- */

/*
 * Read the factory calibration coefficients from BMP280 OTP registers
 * (0x88–0x8D: dig_T1/T2/T3).
 */
i2c_err_t bmp280_read_calibration(i2c_num_t num, uint8_t addr7);

/*
 * Read the raw ADC temperature, apply Bosch's integer compensation formula,
 * and write the result to *temp_centideg.
 *
 * Result unit: 1/100 °C  (e.g. 2345 → 23.45 °C)
 *
 * Requires bmp280_read_calibration() to have been called first.
 */
i2c_err_t bmp280_read_temp_compensated(i2c_num_t num, uint8_t addr7,
                                        int32_t *temp_centideg);

#endif /* I2C_H */
