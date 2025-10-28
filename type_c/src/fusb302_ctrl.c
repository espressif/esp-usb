/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdlib.h>
#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "fusb302_ctrl.h"

#define TAG "fusb302"

/* NOTE:
 * Start with polling (no INT), then enable IRQ when stable.
 */

struct fusb302_dev {
    fusb302_hw_cfg_t hw;
    fusb302_role_t role;
    // Optional: cache of last status, device id, etc.
    // uint8_t device_id;
};

/* FUSB302 registers (subset, correct masks) */
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

/* SWITCHES1 (not used for CC measure; it’s TX/datapath/specrev) */
#define SW1_POWERROLE   (1u << 7) /* Source=1, Sink=0 (used with PD PHY) */
#define SW1_SPECREV1    (1u << 6)
#define SW1_SPECREV0    (1u << 5)
#define SW1_DATAROLE    (1u << 4)
#define SW1_TXCC2_EN    (1u << 1)
#define SW1_TXCC1_EN    (1u << 0)

/* CONTROL2 MODE bits */
#define CTL2_MODE_MASK  (0x6)
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
#define ST0_BC_LVL_MASK     0x03    /* 0..3 -> thresholds */
#define ST0_BC_LVL_0_200    0x0
#define ST0_BC_LVL_200_600  0x1
#define ST0_BC_LVL_600_1230 0x2
#define ST0_BC_LVL_1230_MAX 0x3


static esp_err_t i2c_rd8(int port, uint8_t addr, uint8_t reg, uint8_t *val)
{
    if (!val) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_write_read_device(port, addr, &reg, 1, val, 1, pdMS_TO_TICKS(100));
}

static esp_err_t i2c_wr8(int port, uint8_t addr, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_write_to_device(port, addr, buf, sizeof(buf), pdMS_TO_TICKS(100));
}

static esp_err_t set_role_snk(fusb302_dev_t *dev)
{
    /* Rd on both CCs, measure CC1 initially, no Rp, no VCONN */
    uint8_t sw0 = SW0_CC1_PD_EN | SW0_CC2_PD_EN | SW0_MEAS_CC1;
    return i2c_wr8(dev->hw.i2c_port, dev->hw.i2c_addr, REG_SWITCHES0, sw0);
}


static esp_err_t fusb302_probe(fusb302_dev_t *dev)
{
    // Many FUSB302 variants expose DEVICE_ID at 0x01
    uint8_t reg = 0x01, val = 0x00;
    esp_err_t ret = i2c_master_write_read_device(dev->hw.i2c_port, dev->hw.i2c_addr,
                                                 &reg, 1, &val, 1, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "probe failed: port=%d addr=0x%02X (%s)",
                 dev->hw.i2c_port, dev->hw.i2c_addr, esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "probe ok: DEVICE_ID=0x%02X", val);
    return ESP_OK;
}

