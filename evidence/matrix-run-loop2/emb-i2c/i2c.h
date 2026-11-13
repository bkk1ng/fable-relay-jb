/**
 * i2c.h — Polling I2C master, register-level, STM32F4 Cortex-M4.
 *
 * No HAL, no CMSIS headers. Targets the STM32F4 I2C peripheral directly.
 * Caller is responsible for enabling peripheral and GPIO clocks before i2c_init.
 */
#ifndef I2C_H
#define I2C_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ------------------------------------------------------------------
 * Peripheral base addresses (STM32F4)
 * ------------------------------------------------------------------ */
#define I2C1_BASE   UINT32_C(0x40005400)
#define I2C2_BASE   UINT32_C(0x40005800)
#define I2C3_BASE   UINT32_C(0x40005C00)

/* ------------------------------------------------------------------
 * Register offsets
 * ------------------------------------------------------------------ */
#define I2C_CR1_OFF     0x00U
#define I2C_CR2_OFF     0x04U
#define I2C_OAR1_OFF    0x08U
#define I2C_OAR2_OFF    0x0CU
#define I2C_DR_OFF      0x10U
#define I2C_SR1_OFF     0x14U
#define I2C_SR2_OFF     0x18U
#define I2C_CCR_OFF     0x1CU
#define I2C_TRISE_OFF   0x20U

/* ------------------------------------------------------------------
 * CR1 bits
 * ------------------------------------------------------------------ */
#define I2C_CR1_PE      (1U << 0)   /* Peripheral enable              */
#define I2C_CR1_START   (1U << 8)   /* Start generation               */
#define I2C_CR1_STOP    (1U << 9)   /* Stop generation                */
#define I2C_CR1_ACK     (1U << 10)  /* Acknowledge enable             */
#define I2C_CR1_POS     (1U << 11)  /* ACK/PEC position (N=2 reads)   */
#define I2C_CR1_SWRST   (1U << 15)  /* Software reset                 */

/* ------------------------------------------------------------------
 * CR2 bits
 * ------------------------------------------------------------------ */
#define I2C_CR2_FREQ_MASK   0x3FU   /* APB1 peripheral clock in MHz   */

/* ------------------------------------------------------------------
 * SR1 bits
 * ------------------------------------------------------------------ */
#define I2C_SR1_SB      (1U << 0)   /* Start condition generated      */
#define I2C_SR1_ADDR    (1U << 1)   /* Address sent/matched           */
#define I2C_SR1_BTF     (1U << 2)   /* Byte transfer finished         */
#define I2C_SR1_RXNE    (1U << 6)   /* RX data register not empty     */
#define I2C_SR1_TXE     (1U << 7)   /* TX data register empty         */
#define I2C_SR1_BERR    (1U << 8)   /* Bus error                      */
#define I2C_SR1_ARLO    (1U << 9)   /* Arbitration lost               */
#define I2C_SR1_AF      (1U << 10)  /* Acknowledge failure            */
#define I2C_SR1_OVR     (1U << 11)  /* Overrun / underrun             */
#define I2C_SR1_TIMEOUT (1U << 14)  /* Timeout / Tlow error           */

#define I2C_SR1_ERR_MASK \
    (I2C_SR1_BERR | I2C_SR1_ARLO | I2C_SR1_AF | I2C_SR1_OVR | I2C_SR1_TIMEOUT)

/* ------------------------------------------------------------------
 * CCR bits
 * ------------------------------------------------------------------ */
#define I2C_CCR_FS      (1U << 15)  /* Fast mode select (400 kHz)     */
#define I2C_CCR_DUTY    (1U << 14)  /* Fast mode Tlow/Thigh duty 16/9 */

/* ------------------------------------------------------------------
 * Timeout — loop iterations; tune for your CPU frequency.
 * At 168 MHz one iteration ≈ a few ns; 200 000 covers > 1 ms.
 * ------------------------------------------------------------------ */
#ifndef I2C_TIMEOUT
#define I2C_TIMEOUT     200000U
#endif

/* ------------------------------------------------------------------
 * Error codes
 * ------------------------------------------------------------------ */
typedef enum {
    I2C_OK              =  0,
    I2C_ERR_TIMEOUT     = -1,   /* Flag did not assert in time        */
    I2C_ERR_ARBITRATION = -2,   /* Arbitration lost (ARLO)            */
    I2C_ERR_BUS         = -3,   /* Bus error (BERR)                   */
    I2C_ERR_ACK         = -4,   /* No acknowledge from device (AF)    */
    I2C_ERR_OVERRUN     = -5,   /* Overrun / underrun                 */
    I2C_ERR_PARAM       = -6,   /* Bad argument                       */
} i2c_err_t;

/* ------------------------------------------------------------------
 * Device handle
 * ------------------------------------------------------------------ */
typedef struct {
    uint32_t base;  /* Peripheral base address */
} i2c_t;

