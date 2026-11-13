/**
 * i2c.c — Polling I2C master, register-level, STM32F4 Cortex-M4.
 *
 * Target: STM32F4 family.  No HAL, no CMSIS headers.
 * Peripheral registers are accessed through volatile pointers derived
 * from the base addresses defined in i2c.h.
 *
 * BMP280 helpers at the bottom implement the Bosch integer compensation
 * formula from the BMP280 datasheet section 4.2.3.
 */

#include "i2c.h"

/* ------------------------------------------------------------------
 * Register accessor macros
 * ------------------------------------------------------------------ */
#define REG(base, off)  (*((volatile uint32_t *)((base) + (off))))

#define CR1(d)    REG((d)->base, I2C_CR1_OFF)
#define CR2(d)    REG((d)->base, I2C_CR2_OFF)
#define DR(d)     REG((d)->base, I2C_DR_OFF)
#define SR1(d)    REG((d)->base, I2C_SR1_OFF)
#define SR2(d)    REG((d)->base, I2C_SR2_OFF)
#define CCR(d)    REG((d)->base, I2C_CCR_OFF)
#define TRISE(d)  REG((d)->base, I2C_TRISE_OFF)

/* ------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------ */

/** Map SR1 error bits to an i2c_err_t. Returns I2C_OK if none set. */
static i2c_err_t check_errors(const i2c_t *dev)
{
    uint32_t sr1 = SR1(dev);
    if (sr1 & I2C_SR1_ARLO)    return I2C_ERR_ARBITRATION;
    if (sr1 & I2C_SR1_BERR)    return I2C_ERR_BUS;
    if (sr1 & I2C_SR1_AF)      return I2C_ERR_ACK;
    if (sr1 & I2C_SR1_OVR)     return I2C_ERR_OVERRUN;
    if (sr1 & I2C_SR1_TIMEOUT) return I2C_ERR_TIMEOUT;
    return I2C_OK;
}

/** Clear all SR1 error flags. */
static void clear_errors(i2c_t *dev)
{
    /* Write 0 to the error bits; writing 1 to reserved bits is unsafe,
     * so AND away only the error flags. */
    SR1(dev) &= ~I2C_SR1_ERR_MASK;
}

/**
 * Wait for a specific SR1 bit to become set.
 *
 * Returns I2C_ERR_TIMEOUT if the bit does not set within I2C_TIMEOUT
 * iterations, or the first bus error detected.
 */
static i2c_err_t wait_flag(i2c_t *dev, uint32_t flag)
{
    uint32_t timeout = I2C_TIMEOUT;
    while (!(SR1(dev) & flag)) {
        i2c_err_t e = check_errors(dev);
        if (e != I2C_OK) {
            clear_errors(dev);
            return e;
        }
        if (--timeout == 0U) {
            return I2C_ERR_TIMEOUT;
        }
    }
    return I2C_OK;
}

/**
 * Wait for the BUSY flag in SR2 to clear (bus idle).
 * Used after STOP to ensure the line is free.
 */
static i2c_err_t wait_not_busy(i2c_t *dev)
{
    uint32_t timeout = I2C_TIMEOUT;
    while (REG(dev->base, I2C_SR2_OFF) & (1U << 1)) { /* BUSY = SR2 bit1 */
        if (--timeout == 0U) {
            return I2C_ERR_TIMEOUT;
        }
    }
    return I2C_OK;
}

/* ------------------------------------------------------------------
 * i2c_init
 * ------------------------------------------------------------------ */
