/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
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
#include "esp_typec.h"

#define TAG "fusb302"

#define MAX_INT_READS 8 /* Maximum number of interrupt register reads to avoid infinite loops */
#define SRC_ATTACH_DEBOUNCE_MS 3

/**
 * @brief FUSB302 device context structure.
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
#define CTL0_HOST_CUR_DEFAULT  (0x1u << 2)       /* 01b -> Default */
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

/* REG_INTERRUPT (0x42) — top-level CC/VBUS causes */
#define INT_BC_LVL      (1u << 0)
#define INT_COLLISION   (1u << 1)
#define INT_WAKE        (1u << 2)
#define INT_ALERT       (1u << 3)
#define INT_CRC_CHK     (1u << 4)
#define INT_COMP_CHNG   (1u << 5)
#define INT_ACTIVITY    (1u << 6)
#define INT_VBUSOK      (1u << 7)

/* REG_INTERRUPTA (0x3E) — RX/Toggle outcomes (mapping varies by rev) */
#define INTA_TX_SUCC    (1u << 1)
#define INTA_RETRY_FAIL (1u << 2)
#define INTA_SOFT_FAIL  (1u << 3)
#define INTA_HARD_SENT  (1u << 5)
#define INTA_TOG_DONE   (1u << 6)

/* REG_INTERRUPTB (0x3F) — RX, resets, FIFO (mapping varies by rev) */
#define INTB_GCRCSENT    (1u << 0)
#define INTB_RX_SOP      (1u << 1)
#define INTB_RX_ANY_MASK 0x1F

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

static inline uint8_t rp_current_to_mdac(esp_typec_rp_current_t rp)
{
    switch (rp) {
    case ESP_TYPEC_RP_3A0:
        return 0x3E;
    case ESP_TYPEC_RP_1A5:
    case ESP_TYPEC_RP_DEFAULT:
    default:
        return 0x26;
    }
}

static inline uint32_t bclvl_to_ma(uint8_t bclvl)
{
    switch (bclvl) {
    case ST0_BC_LVL_200_600:
        return 500;
    case ST0_BC_LVL_600_1230:
        return 1500;
    case ST0_BC_LVL_1230_MAX:
        return 3000;
    default:
        return 0;
    }
}

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

static esp_err_t fusb302_set_host_cur(fusb302_dev_t *dev, uint8_t host_cur_bits)
{
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_dev_handle_t d = dev->hw.i2c_dev;
    uint8_t ctl0 = 0;

    ESP_RETURN_ON_ERROR(i2c_rd8(d, REG_CONTROL0, &ctl0), TAG, "read CONTROL0");
    ctl0 = (ctl0 & ~CTL0_HOST_CUR_MASK) | host_cur_bits;
    return i2c_wr8(d, REG_CONTROL0, ctl0);
}

static esp_err_t fusb302_toggle_disable(fusb302_dev_t *dev)
{
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_dev_handle_t d = dev->hw.i2c_dev;
    uint8_t ctl2 = 0;

    ESP_RETURN_ON_ERROR(i2c_rd8(d, REG_CONTROL2, &ctl2), TAG, "rd CONTROL2");
    if (ctl2 & CTL2_TOGGLE) {
        ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_CONTROL2, ctl2 & ~CTL2_TOGGLE), TAG, "toggle=0");
    }
    return ESP_OK;
}

static esp_err_t fusb302_toggle_enable(fusb302_dev_t *dev)
{
    i2c_master_dev_handle_t d = dev->hw.i2c_dev;
    uint8_t ctl2;

    ESP_RETURN_ON_ERROR(i2c_rd8(d, REG_CONTROL2, &ctl2), TAG, "rd CONTROL2");

    uint8_t base = ctl2 & ~CTL2_TOGGLE;

    // Clear any latched toggle-related causes
    uint8_t tmp;
    (void)i2c_rd8(d, REG_INTERRUPTA, &tmp);
    (void)i2c_rd8(d, REG_INTERRUPT,  &tmp);
    (void)i2c_rd8(d, REG_INTERRUPTB, &tmp);

    // Force a clean 0 -> 1 edge
    ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_CONTROL2, base), TAG, "toggle=0");
    esp_rom_delay_us(50);
    ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_CONTROL2, base | CTL2_TOGGLE), TAG, "toggle=1");

    return ESP_OK;
}

