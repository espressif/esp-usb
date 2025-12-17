/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "fusb302_ctrl.h"
#include "esp_typec.h" // for esp_typec_power_role_t

#define TAG "fusb302"

#define MAX_INT_READS 8 /* Maximum number of interrupt register reads to avoid infinite loops */

/**
 * @brief FUSB302 device context structure.
 * @param hw Hardware configuration
 * @param role Current power role
 */
struct fusb302_dev {
    fusb302_hw_cfg_t hw;              /**< Hardware configuration */
    esp_typec_power_role_t role;      /**< Current power role */
    esp_typec_rp_current_t rp_current;/**< Advertised Rp current when sourcing */
};

/* FUSB302 registers */
enum {
    REG_DEVICE_ID  = 0x01,
    REG_SWITCHES0  = 0x02,
    REG_SWITCHES1  = 0x03,
    REG_MEASURE    = 0x04,
    REG_CONTROL0   = 0x06,
    REG_CONTROL1   = 0x07,
    REG_CONTROL2   = 0x08,
    REG_CONTROL3   = 0x09,
    REG_MASK       = 0x0A,
    REG_POWER      = 0x0B,
    REG_RESET      = 0x0C,
    REG_MASKA      = 0x0E,
    REG_MASKB      = 0x0F,

    REG_STATUS0A   = 0x3C,
    REG_STATUS1A   = 0x3D,
    REG_INTERRUPTA = 0x3E,
    REG_INTERRUPTB = 0x3F,
    REG_STATUS0    = 0x40,
    REG_STATUS1    = 0x41,
    REG_INTERRUPT  = 0x42,
    REG_FIFOS      = 0x43,
};

/* SWITCHES0 */
#define SW0_CC2_PU_EN   (1u << 7)
#define SW0_CC1_PU_EN   (1u << 6)
#define SW0_VCONN_CC2   (1u << 5)
#define SW0_VCONN_CC1   (1u << 4)
#define SW0_MEAS_CC2    (1u << 3)
#define SW0_MEAS_CC1    (1u << 2)
#define SW0_CC2_PD_EN   (1u << 1)
#define SW0_CC1_PD_EN   (1u << 0)

/* SWITCHES1 */
#define SW1_POWERROLE   (1u << 7)
#define SW1_SPECREV1    (1u << 6)
#define SW1_SPECREV0    (1u << 5)
#define SW1_DATAROLE    (1u << 4)
#define SW1_TXCC2_EN    (1u << 1)
#define SW1_TXCC1_EN    (1u << 0)

/* CONTROL0 */
#define CTL0_INT_MASK   (1u << 5)   /* 0 = INT pin enabled, 1 = masked */
#define CTL0_HOST_CUR_MASK   (0x3 << 2)
#define CTL0_HOST_CUR_DEFAULT  (0x1u << 2)       /* 01b -> Default (80uA) */
#define CTL0_HOST_CUR_MEDIUM   (0x2u << 2)       /* 10b -> 1.5A */
#define CTL0_HOST_CUR_HIGH     (0x3u << 2)       /* 11b -> 3A  */

/* CONTROL2 MODE bits */
#define CTL2_MODE_MASK  (0xE)
#define CTL2_MODE_DFP   (0x6)
#define CTL2_MODE_UFP   (0x4)
#define CTL2_MODE_DRP   (0x2)
#define CTL2_MODE_NONE  (0x0)
#define CTL2_TOGGLE     (1u << 0)

/* POWER levels */
#define PWR_PWR_ALL     0x0F  /* full on */
#define PWR_PWR_HIGH    0x07
#define PWR_PWR_MEDIUM  0x03
#define PWR_PWR_LOW     0x01

/* RESET */
#define RST_PD_RESET    (1u << 1)
#define RST_SW_RESET    (1u << 0)

/* STATUS0 */
#define ST0_VBUSOK          (1u << 7)
#define ST0_ACTIVITY        (1u << 6)
#define ST0_COMP            (1u << 5)
#define ST0_CRC_CHK         (1u << 4)
#define ST0_ALERT           (1u << 3)
#define ST0_WAKE            (1u << 2)
#define ST0_BC_LVL_MASK     0x03
#define ST0_BC_LVL_0_200    0x0
#define ST0_BC_LVL_200_600  0x1
#define ST0_BC_LVL_600_1230 0x2
#define ST0_BC_LVL_1230_MAX 0x3