i2c_err_t i2c_init(i2c_t *dev, uint32_t base, uint32_t pclk_mhz,
                    uint32_t speed_khz)
{
    if (!dev || pclk_mhz < 2U || pclk_mhz > 50U ||
        (speed_khz != 100U && speed_khz != 400U)) {
        return I2C_ERR_PARAM;
    }

    dev->base = base;

    /* Software reset to clear any stuck state. */
    CR1(dev) |=  I2C_CR1_SWRST;
    CR1(dev) &= ~I2C_CR1_SWRST;

    /* Disable peripheral while configuring. */
    CR1(dev) &= ~I2C_CR1_PE;

    /* CR2: APB1 clock in MHz. */
    CR2(dev) = (CR2(dev) & ~I2C_CR2_FREQ_MASK) | (pclk_mhz & I2C_CR2_FREQ_MASK);

    if (speed_khz == 100U) {
        /* Standard mode: CCR = Fpclk / (2 * 100 kHz) */
        uint32_t ccr_val = (pclk_mhz * 1000U) / (2U * 100U);
        if (ccr_val < 4U) ccr_val = 4U;
        CCR(dev) = ccr_val;                /* FS=0, standard mode        */

        /* TRISE: (Fpclk_MHz * max_rise_ns / 1000) + 1
         * Standard mode max rise time = 1000 ns */
        TRISE(dev) = pclk_mhz + 1U;
    } else {
        /* Fast mode (400 kHz), duty cycle 2:1 (I2C_CCR_DUTY = 0):
         * CCR = Fpclk / (3 * 400 kHz) */
        uint32_t ccr_val = (pclk_mhz * 1000U) / (3U * 400U);
        if (ccr_val < 1U) ccr_val = 1U;
        CCR(dev) = I2C_CCR_FS | ccr_val;

        /* Fast mode max rise time = 300 ns */
        TRISE(dev) = ((pclk_mhz * 300U) / 1000U) + 1U;
    }

    /* Enable peripheral and ACK. */
    CR1(dev) = I2C_CR1_PE | I2C_CR1_ACK;

    return I2C_OK;
}

/* ------------------------------------------------------------------
 * i2c_start
 * ------------------------------------------------------------------ */
i2c_err_t i2c_start(i2c_t *dev)
{
    /* Enable ACK before each transaction. */
    CR1(dev) |= I2C_CR1_ACK;

    /* Request START. */
    CR1(dev) |= I2C_CR1_START;

    /* Wait for SB (start condition generated). */
    return wait_flag(dev, I2C_SR1_SB);
}

/* ------------------------------------------------------------------
 * i2c_stop
 * ------------------------------------------------------------------ */
i2c_err_t i2c_stop(i2c_t *dev)
{
    CR1(dev) |= I2C_CR1_STOP;
    return wait_not_busy(dev);
}

/* ------------------------------------------------------------------
 * i2c_send_addr
 * ------------------------------------------------------------------ */
i2c_err_t i2c_send_addr(i2c_t *dev, uint8_t addr7, bool read)
{
    i2c_err_t err;

    /* Writing DR clears SB and sends the address byte. */
    DR(dev) = (uint32_t)((addr7 << 1) | (read ? 1U : 0U));

    /* Wait for ADDR flag: address sent, ACK received (or AF on NACK). */
    err = wait_flag(dev, I2C_SR1_ADDR);
    if (err != I2C_OK) {
        return err;
    }

    /* Clear ADDR by reading SR1 then SR2.
     * For N=1 read the caller is responsible for disabling ACK and
     * setting STOP BEFORE this clear; that sequencing is managed in
     * i2c_read() rather than here to keep this primitive simple. */
    (void)SR1(dev);
    (void)SR2(dev);

    return I2C_OK;
}

/* ------------------------------------------------------------------
 * i2c_write_byte
 * ------------------------------------------------------------------ */
i2c_err_t i2c_write_byte(i2c_t *dev, uint8_t data)
{
    i2c_err_t err;

    /* Wait for TXE: DR empty, safe to load next byte. */
    err = wait_flag(dev, I2C_SR1_TXE);
    if (err != I2C_OK) return err;

    DR(dev) = data;

    /* Wait for BTF: shift register and DR both empty — byte is on the wire. */
    err = wait_flag(dev, I2C_SR1_BTF);
    return err;
}