static esp_err_t fusb302_apply_polarity(fusb302_dev_t *dev, bool cc2_active, bool is_source)
{
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_dev_handle_t d = dev->hw.i2c_dev;

    if (is_source) {
        uint8_t sw1 = 0;
        ESP_RETURN_ON_ERROR(i2c_rd8(d, REG_SWITCHES1, &sw1), TAG, "rd SW1");
        sw1 = (sw1 & ~(SW1_TXCC1_EN | SW1_TXCC2_EN)) | SW1_POWERROLE |
              (cc2_active ? SW1_TXCC2_EN : SW1_TXCC1_EN);
        ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_SWITCHES1, sw1), TAG, "wr SW1 TXCC");
    }

    uint8_t sw0 = 0;
    ESP_RETURN_ON_ERROR(i2c_rd8(d, REG_SWITCHES0, &sw0), TAG, "rd SW0");
    sw0 = (sw0 & ~(SW0_MEAS_CC1 | SW0_MEAS_CC2)) |
          (cc2_active ? SW0_MEAS_CC2 : SW0_MEAS_CC1);
    ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_SWITCHES0, sw0), TAG, "wr SW0 MEAS");
    esp_rom_delay_us(50);

    return ESP_OK;
}

/* ---------------- Public functions ---------------- */

esp_err_t fusb302_commit_attach(fusb302_dev_t *dev, bool cc2_active, bool is_source)
{
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Stop autonomous toggle before committing orientation/role. */
    ESP_RETURN_ON_ERROR(fusb302_toggle_disable(dev), TAG, "toggle=0");
    ESP_RETURN_ON_ERROR(fusb302_apply_polarity(dev, cc2_active, is_source), TAG, "apply polarity");

    if (is_source) {
        /* Advertise the configured Rp current after attach. */
        ESP_RETURN_ON_ERROR(fusb302_set_host_cur(dev, rp_current_to_host_cur_bits(dev->rp_current)),
                            TAG, "set HOST_CUR attach");
        ESP_RETURN_ON_ERROR(i2c_wr8(dev->hw.i2c_dev, REG_MEASURE,
                                    rp_current_to_mdac(dev->rp_current)),
                            TAG, "set MDAC attach");
        /* Debounce to avoid false detach during the first few ms after attach. */
        vTaskDelay(pdMS_TO_TICKS(SRC_ATTACH_DEBOUNCE_MS));
    }
    return ESP_OK;
}

