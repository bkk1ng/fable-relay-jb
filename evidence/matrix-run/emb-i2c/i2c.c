/*
 * i2c.c — register-level polling I2C master, ARM Cortex-M4 / STM32F4
 *
 * See i2c.h for the register map, bit definitions, and API contracts.
 *
 * Peripheral setup (clocks, GPIO AF, CR2 FREQ, CCR, TRISE, CR1 PE) is the
 * caller's responsibility.  Every function here assumes the peripheral is
 * already enabled and the bus is in a known idle state before the first
 * i2c_start call.
 *
 * BMP280 register addresses (Bosch DS001 rev 1.19)
 *   Calibration : 0x88 – 0x9F  (24 bytes, little-endian pairs)
 *   ctrl_meas   : 0xF4         (osrs_t, osrs_p, mode)
 *   ADC_T       : 0xFA – 0xFC  (20-bit, MSB…LSB, bits[3:0] of 0xFC unused)
 */

#include "i2c.h"

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/*
 * clear_errors — write 0 to all error bits in SR1 that have w0c semantics
 * (BERR, ARLO, AF, OVR, PECERR, TIMEOUT, SMBALERT).
 * Called after detecting any SR1 error so the peripheral is left clean.
 */
static void clear_errors(I2C_Regs *i2c)
{
    i2c->SR1 &= ~(uint32_t)I2C_SR1_ERR_MASK;
}

/*
 * wait_flag — spin until (i2c->SR1 & flag) is nonzero or the timeout
 * counter expires.  Also checks for hardware error bits on every iteration.
 *
 * Returns I2C_OK when the flag is set.
 * Returns I2C_ERR_NACK / I2C_ERR_ARBLOST / I2C_ERR_BUS on a hardware error.
 * Returns I2C_ERR_TIMEOUT when the counter reaches zero without the flag.
 */
static I2C_Status wait_flag(I2C_Regs *i2c, uint32_t flag)
{
    uint32_t timeout = I2C_TIMEOUT;

    while (timeout--) {
        uint32_t sr1 = i2c->SR1;

        /* Check hardware error bits first */
        if (sr1 & I2C_SR1_AF) {
            clear_errors(i2c);
            return I2C_ERR_NACK;
        }
        if (sr1 & I2C_SR1_ARLO) {
            clear_errors(i2c);
            return I2C_ERR_ARBLOST;
        }
        if (sr1 & (I2C_SR1_BERR | I2C_SR1_OVR | I2C_SR1_TIMEOUT)) {
            clear_errors(i2c);
            return I2C_ERR_BUS;
        }

        if (sr1 & flag) {
            return I2C_OK;
        }
    }

    return I2C_ERR_TIMEOUT;
}

/*
 * wait_sr2_flag — same contract as wait_flag but reads SR2.
 * Used only for BUSY after STOP.
 */
static I2C_Status wait_sr2_clear(I2C_Regs *i2c, uint32_t flag)
{
    uint32_t timeout = I2C_TIMEOUT;

    while (timeout--) {
        if (!(i2c->SR2 & flag)) {
            return I2C_OK;
        }
    }

    return I2C_ERR_TIMEOUT;
}

/* =========================================================================
 * Low-level single-operation helpers
 * ========================================================================= */

I2C_Status i2c_start(I2C_Regs *i2c)
{
    /* Request START; hardware clears the bit once the condition is sent */
    i2c->CR1 |= I2C_CR1_START;
    return wait_flag(i2c, I2C_SR1_SB);
}

I2C_Status i2c_stop(I2C_Regs *i2c)
{
    i2c->CR1 |= I2C_CR1_STOP;
    /* Wait for the bus to go idle; BUSY deasserts after STOP is finished */
    return wait_sr2_clear(i2c, I2C_SR2_BUSY);
}

I2C_Status i2c_send_addr(I2C_Regs *i2c, uint8_t addr7, uint8_t rw)
{
    I2C_Status st;

    /* 7-bit address in bits [7:1], R/nW in bit 0 */
    i2c->DR = (uint32_t)((addr7 << 1) | (rw & 0x01U));

    st = wait_flag(i2c, I2C_SR1_ADDR);
    if (st != I2C_OK) {
        return st;
    }

    /*
     * Clear ADDR by reading SR1 then SR2 (RM0090 §27.3.3).
     * The read of SR2 is the actual clearing action; both reads are required
     * in sequence.
     */
    (void)i2c->SR1;
    (void)i2c->SR2;

    return I2C_OK;
}