/* ------------------------------------------------------------------
 * i2c_read_byte
 * ------------------------------------------------------------------ */
i2c_err_t i2c_read_byte(i2c_t *dev, uint8_t *data, bool last)
{
    i2c_err_t err;

    if (last) {
        /* For the last byte we must clear ACK so the peripheral sends
         * a NACK after this byte.  Caller sets STOP after return. */
        CR1(dev) &= ~I2C_CR1_ACK;
    }

    err = wait_flag(dev, I2C_SR1_RXNE);
    if (err != I2C_OK) return err;

    *data = (uint8_t)DR(dev);
    return I2C_OK;
}

/* ------------------------------------------------------------------
 * i2c_write
 * ------------------------------------------------------------------ */
i2c_err_t i2c_write(i2c_t *dev, uint8_t addr7,
                     const uint8_t *buf, size_t len)
{
    i2c_err_t err;

    if (!dev || !buf || len == 0U) return I2C_ERR_PARAM;

    err = i2c_start(dev);
    if (err != I2C_OK) return err;

    err = i2c_send_addr(dev, addr7, false);
    if (err != I2C_OK) { (void)i2c_stop(dev); return err; }

    for (size_t i = 0; i < len; i++) {
        err = i2c_write_byte(dev, buf[i]);
        if (err != I2C_OK) { (void)i2c_stop(dev); return err; }
    }

    return i2c_stop(dev);
}

/* ------------------------------------------------------------------
 * i2c_read
 *
 * The STM32F4 close-transfer sequence differs by byte count:
 *
 *   N == 1: disable ACK, send STOP, clear ADDR, read DR.
 *   N == 2: set POS, disable ACK before clearing ADDR, wait BTF,
 *           send STOP, read DR twice.
 *   N  > 2: ACK each byte up to N-3, then disable ACK at N-2,
 *           wait BTF at N-2/N-1 boundary, send STOP, drain last two.
 *
 * Reference: STM32F4 Reference Manual RM0090, section 27.3.3.
 * ------------------------------------------------------------------ */
i2c_err_t i2c_read(i2c_t *dev, uint8_t addr7,
                    uint8_t *buf, size_t len)
{
    i2c_err_t err;

    if (!dev || !buf || len == 0U) return I2C_ERR_PARAM;

    err = i2c_start(dev);
    if (err != I2C_OK) return err;

    if (len == 1U) {
        /* Disable ACK before clearing ADDR so the NACK goes out in time. */
        CR1(dev) &= ~I2C_CR1_ACK;

        /* Clear ADDR. */
        (void)SR1(dev);
        (void)SR2(dev);

        /* Program STOP immediately after ADDR clear. */
        CR1(dev) |= I2C_CR1_STOP;

        err = wait_flag(dev, I2C_SR1_RXNE);
        if (err != I2C_OK) return err;

        buf[0] = (uint8_t)DR(dev);
        return wait_not_busy(dev);
    }

    if (len == 2U) {
        /* Set POS so ACK/NACK applies to the next-next byte. */
        CR1(dev) |= I2C_CR1_POS;
        CR1(dev) &= ~I2C_CR1_ACK;

        /* Clear ADDR. */
        (void)SR1(dev);
        (void)SR2(dev);

        /* Wait for BTF: both bytes are now in DR and the shift register. */
        err = wait_flag(dev, I2C_SR1_BTF);
        if (err != I2C_OK) { CR1(dev) &= ~I2C_CR1_POS; (void)i2c_stop(dev); return err; }

        CR1(dev) |= I2C_CR1_STOP;

        buf[0] = (uint8_t)DR(dev);
        buf[1] = (uint8_t)DR(dev);

        CR1(dev) &= ~I2C_CR1_POS;
        return wait_not_busy(dev);
    }

    /* N > 2: send address with ACK enabled (already on from i2c_start). */
    err = i2c_send_addr(dev, addr7, true);
    if (err != I2C_OK) { (void)i2c_stop(dev); return err; }

    for (size_t i = 0; i < len; i++) {
        if (i == len - 3U) {
            /* Wait for BTF at position N-3: DR=N-2, SR=N-3. */
            err = wait_flag(dev, I2C_SR1_BTF);
            if (err != I2C_OK) { (void)i2c_stop(dev); return err; }

            /* Disable ACK — NACK will be sent for byte N-1. */
            CR1(dev) &= ~I2C_CR1_ACK;

            buf[i] = (uint8_t)DR(dev);
            i++;

            /* Wait for BTF again: DR=N-1, SR=N-2. */
            err = wait_flag(dev, I2C_SR1_BTF);
            if (err != I2C_OK) { (void)i2c_stop(dev); return err; }

            /* Send STOP while DR and SR hold the last two bytes. */
            CR1(dev) |= I2C_CR1_STOP;

            buf[i] = (uint8_t)DR(dev);
            i++;
            buf[i] = (uint8_t)DR(dev);
            break;
        }

        err = wait_flag(dev, I2C_SR1_RXNE);
        if (err != I2C_OK) { (void)i2c_stop(dev); return err; }

        buf[i] = (uint8_t)DR(dev);
    }

    return wait_not_busy(dev);
}