esp_err_t fusb302_init(const fusb302_hw_cfg_t *hw, fusb302_dev_t **out)
{
    if (!hw || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_OK;
    fusb302_dev_t *dev = calloc(1, sizeof(*dev));
    if (!dev) {
        return ESP_ERR_NO_MEM;
    }
    dev->hw = *hw;
    dev->rp_current = (hw->rp_current <= ESP_TYPEC_RP_3A0) ?
                      hw->rp_current : ESP_TYPEC_RP_DEFAULT;
    *out = NULL;

    uint8_t v;

    // Probe
    ESP_GOTO_ON_ERROR(i2c_rd8(hw->i2c_dev, REG_DEVICE_ID, &v), fail, TAG, "probe");

    // Soft reset
    ESP_GOTO_ON_ERROR(i2c_wr8(hw->i2c_dev, REG_RESET, RST_SW_RESET), fail, TAG, "reset");
    vTaskDelay(pdMS_TO_TICKS(2));

    // Power all relevant blocks
    ESP_GOTO_ON_ERROR(i2c_wr8(hw->i2c_dev, REG_POWER, PWR_PWR_ALL), fail, TAG, "power");

    ESP_GOTO_ON_ERROR(fusb302_set_role(dev, dev->role), fail, TAG, "set default role");
    ESP_GOTO_ON_ERROR(fusb302_enable_irq(dev, true), fail, TAG, "enable irq");
    ESP_LOGI(TAG, "init done");
    *out = dev;
    return ESP_OK;

fail:
    free(dev);
    return ret;
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
    case ESP_TYPEC_PWR_SINK: {
        uint8_t prev_status1a = 0;
        uint8_t prev_togss = 0;
        bool from_drp = (dev->role == ESP_TYPEC_PWR_DRP);

        if (from_drp) {
            (void)i2c_rd8(d, REG_STATUS1A, &prev_status1a);
            prev_togss = (prev_status1a >> ST1A_TOGSS_SHIFT) & 0x07;
        }

        ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_CONTROL2, CTL2_MODE_UFP), TAG, "set UFP");

        /* Rd on both CC pins */
        ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_SWITCHES0, SW0_CC1_PD_EN | SW0_CC2_PD_EN),
                            TAG, "SWITCHES0 Rd");

        bool already_snk = from_drp &&
                           (prev_togss == ST1A_TOGSS_SNK1 || prev_togss == ST1A_TOGSS_SNK2);
        if (!already_snk) {
            /* Start/ensure toggle engine running in UFP mode */
            ESP_RETURN_ON_ERROR(fusb302_toggle_enable(dev), TAG, "toggle arm (UFP)");
        }

        ESP_LOGI(TAG, "FUSB302 set as Sink (UFP+TOGGLE)");
        break;
    }

    case ESP_TYPEC_PWR_SOURCE: {
        uint8_t prev_status1a = 0;
        uint8_t prev_togss = 0;
        bool from_drp = (dev->role == ESP_TYPEC_PWR_DRP);
        bool already_src = false;

        if (from_drp) {
            (void)i2c_rd8(d, REG_STATUS1A, &prev_status1a);
            prev_togss = (prev_status1a >> 3) & 0x07;
            already_src = (prev_togss == ST1A_TOGSS_SRC1 || prev_togss == ST1A_TOGSS_SRC2);
        }

        // Set DFP mode WITHOUT toggle first
        ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_CONTROL2, CTL2_MODE_DFP), TAG, "set DFP");

        /* Rp on both CC pins, clear any Rd leftover from sink */
        ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_SWITCHES0, SW0_CC1_PU_EN | SW0_CC2_PU_EN),
                            TAG, "SWITCHES0 Rp");

        if (!already_src) {
            /* Use Default Rp while toggling for reliable TOG_DONE */
            ESP_RETURN_ON_ERROR(fusb302_set_host_cur(dev, CTL0_HOST_CUR_DEFAULT), TAG, "set HOST_CUR toggle");
            ESP_RETURN_ON_ERROR(i2c_wr8(dev->hw.i2c_dev, REG_MEASURE,
                                        rp_current_to_mdac(ESP_TYPEC_RP_DEFAULT)),
                                TAG, "set MDAC toggle");
        }

        /* POWERROLE=1, TXCC disabled until polarity is applied */
        uint8_t sw1 = 0;
        ESP_RETURN_ON_ERROR(i2c_rd8(d, REG_SWITCHES1, &sw1), TAG, "rd SW1");
        sw1 = (sw1 & ~(SW1_TXCC1_EN | SW1_TXCC2_EN)) | SW1_POWERROLE;
        ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_SWITCHES1, sw1), TAG, "wr SW1 role");

        if (!already_src) {
            // Not already attached: arm toggle normally
            ESP_RETURN_ON_ERROR(fusb302_toggle_enable(dev), TAG, "toggle arm");
        }

        ESP_LOGI(TAG, "FUSB302 set as Source (DFP+TOGGLE)");
        break;
    }

    case ESP_TYPEC_PWR_DRP: {  // Dual-Role (DRP toggle)
        /*
         * DRP + TOGGLE:
         *  - FUSB302 alternates between UFP/DFP, reports result in TOGSS.
         *  - Policy layer (later) can look at TOGSS to decide final role.
         */
        /* Set DRP mode first; then force a clean 0->1 toggle edge. */
        ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_CONTROL2, CTL2_MODE_DRP),
                            TAG, "set DRP");

        /* Enable Rp when acting as DFP */
        value = SW0_CC1_PU_EN | SW0_CC2_PU_EN;
        ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_SWITCHES0, value),
                            TAG, "SWITCHES0 DRP Rp");

        /* HOST_CUR for DFP phases */
        ESP_RETURN_ON_ERROR(i2c_rd8(d, REG_CONTROL0, &value),
                            TAG, "read CONTROL0");
        value &= ~CTL0_HOST_CUR_MASK;
        value |= CTL0_HOST_CUR_DEFAULT;
        ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_CONTROL0, value),
                            TAG, "set HOST_CUR DRP");

        /* Use Default Rp MDAC while toggling (datasheet toggle example). */
        ESP_RETURN_ON_ERROR(i2c_wr8(dev->hw.i2c_dev, REG_MEASURE,
                                    rp_current_to_mdac(ESP_TYPEC_RP_DEFAULT)),
                            TAG, "set MDAC DRP (default)");

        /* Ensure toggle restarts even if it was already set in a prior role. */
        ESP_RETURN_ON_ERROR(fusb302_toggle_enable(dev),
                            TAG, "toggle arm (DRP)");

        ESP_LOGI(TAG, "FUSB302 set as DRP (TOGGLE), Rp advert=%umA",
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

        if (i0 & INT_COMP_CHNG) {
            *events |= TYPEC_EVT_CC;
        }
        if (ia & INTA_TOG_DONE) {
            *events |= TYPEC_EVT_TOG;
        }
        if (i0 & INT_VBUSOK) {
            *events |= TYPEC_EVT_VBUS;
        }
        if (ib & INTB_RX_SOP) {
            *events |= TYPEC_EVT_RX;
        }

        ESP_LOGD(TAG, "IRQ: INT=0x%02x INTA=0x%02x INTB=0x%02x -> events=0x%02x",
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

    if (!enable) {
        ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_MASK,  0xFF), TAG, "mask INT");
        ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_MASKA, 0xFF), TAG, "mask INTA");
        ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_MASKB, 0xFF), TAG, "mask INTB");

        ESP_RETURN_ON_ERROR(i2c_rd8(d, REG_CONTROL0, &v), TAG, "rd ctl0");
        v |= CTL0_INT_MASK;
        ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_CONTROL0, v), TAG, "wr ctl0");
        return ESP_OK;
    }

    // INT pin enabled: CONTROL0.INT_MASK = 0
    ESP_RETURN_ON_ERROR(i2c_rd8(d, REG_CONTROL0, &v), TAG, "rd ctl0");
    v &= ~CTL0_INT_MASK;
    ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_CONTROL0, v), TAG, "wr ctl0");

    // Clear any latched interrupts (read-to-clear)
    uint8_t tmp;
    (void)i2c_rd8(d, REG_INTERRUPT,  &tmp);
    (void)i2c_rd8(d, REG_INTERRUPTA, &tmp);
    (void)i2c_rd8(d, REG_INTERRUPTB, &tmp);

    // REG_MASK: only what we actually use
    uint8_t mask = 0xFF;
    mask &= ~INT_VBUSOK;        // sink attach/detach, useful globally
    mask &= ~INT_COMP_CHNG;     // source detach (after MEAS points to active CC)

    ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_MASK, mask), TAG, "wr MASK");

    // REG_MASKA: TOG_DONE for toggle attach/orientation
    uint8_t maska = 0xFF;
    maska &= ~INTA_TOG_DONE;
    ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_MASKA, maska), TAG, "wr MASKA");

    // REG_MASKB: RX_SOP when RX handling is enabled
    uint8_t maskb = 0xFF;
    maskb &= ~INTB_RX_SOP;
    ESP_RETURN_ON_ERROR(i2c_wr8(d, REG_MASKB, maskb), TAG, "wr MASKB");

    return ESP_OK;
}