I2C_Status i2c_write_byte(I2C_Regs *i2c, uint8_t data)
{
    I2C_Status st;

    st = wait_flag(i2c, I2C_SR1_TXE);
    if (st != I2C_OK) {
        return st;
    }

    i2c->DR = data;
    return I2C_OK;
}

I2C_Status i2c_read_byte(I2C_Regs *i2c, uint8_t *data)
{
    I2C_Status st;

    st = wait_flag(i2c, I2C_SR1_RXNE);
    if (st != I2C_OK) {
        return st;
    }

    *data = (uint8_t)(i2c->DR & 0xFFU);
    return I2C_OK;
}

/* =========================================================================
 * Composite transfer helpers
 * ========================================================================= */

I2C_Status i2c_write(I2C_Regs *i2c, uint8_t addr7,
                     const uint8_t *buf, uint32_t len)
{
    I2C_Status st;
    uint32_t i;

    st = i2c_start(i2c);
    if (st != I2C_OK) { return st; }

    st = i2c_send_addr(i2c, addr7, 0);
    if (st != I2C_OK) { (void)i2c_stop(i2c); return st; }

    for (i = 0; i < len; i++) {
        st = i2c_write_byte(i2c, buf[i]);
        if (st != I2C_OK) { (void)i2c_stop(i2c); return st; }
    }

    /* Wait for the last byte to shift out before asserting STOP */
    st = wait_flag(i2c, I2C_SR1_BTF);
    if (st != I2C_OK) { (void)i2c_stop(i2c); return st; }

    return i2c_stop(i2c);
}

I2C_Status i2c_read(I2C_Regs *i2c, uint8_t addr7,
                    uint8_t *buf, uint32_t len)
{
    I2C_Status st;

    if (len == 0) {
        return I2C_OK;
    }

    /* ---- 1-byte receive (RM0090 §27.3.3, figure 274) ---- */
    if (len == 1) {
        /*
         * Clear ACK before ADDR is cleared so the single byte is NACKed
         * immediately after it is received.
         */
        i2c->CR1 &= ~I2C_CR1_ACK;

        st = i2c_start(i2c);
        if (st != I2C_OK) { return st; }

        st = i2c_send_addr(i2c, addr7, 1);
        /* ADDR cleared inside i2c_send_addr — ACK was already cleared above */
        if (st != I2C_OK) { (void)i2c_stop(i2c); return st; }

        /* Schedule STOP before reading DR so it asserts right after the byte */
        i2c->CR1 |= I2C_CR1_STOP;

        st = i2c_read_byte(i2c, &buf[0]);
        if (st != I2C_OK) { return st; }

        /* Wait for bus to go idle */
        return wait_sr2_clear(i2c, I2C_SR2_BUSY);
    }

    /* ---- 2-byte receive (RM0090 §27.3.3, figure 275) ---- */
    if (len == 2) {
        /*
         * Set POS: ACK controls the NEXT byte after the current, so the NACK
         * will be applied to the second byte.  Clear ACK now so the second
         * byte is NACKed.
         */
        i2c->CR1 |= I2C_CR1_POS;
        i2c->CR1 &= ~I2C_CR1_ACK;

        st = i2c_start(i2c);
        if (st != I2C_OK) { i2c->CR1 &= ~I2C_CR1_POS; return st; }

        st = i2c_send_addr(i2c, addr7, 1);
        if (st != I2C_OK) {
            i2c->CR1 &= ~I2C_CR1_POS;
            (void)i2c_stop(i2c);
            return st;
        }

        /* Wait for both bytes to arrive (BTF set when shift and DR both full) */
        st = wait_flag(i2c, I2C_SR1_BTF);
        if (st != I2C_OK) {
            i2c->CR1 &= ~I2C_CR1_POS;
            (void)i2c_stop(i2c);
            return st;
        }

        /* Schedule STOP, then drain both bytes */
        i2c->CR1 |= I2C_CR1_STOP;
        buf[0] = (uint8_t)(i2c->DR & 0xFFU);
        buf[1] = (uint8_t)(i2c->DR & 0xFFU);

        i2c->CR1 &= ~I2C_CR1_POS;
        return wait_sr2_clear(i2c, I2C_SR2_BUSY);
    }

    /* ---- N-byte receive (N > 2, RM0090 §27.3.3, figure 273) ---- */
    i2c->CR1 |= I2C_CR1_ACK;   /* ACK all bytes except the last two */

    st = i2c_start(i2c);
    if (st != I2C_OK) { return st; }

    st = i2c_send_addr(i2c, addr7, 1);
    if (st != I2C_OK) { (void)i2c_stop(i2c); return st; }

    {
        uint32_t i;
        for (i = 0; i < len - 3U; i++) {
            st = i2c_read_byte(i2c, &buf[i]);
            if (st != I2C_OK) { (void)i2c_stop(i2c); return st; }
        }

        /*
         * Final three bytes: wait for BTF (penultimate byte in DR, last byte
         * in shift register), then clear ACK and read the third-from-last.
         * After the next BTF, assert STOP and drain the last two.
         */
        st = wait_flag(i2c, I2C_SR1_BTF);
        if (st != I2C_OK) { (void)i2c_stop(i2c); return st; }

        i2c->CR1 &= ~I2C_CR1_ACK;
        buf[len - 3U] = (uint8_t)(i2c->DR & 0xFFU);

        st = wait_flag(i2c, I2C_SR1_BTF);
        if (st != I2C_OK) { (void)i2c_stop(i2c); return st; }

        i2c->CR1 |= I2C_CR1_STOP;
        buf[len - 2U] = (uint8_t)(i2c->DR & 0xFFU);
        buf[len - 1U] = (uint8_t)(i2c->DR & 0xFFU);
    }

    return wait_sr2_clear(i2c, I2C_SR2_BUSY);
}

