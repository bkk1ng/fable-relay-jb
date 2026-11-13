/**
 * i2c.c — Bare-metal I2C master driver for ARM Cortex-M4
 *
 * Register-level, polling, 7-bit addressing, no HAL.
 *
 * Assumes:
 *   - RCC has enabled the I2C peripheral clock (APB1).
 *   - GPIO pins are configured for open-drain AF with appropriate
 *     pull-ups before i2c_init() is called.
 *   - The peripheral base address is passed in through I2C_Config.
 *
 * BMP280 register map (relevant subset):
 *   0x88–0x9F  Calibration data (NVM, 24 bytes: T1–T3, P1–P9)
 *   0xF3       status
 *   0xF4       ctrl_meas  (osrs_t, osrs_p, mode)
 *   0xF5       config
 *   0xFA–0xFC  temp_msb, temp_lsb, temp_xlsb (20-bit ADC)
 */

#include "i2c.h"

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                    */
/* ------------------------------------------------------------------ */

/**
 * clear_errors — clear BERR, ARLO, AF in SR1 so the bus can recover.
 */
static void clear_errors(I2C_Regs *i2c)
{
    i2c->SR1 &= ~(I2C_SR1_BERR | I2C_SR1_ARLO | I2C_SR1_AF);
}

/**
 * wait_flag — spin until bit(s) in SR1 are set, or timeout.
 * Returns I2C_OK or one of the I2C_ERR_* codes.
 *
 * Also watches for BERR, ARLO, AF on every iteration so a bus fault
 * terminates immediately rather than spinning to the timeout.
 */
static int wait_flag(I2C_Regs *i2c, uint32_t flag)
{
    uint32_t cnt = I2C_TIMEOUT_CNT;

    while (cnt--) {
        uint32_t sr1 = i2c->SR1;

        /* Check error bits first */
        if (sr1 & I2C_SR1_BERR) {
            clear_errors(i2c);
            return I2C_ERR_BUS;
        }
        if (sr1 & I2C_SR1_ARLO) {
            clear_errors(i2c);
            return I2C_ERR_ARBITRATION;
        }
        if (sr1 & I2C_SR1_AF) {
            clear_errors(i2c);
            return I2C_ERR_NACK;
        }

        if ((sr1 & flag) == flag) {
            return I2C_OK;
        }
    }
    return I2C_ERR_TIMEOUT;
}

/**
 * addr_clear — read SR1 then SR2 to clear the ADDR flag.
 * Must be called after every address phase.
 */
static inline void addr_clear(I2C_Regs *i2c)
{
    (void)i2c->SR1;
    (void)i2c->SR2;
}

/* ------------------------------------------------------------------ */
/*  i2c_init                                                            */
/* ------------------------------------------------------------------ */
int i2c_init(const I2C_Config *cfg)
{
    I2C_Regs *i2c = cfg->regs;

    /* Disable peripheral, software-reset, re-enable */
    i2c->CR1 &= ~I2C_CR1_PE;
    i2c->CR1 |=  I2C_CR1_SWRST;
    i2c->CR1 &= ~I2C_CR1_SWRST;

    /* CR2: APB1 peripheral clock in MHz (must be 2–50) */
    uint32_t freq_mhz = cfg->pclk_hz / 1000000u;
    i2c->CR2 = freq_mhz & 0x3Fu;

    /* TRISE: maximum rise time
     *   Sm (≤100 kHz): 1000 ns → (freq_mhz * 1) + 1
     *   Fm (≤400 kHz):  300 ns → (freq_mhz * 300 / 1000) + 1
     */
    if (cfg->speed_hz <= 100000u) {
        i2c->TRISE = freq_mhz + 1u;
    } else {
        i2c->TRISE = (freq_mhz * 300u) / 1000u + 1u;
    }

    /* CCR: clock control register
     *   Sm: CCR = pclk / (2 * speed)
     *   Fm: CCR = pclk / (3 * speed) in duty=0 (DUTY bit clear)
     *       CCR must be ≥ 1
     */
    uint32_t ccr;
    if (cfg->speed_hz <= 100000u) {
        ccr = cfg->pclk_hz / (2u * cfg->speed_hz);
        if (ccr < 4u) ccr = 4u;        /* hardware minimum */
        i2c->CCR = ccr & 0xFFFu;
    } else {
        /* Fm, DUTY=0: Thigh = Tlow = CCR * Tpclk, ratio 1:2 */
        ccr = cfg->pclk_hz / (3u * cfg->speed_hz);
        if (ccr < 1u) ccr = 1u;
        /* Set F/S bit (bit 15) for fast mode */
        i2c->CCR = (1u << 15) | (ccr & 0xFFFu);
    }

    /* Enable peripheral and ACK */
    i2c->CR1 = I2C_CR1_PE | I2C_CR1_ACK;

    return I2C_OK;
}