/* STATUS1A (0x3D): [5:0] = TOGSS3 TOGSS2 TOGSS1 RXSOP2DB RXSOP1DB RXSOP */
#define ST1A_TOGSS_SHIFT      3
#define ST1A_TOGSS_MASK       (0x7u << ST1A_TOGSS_SHIFT)
#define ST1A_RXSOP_MASK       0x7u

/* Decoded 3-bit TOGSS values after shifting */
#define ST1A_TOGSS_TOGGLE     0u  /* toggle logic running */
#define ST1A_TOGSS_SRC1       1u  /* STOP_SRC1 (CC1) */
#define ST1A_TOGSS_SRC2       2u  /* STOP_SRC2 (CC2) */
#define ST1A_TOGSS_SNK1       5u  /* STOP_SNK1 (CC1) */
#define ST1A_TOGSS_SNK2       6u  /* STOP_SNK2 (CC2) */
#define ST1A_TOGSS_AUDIO      7u  /* Audio accessory (settles to SRC1) */


/* --- helpers for Rp current mapping --- */
static inline uint8_t rp_current_to_host_cur_bits(esp_typec_rp_current_t rp)
{
    switch (rp) {
    case ESP_TYPEC_RP_1A5:
        return CTL0_HOST_CUR_MEDIUM;
    case ESP_TYPEC_RP_3A0:
        return CTL0_HOST_CUR_HIGH;
    case ESP_TYPEC_RP_DEFAULT:
    default:
        return CTL0_HOST_CUR_DEFAULT;
    }
}

static inline uint32_t rp_current_to_ma(esp_typec_rp_current_t rp)
{
    switch (rp) {
    case ESP_TYPEC_RP_1A5:
        return 1500;
    case ESP_TYPEC_RP_3A0:
        return 3000;
    case ESP_TYPEC_RP_DEFAULT:
    default:
        return 500;
    }
}

/* REG_INTERRUPT (0x42) — top-level CC/VBUS causes */
#define INT_BC_LVL      (1u << 0)
#define INT_COLLISION   (1u << 1)
#define INT_WAKE        (1u << 2)
#define INT_ALERT       (1u << 3)
#define INT_CRC_CHK     (1u << 4)
#define INT_COMP_CHNG   (1u << 5)
#define INT_ACTIVITY    (1u << 6)
#define INT_VBUSOK      (1u << 7)

/* REG_INTERRUPTA (0x3E) — PD/Toggle outcomes (mapping varies by rev) */
#define INTA_TX_SUCC    (1u << 1)
#define INTA_RETRY_FAIL (1u << 2)
#define INTA_SOFT_FAIL  (1u << 3)
#define INTA_HARD_SENT  (1u << 5)
#define INTA_TOG_DONE   (1u << 6)

/* REG_INTERRUPTB (0x3F) — PD RX, resets, FIFO (mapping varies by rev) */
#define INTB_GCRCSENT    (1u << 0)
#define INTB_RX_SOP      (1u << 1)
#define INTB_RX_ANY_MASK 0x1F

/* ---------------- New I2C helpers (esp_driver_i2c) ---------------- */

/**
 * @brief Write 8-bit value to FUSB302 register via I2C.
 * @param dev I2C device handle
 * @param reg Register address
 * @param val Value to write
 * @return ESP_OK on success
 */
static inline esp_err_t i2c_wr8(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev, buf, sizeof(buf), 100 /*ms*/);
}

/**
 * @brief Read 8-bit value from FUSB302 register via I2C.
 * @param dev I2C device handle
 * @param reg Register address
 * @param val Pointer to value output
 * @return ESP_OK on success
 */
static inline esp_err_t i2c_rd8(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *val)
{
    if (!val) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = i2c_master_transmit(dev, &reg, 1, 100);
    if (err != ESP_OK) {
        return err;
    }
    return i2c_master_receive(dev, val, 1, 100);
}