I2C_Status i2c_write_read(I2C_Regs *i2c, uint8_t addr7,
                          const uint8_t *wbuf, uint32_t wlen,
                          uint8_t *rbuf,  uint32_t rlen)
{
    I2C_Status st;
    uint32_t i;

    /* Phase 1: write (without STOP — repeated START follows) */
    st = i2c_start(i2c);
    if (st != I2C_OK) { return st; }

    st = i2c_send_addr(i2c, addr7, 0);
    if (st != I2C_OK) { (void)i2c_stop(i2c); return st; }

    for (i = 0; i < wlen; i++) {
        st = i2c_write_byte(i2c, wbuf[i]);
        if (st != I2C_OK) { (void)i2c_stop(i2c); return st; }
    }

    st = wait_flag(i2c, I2C_SR1_BTF);
    if (st != I2C_OK) { (void)i2c_stop(i2c); return st; }

    /* Phase 2: repeated START then read */
    return i2c_read(i2c, addr7, rbuf, rlen);
}

/* =========================================================================
 * BMP280 helpers
 * ========================================================================= */

/*
 * BMP280 calibration register base address (DS001 §4.2.2, table 17).
 * 24 bytes at 0x88–0x9F, all little-endian 16-bit pairs.
 */
#define BMP280_REG_CALIB_BASE  0x88U

/*
 * BMP280 ADC_T register base address.
 * Three consecutive bytes: 0xFA (msb), 0xFB (lsb), 0xFC (xlsb).
 * The 20-bit value is in bits [19:4] of the three bytes combined.
 */
#define BMP280_REG_TEMP_MSB    0xFAU