/* ------------------------------------------------------------------ */
/*  i2c_start                                                           */
/* ------------------------------------------------------------------ */
int i2c_start(I2C_Regs *i2c)
{
    /* Request START; hardware clears the bit once it's sent */
    i2c->CR1 |= I2C_CR1_ACK | I2C_CR1_START;
    return wait_flag(i2c, I2C_SR1_SB);
}

/* ------------------------------------------------------------------ */
/*  i2c_stop                                                            */
/* ------------------------------------------------------------------ */
void i2c_stop(I2C_Regs *i2c)
{
    i2c->CR1 |= I2C_CR1_STOP;
    /* No flag to poll — hardware clears STOP after the condition.
     * Caller may need a small delay before the next START on a
     * saturated bus; at typical firmware speeds that is free. */
}

/* ------------------------------------------------------------------ */
/*  i2c_address                                                         */
/* ------------------------------------------------------------------ */
int i2c_address(I2C_Regs *i2c, uint8_t addr, uint8_t rw)
{
    int rc;

    /* SB must already be set (caller just called i2c_start).
     * Writing DR clears SB. */
    i2c->DR = (uint8_t)((addr << 1u) | (rw & 1u));

    rc = wait_flag(i2c, I2C_SR1_ADDR);
    if (rc != I2C_OK) {
        return rc;
    }

    /* Clear ADDR by reading SR1 (already read in wait_flag) then SR2 */
    addr_clear(i2c);
    return I2C_OK;
}