/* One-shot CC/VBUS measurement.
 *
 * - In SINK (UFP) role: manual BC_LVL + MEAS scanning (as before).
 * - In SOURCE / DRP: use toggle engine result (TOGSS) for attach + orientation.
 */
static esp_err_t fusb302_measure_status(fusb302_dev_t *dev, typec_cc_status_t *st)
{
    if (!dev || !st) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_dev_handle_t d = dev->hw.i2c_dev;
    memset(st, 0, sizeof(*st));

    if (dev->role == ESP_TYPEC_PWR_SINK) {
        /* ========= SINK path (BC_LVL-based) ========= */
        uint8_t old_mask = 0;
        uint8_t sw0 = 0;
        uint8_t st0 = 0;

        ESP_RETURN_ON_ERROR(i2c_rd8(d, REG_MASK, &old_mask), TAG, "rd MASK");

        /* Mask CC-change interrupts while we flip MEAS bits */
        uint8_t mask_during_meas = old_mask | INT_BC_LVL | INT_COMP_CHNG;
        ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_MASK, mask_during_meas),
                            TAG, "mask CC during meas");

        ESP_RETURN_ON_ERROR(i2c_rd8(d, REG_SWITCHES0, &sw0), TAG, "rd SW0");

        uint8_t sw0_meas = sw0 & ~(SW0_MEAS_CC1 | SW0_MEAS_CC2);

        /* ---- Measure CC1 ---- */
        sw0_meas |= SW0_MEAS_CC1;
        ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_SWITCHES0, sw0_meas), TAG, "meas CC1");
        esp_rom_delay_us(100);
        ESP_RETURN_ON_ERROR(i2c_rd8(d, REG_STATUS0, &st0), TAG, "st0 CC1");
        uint8_t bclvl_cc1 = (st0 & ST0_BC_LVL_MASK);
        bool vbus_ok = (st0 & ST0_VBUSOK) != 0;

        /* ---- Measure CC2 ---- */
        sw0_meas = (sw0_meas & ~SW0_MEAS_CC1) | SW0_MEAS_CC2;
        ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_SWITCHES0, sw0_meas), TAG, "meas CC2");
        esp_rom_delay_us(100);
        ESP_RETURN_ON_ERROR(i2c_rd8(d, REG_STATUS0, &st0), TAG, "st0 CC2");
        uint8_t bclvl_cc2 = (st0 & ST0_BC_LVL_MASK);

        /* Restore SWITCHES0 and MASK */
        ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_SWITCHES0, sw0), TAG, "restore SW0");
        ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_MASK, old_mask), TAG, "restore MASK");

        st->vbus_ok = vbus_ok;

        bool cc1_has_rp = (bclvl_cc1 != ST0_BC_LVL_0_200);
        bool cc2_has_rp = (bclvl_cc2 != ST0_BC_LVL_0_200);

        st->attached   = (cc1_has_rp ^ cc2_has_rp);    // exactly one CC sees Rp
        st->cc2_active = st->attached ? cc2_has_rp : false;

        uint8_t bclvl  = cc2_has_rp ? bclvl_cc2 : bclvl_cc1;
        st->rp_cur_ma  = (bclvl == ST0_BC_LVL_200_600)  ? 500  :
                         (bclvl == ST0_BC_LVL_600_1230) ? 1500 :
                         (bclvl == ST0_BC_LVL_1230_MAX) ? 3000 :
                         (vbus_ok ? 500 : 0);

        ESP_LOGI(TAG, "CC(SNK): att=%d act=%s CC1=0x%02X CC2=0x%02X Rp=%umA VBUSOK=%d",
                 st->attached, st->cc2_active ? "CC2" : "CC1",
                 bclvl_cc1, bclvl_cc2, (unsigned)st->rp_cur_ma, st->vbus_ok);
    } else {
        /* ========= SOURCE / DRP path: TOGGLE attach + manual DETACH ========= */

        uint8_t status0 = 0, status1a = 0, ctl2 = 0;
        uint8_t sw0 = 0, sw1 = 0;

        ESP_RETURN_ON_ERROR(i2c_rd8(d, REG_STATUS1A, &status1a), TAG, "rd STATUS1A");
        ESP_RETURN_ON_ERROR(i2c_rd8(d, REG_CONTROL2, &ctl2),     TAG, "rd CONTROL2");

        uint8_t togss = (status1a >> 3) & 0x07;   // bits [5:3], as you described

        st->rp_cur_ma = rp_current_to_ma(dev->rp_current);

        /* --- 1) If TOGSS does NOT indicate a valid “source attached” result: ensure toggle is running --- */
        bool src_att = (togss == ST1A_TOGSS_SRC1) || (togss == ST1A_TOGSS_SRC2);

        if (!src_att) {
            st->attached   = false;
            st->cc2_active = false;

            /* If toggle engine is not running, restart it with a 0->1 edge */
            if ((ctl2 & CTL2_TOGGLE) == 0) {
                uint8_t base = ctl2 & ~CTL2_TOGGLE;
                ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_CONTROL2, base), TAG, "ctl2 toggle=0");
                esp_rom_delay_us(50);
                ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_CONTROL2, base | CTL2_TOGGLE), TAG, "ctl2 toggle=1");

                /* Clear any latched TOG_DONE/etc (optional but helps) */
                uint8_t tmp;
                (void)i2c_rd8(d, REG_INTERRUPTA, &tmp);
                (void)i2c_rd8(d, REG_INTERRUPT,  &tmp);
            }

            /* VBUSOK is still useful to log, but not “attachment” here */
            ESP_RETURN_ON_ERROR(i2c_rd8(d, REG_STATUS0, &status0), TAG, "rd STATUS0");
            st->vbus_ok = (status0 & ST0_VBUSOK) != 0;

            ESP_LOGI(TAG, "CC(TOG): role=%d status1a=0x%02X togss=%u att=0 act=CC1 Rp=%umA VBUSOK=%d",
                     dev->role, status1a, togss, (unsigned)st->rp_cur_ma, st->vbus_ok);
            return ESP_OK;
        }

        /* --- 2) We have a TOGGLE “source attached” result -> polarity is known --- */
        bool cc2 = (togss == ST1A_TOGSS_SRC2);
        st->cc2_active = cc2;

        /* Program TXCC polarity so PD works in BOTH orientations */
        ESP_RETURN_ON_ERROR(i2c_rd8(d, REG_SWITCHES1, &sw1), TAG, "rd SW1");
        uint8_t want_sw1 = (sw1 & ~(SW1_TXCC1_EN | SW1_TXCC2_EN)) | SW1_POWERROLE |
                           (cc2 ? SW1_TXCC2_EN : SW1_TXCC1_EN);
        if (want_sw1 != sw1) {
            ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_SWITCHES1, want_sw1), TAG, "wr SW1 TXCC");
        }

        /* Point MEAS at the ACTIVE CC so STATUS0.COMP and INT_COMP_CHNG can detect DETACH */
        ESP_RETURN_ON_ERROR(i2c_rd8(d, REG_SWITCHES0, &sw0), TAG, "rd SW0");
        uint8_t want_sw0 = (sw0 & ~(SW0_MEAS_CC1 | SW0_MEAS_CC2)) |
                           (cc2 ? SW0_MEAS_CC2 : SW0_MEAS_CC1) |
                           SW0_CC1_PU_EN | SW0_CC2_PU_EN;
        if (want_sw0 != sw0) {
            ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_SWITCHES0, want_sw0), TAG, "wr SW0 MEAS");
            esp_rom_delay_us(50);
        }

        /* Now read STATUS0 with the correct MEAS selected */
        ESP_RETURN_ON_ERROR(i2c_rd8(d, REG_STATUS0, &status0), TAG, "rd STATUS0");
        st->vbus_ok = (status0 & ST0_VBUSOK) != 0;

        /* COMP=0 => below MDAC => Rd present => still attached
           COMP=1 => above MDAC => open => detached */
        bool rd_present = ((status0 & ST0_COMP) == 0);
        st->attached = rd_present;

        if (!st->attached) {
            /* Detach: clear TXCC and restart toggle so next attach works */
            uint8_t sw1_det = want_sw1 & ~(SW1_TXCC1_EN | SW1_TXCC2_EN);
            (void)i2c_wr8(d, REG_SWITCHES1, sw1_det);

            uint8_t base = (ctl2 & ~CTL2_TOGGLE);
            (void)i2c_wr8(d, REG_CONTROL2, base);
            esp_rom_delay_us(50);
            (void)i2c_wr8(d, REG_CONTROL2, base | CTL2_TOGGLE);
        }

        ESP_LOGI(TAG, "CC(TOG): role=%d status1a=0x%02X togss=%u att=%d act=%s Rp=%umA VBUSOK=%d",
                 dev->role, status1a, togss, st->attached,
                 st->cc2_active ? "CC2" : "CC1",
                 (unsigned)st->rp_cur_ma, st->vbus_ok);
    }
    return ESP_OK;
}