/* ------------------------------------------------------------------
 * i2c_write_read
 * ------------------------------------------------------------------ */
i2c_err_t i2c_write_read(i2c_t *dev,
                           uint8_t        addr7,
                           const uint8_t *wbuf, size_t wlen,
                           uint8_t       *rbuf, size_t rlen)
{
    i2c_err_t err;

    if (!dev || !wbuf || wlen == 0U || !rbuf || rlen == 0U)
        return I2C_ERR_PARAM;

    /* Phase 1: write (START | ADDR+W | reg bytes, no STOP). */
    err = i2c_start(dev);
    if (err != I2C_OK) return err;

    err = i2c_send_addr(dev, addr7, false);
    if (err != I2C_OK) { (void)i2c_stop(dev); return err; }

    for (size_t i = 0; i < wlen; i++) {
        err = i2c_write_byte(dev, wbuf[i]);
        if (err != I2C_OK) { (void)i2c_stop(dev); return err; }
    }

    /* Phase 2: repeated START, then read using the full N-aware sequence. */
    if (rlen == 1U) {
        /* Must handle N=1 entirely here — cannot delegate to i2c_read()
         * because that would issue its own START rather than a repeated one. */
        CR1(dev) &= ~I2C_CR1_ACK;
        CR1(dev) |=  I2C_CR1_START;

        err = wait_flag(dev, I2C_SR1_SB);
        if (err != I2C_OK) return err;

        DR(dev) = (uint32_t)((addr7 << 1) | 1U);

        /* Wait for ADDR. */
        err = wait_flag(dev, I2C_SR1_ADDR);
        if (err != I2C_OK) { (void)i2c_stop(dev); return err; }

        (void)SR1(dev);
        (void)SR2(dev);

        CR1(dev) |= I2C_CR1_STOP;

        err = wait_flag(dev, I2C_SR1_RXNE);
        if (err != I2C_OK) return err;

        rbuf[0] = (uint8_t)DR(dev);
        return wait_not_busy(dev);
    }

    if (rlen == 2U) {
        CR1(dev) |=  I2C_CR1_POS;
        CR1(dev) &= ~I2C_CR1_ACK;
        CR1(dev) |=  I2C_CR1_START;

        err = wait_flag(dev, I2C_SR1_SB);
        if (err != I2C_OK) { CR1(dev) &= ~I2C_CR1_POS; return err; }

        DR(dev) = (uint32_t)((addr7 << 1) | 1U);

        err = wait_flag(dev, I2C_SR1_ADDR);
        if (err != I2C_OK) { CR1(dev) &= ~I2C_CR1_POS; (void)i2c_stop(dev); return err; }

        (void)SR1(dev);
        (void)SR2(dev);

        err = wait_flag(dev, I2C_SR1_BTF);
        if (err != I2C_OK) { CR1(dev) &= ~I2C_CR1_POS; (void)i2c_stop(dev); return err; }

        CR1(dev) |= I2C_CR1_STOP;
        rbuf[0] = (uint8_t)DR(dev);
        rbuf[1] = (uint8_t)DR(dev);
        CR1(dev) &= ~I2C_CR1_POS;
        return wait_not_busy(dev);
    }

    /* N > 2 repeated-start path. */
    CR1(dev) |= I2C_CR1_START;

    err = wait_flag(dev, I2C_SR1_SB);
    if (err != I2C_OK) return err;

    DR(dev) = (uint32_t)((addr7 << 1) | 1U);

    err = wait_flag(dev, I2C_SR1_ADDR);
    if (err != I2C_OK) { (void)i2c_stop(dev); return err; }

    (void)SR1(dev);
    (void)SR2(dev);   /* ADDR cleared, ACK still on */

    for (size_t i = 0; i < rlen; i++) {
        if (i == rlen - 3U) {
            err = wait_flag(dev, I2C_SR1_BTF);
            if (err != I2C_OK) { (void)i2c_stop(dev); return err; }

            CR1(dev) &= ~I2C_CR1_ACK;
            rbuf[i] = (uint8_t)DR(dev);
            i++;

            err = wait_flag(dev, I2C_SR1_BTF);
            if (err != I2C_OK) { (void)i2c_stop(dev); return err; }

            CR1(dev) |= I2C_CR1_STOP;
            rbuf[i] = (uint8_t)DR(dev);
            i++;
            rbuf[i] = (uint8_t)DR(dev);
            break;
        }

        err = wait_flag(dev, I2C_SR1_RXNE);
        if (err != I2C_OK) { (void)i2c_stop(dev); return err; }

        rbuf[i] = (uint8_t)DR(dev);
    }

    return wait_not_busy(dev);
}