/* ------------------------------------------------------------------
 * Init
 * ------------------------------------------------------------------ */
/**
 * Initialise the I2C peripheral in master mode.
 *
 * The caller must have already enabled the peripheral clock (RCC) and
 * configured the SDA/SCL GPIO pins to AF open-drain before calling this.
 *
 * @param dev        Handle to populate.
 * @param base        Peripheral base address (I2C1_BASE, I2C2_BASE, …).
 * @param pclk_mhz   APB1 clock frequency in MHz (2–50 for F4).
 * @param speed_khz  100 → standard mode, 400 → fast mode.
 * @return I2C_OK or I2C_ERR_PARAM if arguments are out of range.
 */
i2c_err_t i2c_init(i2c_t *dev, uint32_t base, uint32_t pclk_mhz,
                    uint32_t speed_khz);

/* ------------------------------------------------------------------
 * Primitive bus operations
 * ------------------------------------------------------------------ */
/** Assert a (re)start condition; waits for SB. */
i2c_err_t i2c_start(i2c_t *dev);

/** Assert a stop condition; waits for the bus to go idle. */
i2c_err_t i2c_stop(i2c_t *dev);

/**
 * Send the 7-bit address byte after a start.
 *
 * Clears the SB flag (by writing DR), then waits for ADDR.
 * Clears ADDR (SR1 read + SR2 read) before returning.
 *
 * @param addr7  7-bit device address (not pre-shifted).
 * @param read   true  → address byte = addr7<<1 | 1  (master-receiver)
 *               false → address byte = addr7<<1 | 0  (master-transmitter)
 */
i2c_err_t i2c_send_addr(i2c_t *dev, uint8_t addr7, bool read);

/** Write one byte to the bus; waits for TXE before and BTF after. */
i2c_err_t i2c_write_byte(i2c_t *dev, uint8_t data);

/**
 * Read one byte from the bus.
 *
 * Before calling for the last byte in a multi-byte transfer the caller
 * must have already cleared I2C_CR1_ACK so the peripheral NAKs the
 * byte being clocked in.  This function manages that for the N=1 and
 * N=2 cases internally (via i2c_read); do not call it stand-alone
 * without understanding the STM32F4 close-transfer sequence.
 *
 * @param last  true if this is the penultimate byte being handled
 *              before the caller issues STOP (clears ACK here).
 */
i2c_err_t i2c_read_byte(i2c_t *dev, uint8_t *data, bool last);

/* ------------------------------------------------------------------
 * Composite helpers
 * ------------------------------------------------------------------ */
/** Write len bytes to addr7; generates start and stop. */
i2c_err_t i2c_write(i2c_t *dev, uint8_t addr7,
                     const uint8_t *buf, size_t len);

/**
 * Read len bytes from addr7.
 * Handles the N=1, N=2, and N>2 close-transfer sequences correctly.
 */
i2c_err_t i2c_read(i2c_t *dev, uint8_t addr7,
                    uint8_t *buf, size_t len);

/**
 * Write wlen bytes, issue a repeated START, then read rlen bytes.
 * This is the standard register-read pattern used by most sensors:
 *   START | ADDR+W | reg | RSTART | ADDR+R | data… | STOP
 */
i2c_err_t i2c_write_read(i2c_t *dev,
                           uint8_t        addr7,
                           const uint8_t *wbuf, size_t wlen,
                           uint8_t       *rbuf, size_t rlen);

/* ------------------------------------------------------------------
 * BMP280 calibration storage (temperature only)
 * ------------------------------------------------------------------ */
typedef struct {
    uint16_t dig_T1;    /* unsigned */
    int16_t  dig_T2;    /* signed   */
    int16_t  dig_T3;    /* signed   */
} bmp280_calib_t;

/* ------------------------------------------------------------------
 * BMP280 helpers
 * ------------------------------------------------------------------ */
/**
 * Read the three temperature calibration words from BMP280
 * (registers 0x88–0x8D, little-endian 16-bit pairs).
 *
 * @param addr7  BMP280 I2C address (0x76 or 0x77).
 * @param cal    Output calibration struct.
 */
i2c_err_t bmp280_read_calibration(i2c_t *dev, uint8_t addr7,
                                    bmp280_calib_t *cal);

/**
 * Trigger a forced-mode single measurement, wait for completion,
 * read the 20-bit raw ADC value, and return the compensated
 * temperature using the Bosch integer formula.
 *
 * @param cal       Calibration data from bmp280_read_calibration.
 * @param temp_cdeg Output: temperature in hundredths of °C
 *                  (e.g. 2512 → 25.12 °C).
 */
i2c_err_t bmp280_read_temp_compensated(i2c_t *dev,
                                         uint8_t               addr7,
                                         const bmp280_calib_t *cal,
                                         int32_t              *temp_cdeg);

#endif /* I2C_H */