static esp_err_t fusb302_config_src_detect(fusb302_dev_t *dev)
{
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_dev_handle_t d = dev->hw.i2c_dev;

    /* --- Select MDAC threshold for Rd attach/detach, per Table 6 --- */
    // Values are identical to what Linux driver uses: MDAC codes for ~1.6 V / 2.6 V
    uint8_t mdac_attach;

    switch (dev->rp_current) {
    case ESP_TYPEC_RP_3A0:
        mdac_attach = 61;    // ≈2.6 V
        break;
    case ESP_TYPEC_RP_1A5:
    case ESP_TYPEC_RP_DEFAULT:
    default:
        mdac_attach = 38;    // ≈1.6 V
        break;
    }

    ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_MEASURE, mdac_attach),
                        TAG, "set SRC MDAC");

    /* --- Unmask BOTH BC_LVL and COMP_CHNG --- */
    uint8_t mask;
    ESP_RETURN_ON_ERROR(i2c_rd8(d, REG_MASK, &mask),
                        TAG, "rd MASK");

    mask &= ~(INT_BC_LVL | INT_COMP_CHNG);   // unmask both

    ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_MASK, mask),
                        TAG, "wr MASK SRC");

    return ESP_OK;
}

/* ---------------- Public functions (logic unchanged) ---------------- */