/* ==================================================================
 * BMP280 helpers
 * ================================================================== */

/* ------------------------------------------------------------------
 * BMP280 register map (temperature-relevant subset)
 * ------------------------------------------------------------------ */
#define BMP280_REG_CALIB_T1     0x88U   /* 0x88–0x8D: 3 × uint16/int16  */
#define BMP280_REG_STATUS       0xF3U
#define BMP280_REG_CTRL_MEAS    0xF4U
#define BMP280_REG_PRESS_MSB    0xF7U   /* 0xF7–0xFC: press + temp raw   */

/* ctrl_meas field values */
#define BMP280_OSRS_T_1X        (1U << 5)   /* temperature oversampling ×1 */
#define BMP280_OSRS_P_SKIP      (0U << 2)   /* pressure skipped            */
#define BMP280_MODE_FORCED      0x01U       /* forced mode: one shot       */

/* status bit */
#define BMP280_STATUS_MEASURING (1U << 3)

/* ------------------------------------------------------------------
 * bmp280_read_calibration
 * ------------------------------------------------------------------ */
i2c_err_t bmp280_read_calibration(i2c_t *dev, uint8_t addr7,
                                    bmp280_calib_t *cal)
{
    i2c_err_t err;
    uint8_t   raw[6];
    uint8_t   reg = BMP280_REG_CALIB_T1;

    if (!dev || !cal) return I2C_ERR_PARAM;

    err = i2c_write_read(dev, addr7, &reg, 1U, raw, 6U);
    if (err != I2C_OK) return err;

    /* Calibration words are stored little-endian.
     * T1 is unsigned; T2 and T3 are signed two's complement. */
    cal->dig_T1 = (uint16_t)((uint16_t)raw[1] << 8) | raw[0];
    cal->dig_T2 = (int16_t)(((uint16_t)raw[3] << 8) | raw[2]);
    cal->dig_T3 = (int16_t)(((uint16_t)raw[5] << 8) | raw[4]);

    return I2C_OK;
}

