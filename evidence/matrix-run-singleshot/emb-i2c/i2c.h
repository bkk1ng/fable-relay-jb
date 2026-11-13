/*
 * i2c.h — Polling I2C master, register-level, STM32F4 (Cortex-M4)
 *
 * No HAL, no CMSIS device headers.  Peripheral register layout and
 * bit definitions are defined here directly.
 *
 * Assumes APB1 drives the I2C clock.  Call i2c_init() with the actual
 * APB1 frequency; every other helper is stateless and re-entrant when
 * called on different bus instances.
 */

#ifndef I2C_H
#define I2C_H

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Register map                                                         */
/* ------------------------------------------------------------------ */

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

#define I2C1_BASE   0x40005400U
#define I2C2_BASE   0x40005800U
#define I2C3_BASE   0x40005C00U

#define I2C1    ((I2C_Regs *)I2C1_BASE)
#define I2C2    ((I2C_Regs *)I2C2_BASE)
#define I2C3    ((I2C_Regs *)I2C3_BASE)

/* CR1 */
#define I2C_CR1_PE          (1U <<  0)
#define I2C_CR1_START       (1U <<  8)
#define I2C_CR1_STOP        (1U <<  9)
#define I2C_CR1_ACK         (1U << 10)
#define I2C_CR1_POS         (1U << 11)
#define I2C_CR1_SWRST       (1U << 15)

/* CR2 */
#define I2C_CR2_FREQ_MASK   0x3FU

/* SR1 */
#define I2C_SR1_SB          (1U <<  0)
#define I2C_SR1_ADDR        (1U <<  1)
#define I2C_SR1_BTF         (1U <<  2)
#define I2C_SR1_RXNE        (1U <<  6)
#define I2C_SR1_TXE         (1U <<  7)
#define I2C_SR1_BERR        (1U <<  8)
#define I2C_SR1_ARLO        (1U <<  9)
#define I2C_SR1_AF          (1U << 10)
#define I2C_SR1_OVR         (1U << 11)
#define I2C_SR1_TIMEOUT     (1U << 14)

/* SR2 */
#define I2C_SR2_BUSY        (1U <<  1)

/* CCR */
#define I2C_CCR_FS          (1U << 15)
#define I2C_CCR_DUTY        (1U << 14)
#define I2C_CCR_CCR_MASK    0xFFFU

/* ------------------------------------------------------------------ */
/* Error codes                                                          */
/* ------------------------------------------------------------------ */

typedef enum {
    I2C_OK                   =  0,
    I2C_ERR_TIMEOUT          = -1,
    I2C_ERR_ARBITRATION_LOST = -2,
    I2C_ERR_BUS_ERROR        = -3,
    I2C_ERR_NACK             = -4,
    I2C_ERR_OVERRUN          = -5,
} I2C_Status;

/* ------------------------------------------------------------------ */
/* Timeout                                                              */
/*                                                                      */
/* Busy-wait loop iteration count.  At 168 MHz a single iteration is   */
/* ~6 ns; 100 000 covers ~600 µs, enough for a full byte at 10 kHz.   */
/* Adjust to match SYSCLK if needed.                                    */
/* ------------------------------------------------------------------ */

#ifndef I2C_TIMEOUT
#define I2C_TIMEOUT  100000U
#endif

/* ------------------------------------------------------------------ */
/* Low-level helpers (7-bit addressing)                                 */
/* ------------------------------------------------------------------ */

/* Configure CCR, TRISE, and enable the peripheral.
   pclk_mhz : APB1 clock in MHz (2–50).
   speed_hz : 100000 for standard mode, 400000 for fast mode.         */
I2C_Status i2c_init(I2C_Regs *i2c, uint32_t pclk_mhz, uint32_t speed_hz);

/* Generate START (or repeated START).  Waits for SB flag.            */
I2C_Status i2c_start(I2C_Regs *i2c);

/* Send 7-bit address + R/W bit and clear ADDR by reading SR1+SR2.
   rw: 0 = write, 1 = read.                                           */
I2C_Status i2c_send_addr(I2C_Regs *i2c, uint8_t addr7, uint8_t rw);

/* Wait for TXE then load one byte into DR.                            */
I2C_Status i2c_write_byte(I2C_Regs *i2c, uint8_t byte);

/* Read one byte after RXNE.
   ack=1: peripheral ACKs (more bytes follow).
   ack=0: peripheral NACKs and schedules STOP (last byte of a burst).  */
I2C_Status i2c_read_byte(I2C_Regs *i2c, uint8_t *out, uint8_t ack);

/* Generate STOP.                                                       */
void       i2c_stop(I2C_Regs *i2c);

/* ------------------------------------------------------------------ */
/* Composite helpers                                                    */
/* ------------------------------------------------------------------ */

/* Send START, address, len bytes, BTF, STOP.                          */
I2C_Status i2c_write(I2C_Regs *i2c, uint8_t addr7,
                     const uint8_t *buf, size_t len);

/* Send START, address, receive len bytes, STOP.
   Handles the 1-byte special case (ACK must be cleared before ADDR). */
I2C_Status i2c_read(I2C_Regs *i2c, uint8_t addr7,
                    uint8_t *buf, size_t len);

/* Write wbuf, repeated START, read rbuf.
   The standard register-read sequence: write register address, then
   read back rlen bytes without releasing the bus between the two.    */
I2C_Status i2c_write_read(I2C_Regs *i2c, uint8_t addr7,
                           const uint8_t *wbuf, size_t wlen,
                           uint8_t *rbuf,       size_t rlen);

/* ------------------------------------------------------------------ */
/* BMP280 temperature driver                                            */
/* ------------------------------------------------------------------ */

#define BMP280_ADDR_LOW   0x76U   /* SDO pulled low  */
#define BMP280_ADDR_HIGH  0x77U   /* SDO pulled high */

/* Temperature calibration words (datasheet Table 17, section 4.2.2). */
typedef struct {
    uint16_t dig_T1;   /* unsigned */
    int16_t  dig_T2;   /* signed   */
    int16_t  dig_T3;   /* signed   */
} BMP280_CalibTemp;

/* Read dig_T1/T2/T3 from calibration registers 0x88–0x8D.            */
I2C_Status bmp280_read_calibration(I2C_Regs *i2c, uint8_t addr7,
                                   BMP280_CalibTemp *cal);

/* Trigger a forced-mode conversion, poll until complete, read the
   20-bit raw ADC value, and apply the BMP280 integer compensation
   formula (datasheet section 4.2.3).
   out_centideg: result in hundredths of °C  (5123 → 51.23 °C).      */
I2C_Status bmp280_read_temp_compensated(I2C_Regs *i2c, uint8_t addr7,
                                        const BMP280_CalibTemp *cal,
                                        int32_t *out_centideg);

#endif /* I2C_H */