esp_err_t fusb302_init(const fusb302_hw_cfg_t *hw, fusb302_dev_t **out)
{
    if (!hw || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    fusb302_dev_t *dev = calloc(1, sizeof(*dev));
    if (!dev) {
        return ESP_ERR_NO_MEM;
    }
    dev->hw = *hw;
    dev->rp_current = (hw->rp_current <= ESP_TYPEC_RP_3A0) ?
                      hw->rp_current : ESP_TYPEC_RP_DEFAULT;
    *out = dev;

    uint8_t v;

    // Probe
    ESP_RETURN_ON_ERROR(i2c_rd8(hw->i2c_dev, REG_DEVICE_ID, &v), TAG, "probe");

    // Soft reset
    ESP_RETURN_ON_ERROR(i2c_wr8(hw->i2c_dev, REG_RESET, RST_SW_RESET), TAG, "reset");
    vTaskDelay(pdMS_TO_TICKS(2));

    // Power all relevant blocks
    ESP_RETURN_ON_ERROR(i2c_wr8(hw->i2c_dev, REG_POWER, PWR_PWR_ALL), TAG, "power");

    // Default role (sink/UFP) BEFORE unmasking INT
    ESP_RETURN_ON_ERROR(fusb302_set_role(dev, ESP_TYPEC_PWR_SINK), TAG, "set default role");

    fusb302_enable_irq(dev, true);

    ESP_LOGI(TAG, "init done");
    return ESP_OK;
}

esp_err_t fusb302_deinit(fusb302_dev_t *dev)
{
    esp_err_t ret = ESP_OK;
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_master_dev_handle_t d = dev->hw.i2c_dev;

    ESP_GOTO_ON_ERROR(fusb302_enable_irq(dev, false), fail, TAG, "disable irq");
    ESP_GOTO_ON_ERROR(i2c_wr8(d, REG_SWITCHES0, 0x00), fail, TAG, "SWITCHES0 Hi-Z");
    ESP_GOTO_ON_ERROR(i2c_wr8(d, REG_CONTROL2, CTL2_MODE_NONE), fail, TAG, "CONTROL2 mode none");
    ESP_GOTO_ON_ERROR(i2c_wr8(d, REG_POWER, PWR_PWR_LOW), fail, TAG, "power down");

fail:
    free(dev);
    return ret;
}

esp_err_t fusb302_set_role(fusb302_dev_t *dev, esp_typec_power_role_t role)
{
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_dev_handle_t d = dev->hw.i2c_dev;
    uint8_t value;

    switch (role) {
    case ESP_TYPEC_PWR_SINK: {  // Configure as Sink (UFP), manual
        ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_CONTROL2, CTL2_MODE_UFP),
                            TAG, "set UFP");

        /* Internal Rd on both CC pins */
        ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_SWITCHES0,
                                    SW0_CC1_PD_EN | SW0_CC2_PD_EN),
                            TAG, "SWITCHES0 Rd");

        ESP_LOGI(TAG, "FUSB302 set as Sink (UFP)");
        break;
    }

    case ESP_TYPEC_PWR_SOURCE: {  // Configure as Source (DFP, toggle)
        /*
         * DFP + TOGGLE:
         *  - FUSB302 toggling engine handles attach + polarity detection.
         *  - We just program advertised Rp current and let measure_status()
         *    read TOGSS to infer attached + cc2_active.
         */
        ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_CONTROL2,
                                    CTL2_MODE_DFP | CTL2_TOGGLE),
                            TAG, "set DFP+TOGGLE");

        /* Allow Rp on both CC pins (toggle engine will manage which is active) */
        value = SW0_CC1_PU_EN | SW0_CC2_PU_EN;
        ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_SWITCHES0, value),
                            TAG, "SWITCHES0 Rp");

        /* POWERROLE=1 (source) for PD */
        value = SW1_POWERROLE;
        ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_SWITCHES1, value),
                            TAG, "SWITCHES1 POWERROLE");

        /* Program advertised current into HOST_CUR */
        ESP_RETURN_ON_ERROR(i2c_rd8(d, REG_CONTROL0, &value),
                            TAG, "read CONTROL0");
        value &= ~CTL0_HOST_CUR_MASK;
        value |= rp_current_to_host_cur_bits(dev->rp_current);
        ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_CONTROL0, value),
                            TAG, "set HOST_CUR");

        /* MDAC / interrupt mask for source detection (helps toggle engine too) */
        ESP_RETURN_ON_ERROR(fusb302_config_src_detect(dev),
                            TAG, "config SRC detect");

        ESP_LOGI(TAG, "FUSB302 set as Source (toggle), Rp advert=%umA",
                 (unsigned)rp_current_to_ma(dev->rp_current));
        break;
    }

    case ESP_TYPEC_PWR_DRP: {  // Dual-Role (DRP toggle)
        /*
         * DRP + TOGGLE:
         *  - FUSB302 alternates between UFP/DFP, reports result in TOGSS.
         *  - Policy layer (later) can look at TOGSS to decide final role.
         */
        ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_CONTROL2,
                                    CTL2_MODE_DRP | CTL2_TOGGLE),
                            TAG, "set DRP+TOGGLE");

        /* Enable Rp when acting as DFP */
        value = SW0_CC1_PU_EN | SW0_CC2_PU_EN;
        ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_SWITCHES0, value),
                            TAG, "SWITCHES0 DRP Rp");

        /* HOST_CUR for DFP phases */
        ESP_RETURN_ON_ERROR(i2c_rd8(d, REG_CONTROL0, &value),
                            TAG, "read CONTROL0");
        value &= ~CTL0_HOST_CUR_MASK;
        value |= rp_current_to_host_cur_bits(dev->rp_current);
        ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_CONTROL0, value),
                            TAG, "set HOST_CUR DRP");

        /* Reuse same MDAC config as source */
        ESP_RETURN_ON_ERROR(fusb302_config_src_detect(dev),
                            TAG, "config DRP detect");

        ESP_LOGI(TAG, "FUSB302 set as DRP (toggle), Rp advert=%umA",
                 (unsigned)rp_current_to_ma(dev->rp_current));
        break;
    }

    default:
        return ESP_ERR_INVALID_ARG;
    }

    dev->role = role;
    return ESP_OK;
}