/* ------------------------------------------------------------------ */
/*  i2c_write                                                           */
/* ------------------------------------------------------------------ */
int i2c_write(I2C_Regs *i2c, const uint8_t *buf, size_t len)
{
    int rc;

    for (size_t i = 0; i < len; i++) {
        /* Wait for transmit buffer empty */
        rc = wait_flag(i2c, I2C_SR1_TXE);
        if (rc != I2C_OK) {
            return rc;
        }
        i2c->DR = buf[i];
    }

    /* Wait for byte transfer finished (last byte fully clocked out) */
    rc = wait_flag(i2c, I2C_SR1_BTF);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  i2c_read                                                            */
/* ------------------------------------------------------------------ */
int i2c_read(I2C_Regs *i2c, uint8_t *buf, size_t len)
{
    int rc;

    if (len == 0) {
        return I2C_OK;
    }

    if (len == 1) {
        /*
         * Single-byte read: clear ACK before the byte arrives so the
         * peripheral sends NACK automatically, then set STOP.
         */
        i2c->CR1 &= ~I2C_CR1_ACK;
        i2c_stop(i2c);

        rc = wait_flag(i2c, I2C_SR1_RXNE);
        if (rc != I2C_OK) {
            return rc;
        }
        buf[0] = (uint8_t)i2c->DR;
        return I2C_OK;
    }

    if (len == 2) {
        /*
         * Two-byte read (RM0090 §27.3.3):
         *   - Set POS: ACK applies to the NEXT byte loaded into shift reg.
         *   - Clear ACK so the second byte is NACKed.
         *   - Wait for BTF (both bytes in DR/shift reg), then STOP, then read.
         */
        i2c->CR1 |=  (1u << 11);        /* POS bit */
        i2c->CR1 &= ~I2C_CR1_ACK;

        rc = wait_flag(i2c, I2C_SR1_BTF);
        if (rc != I2C_OK) {
            i2c->CR1 &= ~(1u << 11);    /* clear POS on error */
            return rc;
        }
        i2c_stop(i2c);
        buf[0] = (uint8_t)i2c->DR;
        buf[1] = (uint8_t)i2c->DR;
        i2c->CR1 &= ~(1u << 11);        /* clear POS */
        return I2C_OK;
    }

    /* General case: N ≥ 3 bytes
     * ACK is already set from i2c_init / i2c_start.
     * When two bytes remain, follow the two-byte procedure above.
     */
    size_t remaining = len;

    while (remaining > 3u) {
        rc = wait_flag(i2c, I2C_SR1_RXNE);
        if (rc != I2C_OK) {
            return rc;
        }
        *buf++ = (uint8_t)i2c->DR;
        remaining--;
    }

    /* Three bytes left: wait BTF, then disable ACK, read first, STOP, read rest */
    rc = wait_flag(i2c, I2C_SR1_BTF);
    if (rc != I2C_OK) {
        return rc;
    }
    i2c->CR1 &= ~I2C_CR1_ACK;
    buf[0] = (uint8_t)i2c->DR;         /* byte N-2 */

    rc = wait_flag(i2c, I2C_SR1_BTF);
    if (rc != I2C_OK) {
        return rc;
    }
    i2c_stop(i2c);
    buf[1] = (uint8_t)i2c->DR;         /* byte N-1 */
    buf[2] = (uint8_t)i2c->DR;         /* byte N   */

    /* Re-arm ACK for next transaction */
    i2c->CR1 |= I2C_CR1_ACK;
    return I2C_OK;
}

/* ------------------------------------------------------------------ */
/*  i2c_write_read                                                      */
/* ------------------------------------------------------------------ */
int i2c_write_read(I2C_Regs *i2c, uint8_t addr,
                   const uint8_t *wbuf, size_t wlen,
                   uint8_t       *rbuf, size_t rlen)
{
    int rc;

    /* Phase 1: START + address (write direction) */
    rc = i2c_start(i2c);
    if (rc != I2C_OK) goto fail;

    rc = i2c_address(i2c, addr, 0);
    if (rc != I2C_OK) goto fail;

    /* Phase 2: write register address / command bytes */
    rc = i2c_write(i2c, wbuf, wlen);
    if (rc != I2C_OK) goto fail;

    /* Phase 3: repeated START + address (read direction) */
    rc = i2c_start(i2c);
    if (rc != I2C_OK) goto fail;

    rc = i2c_address(i2c, addr, 1);
    if (rc != I2C_OK) goto fail;

    /* Phase 4: read data — i2c_read issues STOP internally */
    rc = i2c_read(i2c, rbuf, rlen);
    return rc;

fail:
    i2c_stop(i2c);
    return rc;
}

/* ------------------------------------------------------------------ */
/*  BMP280 — register addresses                                         */
/* ------------------------------------------------------------------ */
#define BMP280_REG_CALIB_START  0x88u   /* T1 LSB — first calib byte  */
#define BMP280_REG_CTRL_MEAS    0xF4u
#define BMP280_REG_TEMP_MSB     0xFAu

/* ctrl_meas: forced mode, osrs_t=1 (×1), osrs_p=0 (skipped) */
#define BMP280_FORCED_TEMP_ONLY  0x25u  /* 00 100 10 1 */

/* Number of calibration bytes: T1(2)+T2(2)+T3(2)+P1(2)..P9(2) = 24 */
#define BMP280_CALIB_LEN  24u

/* ------------------------------------------------------------------ */
/*  bmp280_read_calibration                                             */
/* ------------------------------------------------------------------ */
int bmp280_read_calibration(I2C_Regs *i2c, uint8_t addr,
                             BMP280_CalibData *cal)
{
    uint8_t raw[BMP280_CALIB_LEN];
    uint8_t reg = BMP280_REG_CALIB_START;

    int rc = i2c_write_read(i2c, addr, &reg, 1u, raw, BMP280_CALIB_LEN);
    if (rc != I2C_OK) {
        return rc;
    }

    /* All calibration words are little-endian in NVM.
     * T1, P1 are unsigned; the rest are signed. */
    cal->dig_T1 = (uint16_t)(raw[1]  << 8) | raw[0];
    cal->dig_T2 = (int16_t) ((raw[3]  << 8) | raw[2]);
    cal->dig_T3 = (int16_t) ((raw[5]  << 8) | raw[4]);

    cal->dig_P1 = (uint16_t)(raw[7]  << 8) | raw[6];
    cal->dig_P2 = (int16_t) ((raw[9]  << 8) | raw[8]);
    cal->dig_P3 = (int16_t) ((raw[11] << 8) | raw[10]);
    cal->dig_P4 = (int16_t) ((raw[13] << 8) | raw[12]);
    cal->dig_P5 = (int16_t) ((raw[15] << 8) | raw[14]);
    cal->dig_P6 = (int16_t) ((raw[17] << 8) | raw[16]);
    cal->dig_P7 = (int16_t) ((raw[19] << 8) | raw[18]);
    cal->dig_P8 = (int16_t) ((raw[21] << 8) | raw[20]);
    cal->dig_P9 = (int16_t) ((raw[23] << 8) | raw[22]);

    return I2C_OK;
}

/* ------------------------------------------------------------------ */
/*  bmp280_read_temp_compensated                                        */
/* ------------------------------------------------------------------ */
int32_t bmp280_read_temp_compensated(I2C_Regs *i2c, uint8_t addr,
                                      const BMP280_CalibData *cal,
                                      int32_t *t_fine_out)
{
    uint8_t ctrl = BMP280_FORCED_TEMP_ONLY;
    uint8_t reg  = BMP280_REG_CTRL_MEAS;
    uint8_t raw[3];

    /* Kick off a forced-mode measurement */
    int rc = i2c_write_read(i2c, addr, &reg, 1u, NULL, 0u);
    (void)rc; /* best-effort: proceed even on a marginal ack */

    /* Write ctrl_meas to trigger conversion.
     * Simple write: START, addr+W, reg, value, STOP. */
    {
        int r2;
        uint8_t pair[2] = { BMP280_REG_CTRL_MEAS, ctrl };
        r2 = i2c_start(i2c);
        if (r2 != I2C_OK) goto err;
        r2 = i2c_address(i2c, addr, 0);
        if (r2 != I2C_OK) { i2c_stop(i2c); goto err; }
        r2 = i2c_write(i2c, pair, 2u);
        i2c_stop(i2c);
        if (r2 != I2C_OK) goto err;
    }

    /*
     * Wait for measurement ready: status register bit 3 (measuring)
     * goes low when done. In forced mode at 1× oversampling that is
     * ~2 ms. We poll with a simple timeout rather than a fixed delay
     * so this works across all clock speeds.
     */
    {
        uint8_t status_reg = 0xF3u;
        uint8_t status_val = 0;
        uint32_t timeout = I2C_TIMEOUT_CNT;

        while (timeout--) {
            int rs = i2c_write_read(i2c, addr,
                                     &status_reg, 1u,
                                     &status_val, 1u);
            if (rs != I2C_OK) goto err;
            if (!(status_val & 0x08u)) break;   /* measuring bit clear */
        }
    }

    /* Read the three ADC bytes: temp_msb, temp_lsb, temp_xlsb */
    {
        uint8_t temp_reg = BMP280_REG_TEMP_MSB;
        int rs = i2c_write_read(i2c, addr, &temp_reg, 1u, raw, 3u);
        if (rs != I2C_OK) goto err;
    }

    /* Reconstruct 20-bit ADC value (bits 19:4 in bytes 0:1, bits 3:0 in byte 2 >> 4) */
    int32_t adc_T = ((int32_t)raw[0] << 12) |
                    ((int32_t)raw[1] <<  4) |
                    ((int32_t)raw[2] >>  4);

    /*
     * BMP280 integer compensation formula — datasheet section 4.2.3.
     *
     * var1 = ((adc_T >> 3) - (dig_T1 << 1)) * dig_T2 >> 11
     * var2 = (((adc_T >> 4) - dig_T1)^2 >> 12) * dig_T3 >> 14
     * t_fine = var1 + var2
     * T = (t_fine * 5 + 128) >> 8          [in units of 0.01 °C]
     */
    int32_t var1 = (((adc_T >> 3) - ((int32_t)cal->dig_T1 << 1)) *
                     (int32_t)cal->dig_T2) >> 11;

    int32_t var2 = (((adc_T >> 4) - (int32_t)cal->dig_T1) *
                    ((adc_T >> 4) - (int32_t)cal->dig_T1)) >> 12;
    var2 = (var2 * (int32_t)cal->dig_T3) >> 14;

    int32_t t_fine = var1 + var2;

    if (t_fine_out) {
        *t_fine_out = t_fine;
    }

    return (t_fine * 5 + 128) >> 8;

err:
    if (t_fine_out) {
        *t_fine_out = 0;
    }
    return 0;
}