/* ------------------------------------------------------------------
 * bmp280_read_temp_compensated
 *
 * Integer compensation formula from BMP280 datasheet §4.2.3.
 * The intermediate t_fine value is computed but not exposed; extend
 * the signature if pressure compensation is added later.
 * ------------------------------------------------------------------ */
i2c_err_t bmp280_read_temp_compensated(i2c_t *dev,
                                         uint8_t               addr7,
                                         const bmp280_calib_t *cal,
                                         int32_t              *temp_cdeg)
{
    i2c_err_t err;
    uint8_t   ctrl;
    uint8_t   raw[6];       /* 0xF7–0xFC: press[2:0] + temp[2:0]       */
    uint8_t   reg;
    uint32_t  timeout;

    if (!dev || !cal || !temp_cdeg) return I2C_ERR_PARAM;

    /* Configure: temperature ×1 oversampling, pressure skipped, forced mode.*/
    ctrl = (uint8_t)(BMP280_OSRS_T_1X | BMP280_OSRS_P_SKIP | BMP280_MODE_FORCED);
    {
        uint8_t wbuf[2] = { BMP280_REG_CTRL_MEAS, ctrl };
        err = i2c_write(dev, addr7, wbuf, 2U);
        if (err != I2C_OK) return err;
    }

    /* Poll status register until measuring bit clears (conversion done).
     * Typical temperature-only conversion at ×1: ~2 ms. */
    timeout = I2C_TIMEOUT * 20U;    /* generous: ~40 ms at 168 MHz      */
    for (;;) {
        uint8_t status_val = 0U;
        reg = BMP280_REG_STATUS;
        err = i2c_write_read(dev, addr7, &reg, 1U, &status_val, 1U);
        if (err != I2C_OK) return err;

        if (!(status_val & BMP280_STATUS_MEASURING)) break;

        if (--timeout == 0U) return I2C_ERR_TIMEOUT;
    }

    /* Read 6 bytes starting at 0xF7 (pressure MSB through temp XLSB).
     * We only use the temperature bytes [3..5] but burst-reading keeps
     * the transaction simple and matches the datasheet recommendation. */
    reg = BMP280_REG_PRESS_MSB;
    err = i2c_write_read(dev, addr7, &reg, 1U, raw, 6U);
    if (err != I2C_OK) return err;

    /* Reconstruct 20-bit raw temperature:
     *   raw[3] = temp_msb, raw[4] = temp_lsb, raw[5] = temp_xlsb (bits[7:4])
     */
    int32_t adc_T = (int32_t)(
        ((uint32_t)raw[3] << 12) |
        ((uint32_t)raw[4] <<  4) |
        ((uint32_t)raw[5] >>  4)
    );

    /* Bosch integer compensation formula (verbatim from datasheet §4.2.3).
     * Uses 32-bit arithmetic only; t_fine carries the temperature in a
     * scaled form used by the pressure compensation formula if needed. */
    int32_t var1, var2, t_fine;

    var1 = ((((adc_T >> 3) - ((int32_t)cal->dig_T1 << 1))) *
             (int32_t)cal->dig_T2) >> 11;

    var2 = (((((adc_T >> 4) - (int32_t)cal->dig_T1) *
              ((adc_T >> 4) - (int32_t)cal->dig_T1)) >> 12) *
              (int32_t)cal->dig_T3) >> 14;

    t_fine = var1 + var2;

    /* Temperature in hundredths of degrees Celsius (e.g. 2512 = 25.12 °C). */
    *temp_cdeg = (t_fine * 5 + 128) >> 8;

    return I2C_OK;
}