esp_err_t fusb302_service_irq(fusb302_dev_t *dev,
                              typec_evt_mask_t *events)
{
    if (!dev || !events) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_dev_handle_t d = dev->hw.i2c_dev;
    *events = 0;

    // Drain latched causes; exit when no causes AND pin is high
    for (int i = 0; i < MAX_INT_READS; ++i) {
        uint8_t i0 = 0, ia = 0, ib = 0;

        // Read-to-clear
        ESP_RETURN_ON_ERROR(i2c_rd8(d, REG_INTERRUPT,  &i0), TAG, "rd INT");
        (void)i2c_rd8(d, REG_INTERRUPTA, &ia);
        (void)i2c_rd8(d, REG_INTERRUPTB, &ib);

        if (i0 & (INT_BC_LVL | INT_COMP_CHNG)) {
            *events |= PD_EVT_CC;
        }
        if (ia & INTA_TOG_DONE) {
            *events |= PD_EVT_TOG;
        }
        if (i0 & INT_VBUSOK) {
            *events |= PD_EVT_VBUS;
        }
        if (ib & INTB_RX_SOP) {
            *events |= PD_EVT_RX;
        }

        ESP_LOGI(TAG, "IRQ: INT=0x%02x INTA=0x%02x INTB=0x%02x -> events=0x%02x",
                 i0, ia, ib, *events);
        // Done if nothing latched AND INT pin is high (deasserted)
        if ((i0 | ia | ib) == 0 && gpio_get_level(dev->hw.gpio_int) == 1) {
            break;
        }

        // Small breath to let the device clear internal latches
        esp_rom_delay_us(200);
    }
    return ESP_OK;
}