esp_err_t fusb302_init(const fusb302_hw_cfg_t *hw, fusb302_dev_t **out)
{
    esp_err_t ret = ESP_OK;
    if (!hw || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    fusb302_dev_t *dev = calloc(1, sizeof(*dev));
    if (!dev) {
        return ESP_ERR_NO_MEM;
    }
    dev->hw   = *hw;
    dev->role = FUSB302_ROLE_SINK;

    *out = dev; // set early so logs can use dev->hw
    ESP_RETURN_ON_ERROR(fusb302_probe(dev), TAG, "no i2c response");

    ESP_RETURN_ON_ERROR(i2c_wr8(dev->hw.i2c_port, dev->hw.i2c_addr, REG_RESET, RST_SW_RESET), TAG, "reset");
    vTaskDelay(pdMS_TO_TICKS(2)); // small settle time
    ESP_RETURN_ON_ERROR(i2c_wr8(dev->hw.i2c_port, dev->hw.i2c_addr, REG_RESET, RST_SW_RESET), TAG, "reset");

    /* Power up all blocks needed for CC comparator/measure logic */
    ESP_RETURN_ON_ERROR(i2c_wr8(dev->hw.i2c_port, dev->hw.i2c_addr, REG_POWER, PWR_PWR_ALL), TAG, "power all");

    /* Select UFP (Sink) mode in CONTROL2 for correct internal routing */
    ESP_RETURN_ON_ERROR(i2c_wr8(dev->hw.i2c_port, dev->hw.i2c_addr, REG_CONTROL2, CTL2_MODE_UFP), TAG, "mode=UFP");


    // Clear and then mask interrupts to only what we’ll check initially.
    uint8_t tmp;
    i2c_rd8(dev->hw.i2c_port, dev->hw.i2c_addr, REG_INTERRUPT, &tmp);
    i2c_rd8(dev->hw.i2c_port, dev->hw.i2c_addr, REG_INTERRUPTA, &tmp);
    i2c_rd8(dev->hw.i2c_port, dev->hw.i2c_addr, REG_INTERRUPTB, &tmp);
    // Unmask VBUSOK + BC_LVL (others masked)
    ESP_RETURN_ON_ERROR(i2c_wr8(dev->hw.i2c_port, dev->hw.i2c_addr, REG_MASK, 0x00), TAG, "mask0"); // simple: unmask base ints
    ESP_RETURN_ON_ERROR(i2c_wr8(dev->hw.i2c_port, dev->hw.i2c_addr, REG_INTERRUPTA, 0x00), TAG, "clr inta");
    ESP_RETURN_ON_ERROR(i2c_wr8(dev->hw.i2c_port, dev->hw.i2c_addr, REG_INTERRUPTB, 0x00), TAG, "clr intb");
    ESP_LOGI(TAG, "init i2c=%d addr=0x%02X (poll=%s)", hw->i2c_port, hw->i2c_addr, hw->use_intr ? "no" : "yes");
    ESP_RETURN_ON_ERROR(set_role_snk(dev), TAG, "role sink");

    *out = dev;
    return ret;
}

void fusb302_deinit(fusb302_dev_t *dev)
{
    if (!dev) {
        return;
    }
    // TODO: put device into a safe state if needed
    free(dev);
}

esp_err_t fusb302_set_role(fusb302_dev_t *dev, fusb302_role_t role)
{
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }

    dev->role = role;
    switch (role) {
    case FUSB302_ROLE_SINK:
        return set_role_snk(dev);
    case FUSB302_ROLE_SOURCE:
        // TODO: enable Rp (HOST_CURx) and disable Rd
        return ESP_ERR_NOT_SUPPORTED;
    case FUSB302_ROLE_DRP:
        // TODO: manual toggle or CONTROL2.TOGGLE
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t fusb302_service_irq(fusb302_dev_t *dev, bool *had_any)
{
    if (!dev || !had_any) {
        return ESP_ERR_INVALID_ARG;
    }
    // TODO: read & clear interrupt cause registers until empty
    *had_any = false;
    return ESP_OK;
}

static uint32_t rp_ma_from_bc_lvl(uint8_t bc_lvl)
{
    // 0: open / nothing; 1: Default USB (≈500/900mA); 2: 1.5A; 3: 3.0A
    switch (bc_lvl & 0x03) {
    case 1: return 500;   // conservative for default Rp
    case 2: return 1500;
    case 3: return 3000;
    default: return 0;
    }
}

esp_err_t fusb302_read_cc_status(fusb302_dev_t *dev, fusb302_cc_status_t *st)
{
    if (!dev || !st) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(st, 0, sizeof(*st));

    uint8_t s0 = 0;

    /* Ensure Rd remain enabled while switching measure channel */
    uint8_t base_sw0 = SW0_CC1_PD_EN | SW0_CC2_PD_EN;

    /* Measure CC1 */
    ESP_RETURN_ON_ERROR(i2c_wr8(dev->hw.i2c_port, dev->hw.i2c_addr, REG_SWITCHES0, base_sw0 | SW0_MEAS_CC1), TAG, "meas cc1");
    ESP_RETURN_ON_ERROR(i2c_rd8(dev->hw.i2c_port, dev->hw.i2c_addr, REG_STATUS0, &s0), TAG, "rd STATUS0 cc1");
    uint8_t bclvl_cc1 = (s0 & ST0_BC_LVL_MASK);
    if (bclvl_cc1) {
        ESP_LOGD(TAG, "CC1: BC_LVL=%d", bclvl_cc1);
    }
    /* Measure CC2 */
    ESP_RETURN_ON_ERROR(i2c_wr8(dev->hw.i2c_port, dev->hw.i2c_addr, REG_SWITCHES0, base_sw0 | SW0_MEAS_CC2), TAG, "meas cc2");
    ESP_RETURN_ON_ERROR(i2c_rd8(dev->hw.i2c_port, dev->hw.i2c_addr, REG_STATUS0, &s0), TAG, "rd STATUS0 cc2");
    uint8_t bclvl_cc2 = (s0 & ST0_BC_LVL_MASK);
    if (bclvl_cc2) {
        ESP_LOGD(TAG, "CC2: BC_LVL=%d", bclvl_cc2);
    }
    /* Optional VBUS qualifier */
    bool vbus_ok = (s0 & ST0_VBUSOK) != 0;

    bool cc1_has_rp = (bclvl_cc1 != ST0_BC_LVL_0_200);
    bool cc2_has_rp = (bclvl_cc2 != ST0_BC_LVL_0_200);

    /* For a standard C-to-C connection, exactly one CC must see Rp */
    bool attached = (cc1_has_rp ^ cc2_has_rp);
    /* If you want to be conservative: attached &= vbus_ok; */

    st->attached   = attached;
    st->cc2_active = attached ? cc2_has_rp : false;

    /* Map advertised current based on the ACTIVE CC */
    uint8_t bclvl = cc2_has_rp ? bclvl_cc2 : bclvl_cc1;
    switch (bclvl) {
    case ST0_BC_LVL_0_200:     st->rp_cur_ma = 0;    break; /* no Rp */
    case ST0_BC_LVL_200_600:   st->rp_cur_ma = 500;  break; /* Default (500mA) */
    case ST0_BC_LVL_600_1230:  st->rp_cur_ma = 1500; break; /* 1.5A */
    case ST0_BC_LVL_1230_MAX:  st->rp_cur_ma = 3000; break; /* 3.0A */
    default:                   st->rp_cur_ma = 0;    break;
    }

    return ESP_OK;
}