esp_err_t fusb302_get_status(fusb302_dev_t *dev, typec_cc_status_t *st)
{
    if (!dev || !st) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_dev_handle_t d = dev->hw.i2c_dev;
    memset(st, 0, sizeof(*st));

    st->tog_result = TYPEC_TOG_RESULT_NONE;

    uint8_t status0 = 0;
    uint8_t status1a = 0;

    ESP_RETURN_ON_ERROR(i2c_rd8(d, REG_STATUS1A, &status1a), TAG, "rd STATUS1A");
    uint8_t togss = (status1a >> ST1A_TOGSS_SHIFT) & 0x07;

    bool src_att = (togss == ST1A_TOGSS_SRC1) || (togss == ST1A_TOGSS_SRC2);
    bool snk_att = (togss == ST1A_TOGSS_SNK1) || (togss == ST1A_TOGSS_SNK2);

    if (src_att) {
        st->tog_result = TYPEC_TOG_RESULT_SRC;
    } else if (snk_att) {
        st->tog_result = TYPEC_TOG_RESULT_SNK;
    }

    st->attached = src_att || snk_att;
    st->cc2_active = (togss == ST1A_TOGSS_SRC2) || (togss == ST1A_TOGSS_SNK2);

    ESP_RETURN_ON_ERROR(i2c_rd8(d, REG_STATUS0, &status0), TAG, "rd STATUS0");
    st->vbus_ok = (status0 & ST0_VBUSOK) != 0;

    if (dev->role == ESP_TYPEC_PWR_SINK) {
        uint8_t bclvl = (status0 & ST0_BC_LVL_MASK);
        if (st->attached && st->vbus_ok) {
            esp_rom_delay_us(1000);
            ESP_RETURN_ON_ERROR(i2c_rd8(d, REG_STATUS0, &status0), TAG, "rd STATUS0 retry");
            st->vbus_ok = (status0 & ST0_VBUSOK) != 0;
            bclvl = (status0 & ST0_BC_LVL_MASK);
        }
        st->rp_cur_ma = bclvl_to_ma(bclvl);
        return ESP_OK;
    }

    st->rp_cur_ma = rp_current_to_ma(dev->rp_current);
    if (snk_att) {
        st->rp_cur_ma = 0;
    }

    if (dev->role == ESP_TYPEC_PWR_SOURCE) {
        /* COMP=0 => below MDAC => Rd present */
        bool rd_present = ((status0 & ST0_COMP) == 0);
        st->attached = rd_present;
    }

    return ESP_OK;
}