esp_err_t fusb302_enable_irq(fusb302_dev_t *dev, bool enable)
{
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_dev_handle_t d = dev->hw.i2c_dev;
    uint8_t v = 0;

    if (enable) {
        // INT pin enabled: CONTROL0.INT_MASK = 0
        ESP_RETURN_ON_ERROR(i2c_rd8(d, REG_CONTROL0, &v), TAG, "rd ctl0");
        v &= ~CTL0_INT_MASK;
        ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_CONTROL0, v), TAG, "wr ctl0");

        /* Clear any latched interrupts */
        uint8_t tmp;
        (void)i2c_rd8(d, REG_INTERRUPT,  &tmp);
        (void)i2c_rd8(d, REG_INTERRUPTA, &tmp);
        (void)i2c_rd8(d, REG_INTERRUPTB, &tmp);

        /* ---- Top-level mask (REG_MASK) ----
         * Unmask only:
         *   - INT_VBUSOK      (sink attach/detach)
         *   - INT_BC_LVL      (CC thresholds for source)
         *   - INT_COMP_CHNG   (CC comparator change)
         */
        uint8_t mask  = 0xFF;
        mask &= ~(INT_VBUSOK | INT_BC_LVL | INT_COMP_CHNG);
        ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_MASK, mask), TAG, "wr MASK");

        /* ---- REG_MASKA: only TOG_DONE for toggle/DFP/DRP ---- */
        uint8_t maska = 0xFF;
        maska &= ~INTA_TOG_DONE;
        ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_MASKA, maska), TAG, "wr MASKA");

        /* ---- REG_MASKB: only RX_SOP for PD RX (future) ---- */
        uint8_t maskb = 0xFF;
        maskb &= ~INTB_RX_SOP;
        ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_MASKB, maskb), TAG, "wr MASKB");

    } else {
        ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_MASK,  0xFF), TAG, "mask INT");
        ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_MASKA, 0xFF), TAG, "mask INTA");
        ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_MASKB, 0xFF), TAG, "mask INTB");

        ESP_RETURN_ON_ERROR(i2c_rd8(d, REG_CONTROL0, &v), TAG, "rd ctl0");
        v |= CTL0_INT_MASK;
        ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_CONTROL0, v), TAG, "wr ctl0");
    }

    return ESP_OK;
}

esp_err_t fusb302_get_status(fusb302_dev_t *dev, typec_cc_status_t *st_out)
{
    if (!dev || !st_out) {
        return ESP_ERR_INVALID_ARG;
    }
    return fusb302_measure_status(dev, st_out);
}