I2C_Status bmp280_read_calibration(I2C_Regs *i2c, uint8_t addr7,
                                   BMP280_Calib *calib)
{
    /*
     * 24 raw bytes from 0x88–0x9F.
     * Layout (each pair little-endian):
     *   [0..1]   dig_T1  uint16
     *   [2..3]   dig_T2  int16
     *   [4..5]   dig_T3  int16
     *   [6..7]   dig_P1  uint16
     *   [8..9]   dig_P2  int16
     *   [10..11] dig_P3  int16
     *   [12..13] dig_P4  int16
     *   [14..15] dig_P5  int16
     *   [16..17] dig_P6  int16
     *   [18..19] dig_P7  int16
     *   [20..21] dig_P8  int16
     *   [22..23] dig_P9  int16
     */
    uint8_t raw[24];
    uint8_t reg = BMP280_REG_CALIB_BASE;
    I2C_Status st;

    st = i2c_write_read(i2c, addr7, &reg, 1U, raw, 24U);
    if (st != I2C_OK) {
        return st;
    }

    /* Reconstruct signed/unsigned pairs; DS001 says all pairs little-endian */
    calib->dig_T1 = (uint16_t)((uint16_t)raw[1]  << 8) | raw[0];
    calib->dig_T2 = (int16_t) ((uint16_t)raw[3]  << 8  | raw[2]);
    calib->dig_T3 = (int16_t) ((uint16_t)raw[5]  << 8  | raw[4]);
    calib->dig_P1 = (uint16_t)((uint16_t)raw[7]  << 8) | raw[6];
    calib->dig_P2 = (int16_t) ((uint16_t)raw[9]  << 8  | raw[8]);
    calib->dig_P3 = (int16_t) ((uint16_t)raw[11] << 8  | raw[10]);
    calib->dig_P4 = (int16_t) ((uint16_t)raw[13] << 8  | raw[12]);
    calib->dig_P5 = (int16_t) ((uint16_t)raw[15] << 8  | raw[14]);
    calib->dig_P6 = (int16_t) ((uint16_t)raw[17] << 8  | raw[16]);
    calib->dig_P7 = (int16_t) ((uint16_t)raw[19] << 8  | raw[18]);
    calib->dig_P8 = (int16_t) ((uint16_t)raw[21] << 8  | raw[20]);
    calib->dig_P9 = (int16_t) ((uint16_t)raw[23] << 8  | raw[22]);

    return I2C_OK;
}

I2C_Status bmp280_read_temp_compensated(I2C_Regs *i2c, uint8_t addr7,
                                        const BMP280_Calib *calib,
                                        int32_t *temp_centideg,
                                        int32_t *t_fine_out)
{
    /*
     * Integer compensation formula — DS001 rev 1.19 §4.2.3
     *
     * var1 = ((((adc_T >> 3) - ((int32_t)dig_T1 << 1)))
     *          * ((int32_t)dig_T2)) >> 11;
     *
     * var2 = (((((adc_T >> 4) - ((int32_t)dig_T1))
     *           * ((adc_T >> 4) - ((int32_t)dig_T1))) >> 12)
     *          * ((int32_t)dig_T3)) >> 14;
     *
     * t_fine = var1 + var2;
     * T = (t_fine * 5 + 128) >> 8;   <- result in units of 0.01 °C
     */
    uint8_t raw[3];
    uint8_t reg = BMP280_REG_TEMP_MSB;
    I2C_Status st;
    int32_t adc_T;
    int32_t var1, var2, t_fine;

    st = i2c_write_read(i2c, addr7, &reg, 1U, raw, 3U);
    if (st != I2C_OK) {
        return st;
    }

    /*
     * Reconstruct 20-bit ADC value.
     * raw[0] = temp_msb  (bits 19:12)
     * raw[1] = temp_lsb  (bits 11:4)
     * raw[2] = temp_xlsb (bits 3:0 in [7:4]; lower nibble unused)
     */
    adc_T = ((int32_t)raw[0] << 12)
          | ((int32_t)raw[1] <<  4)
          | ((int32_t)raw[2] >>  4);

    /* Datasheet formula — no floating point, all int32 */
    var1 = (((adc_T >> 3) - ((int32_t)calib->dig_T1 << 1))
             * (int32_t)calib->dig_T2) >> 11;

    var2 = (((((adc_T >> 4) - (int32_t)calib->dig_T1)
               * ((adc_T >> 4) - (int32_t)calib->dig_T1)) >> 12)
             * (int32_t)calib->dig_T3) >> 14;

    t_fine = var1 + var2;

    /* Result in 0.01 °C units (2534 == 25.34 °C) */
    *temp_centideg = (t_fine * 5 + 128) >> 8;

    if (t_fine_out != (int32_t *)0) {
        *t_fine_out = t_fine;
    }

    return I2C_OK;
}
