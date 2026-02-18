/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "usb/fusb302.h"
#include "usb/usb_tcpm.h"

#include "usb_tcpm_backend.h"

static const char *TAG = "fusb302";

#define MAX_INT_READS 8 /* Maximum number of interrupt register reads to avoid infinite loops */
#define SRC_ATTACH_DEBOUNCE_MS 10

typedef struct fusb302_dev fusb302_dev_t;

typedef struct {
    i2c_master_dev_handle_t i2c_dev; /**< REQUIRED: device handle from esp_driver_i2c */
    gpio_num_t gpio_int;             /**< REQUIRED: GPIO number for FUSB302 INT (active-low) */
    usb_tcpm_rp_current_t rp_current;/**< Advertised Rp current when sourcing */
} fusb302_hw_cfg_t;

/**
 * @brief FUSB302 device context structure.
 */
struct fusb302_dev {
    fusb302_hw_cfg_t hw;             /**< Hardware configuration */
    usb_tcpm_power_role_t role;      /**< Current power role */
    usb_tcpm_rp_current_t rp_current;/**< Advertised Rp current when sourcing */
};

/**
 * @brief Backend context for a Type-C port using FUSB302.
 */
typedef struct {
    usb_tcpm_fusb302_config_t hw_cfg; /**< Hardware config from public API */
    i2c_master_dev_handle_t i2c_dev;  /**< I2C device handle */
    fusb302_dev_t *device;               /**< FUSB302 device handle */
} fusb302_port_ctx_t;

/* Internal device functions */
static esp_err_t fusb302_commit_attach(const fusb302_dev_t *device, bool cc2_active, bool is_source);
static esp_err_t fusb302_init(const fusb302_hw_cfg_t *hw, fusb302_dev_t **out);
static esp_err_t fusb302_deinit(fusb302_dev_t *device);
static esp_err_t fusb302_set_role(fusb302_dev_t *device, usb_tcpm_power_role_t role);
static esp_err_t fusb302_service_irq(const fusb302_dev_t *device, typec_evt_mask_t *events);
static esp_err_t fusb302_enable_irq(const fusb302_dev_t *device, bool enable);
static esp_err_t fusb302_get_status(const fusb302_dev_t *device, typec_cc_status_t *status);

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
#define CTL2_MODE_MASK  (0x3u << 1)
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
static inline uint8_t rp_current_to_host_cur_bits(usb_tcpm_rp_current_t rp)
{
    switch (rp) {
    case USB_TCPM_RP_1A5:
        return CTL0_HOST_CUR_MEDIUM;
    case USB_TCPM_RP_3A0:
        return CTL0_HOST_CUR_HIGH;
    case USB_TCPM_RP_DEFAULT:
    default:
        return CTL0_HOST_CUR_DEFAULT;
    }
}

static inline uint32_t rp_current_to_ma(usb_tcpm_rp_current_t rp)
{
    switch (rp) {
    case USB_TCPM_RP_1A5:
        return 1500;
    case USB_TCPM_RP_3A0:
        return 3000;
    case USB_TCPM_RP_DEFAULT:
    default:
        return 500;
    }
}

static inline uint8_t rp_current_to_mdac(usb_tcpm_rp_current_t rp)
{
    switch (rp) {
    case USB_TCPM_RP_3A0:
        return 0x3E;
    case USB_TCPM_RP_1A5:
    case USB_TCPM_RP_DEFAULT:
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

// Write 8-bit value to a FUSB302 register via I2C.
static inline esp_err_t i2c_wr8(i2c_master_dev_handle_t device, uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(device, buf, sizeof(buf), 100 /*ms*/);
}

// Read 8-bit value from a FUSB302 register via I2C.
static inline esp_err_t i2c_rd8(i2c_master_dev_handle_t device, uint8_t reg, uint8_t *val)
{
    if (!val) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_transmit_receive(device, &reg, 1, val, 1, 100);
}

static esp_err_t fusb302_set_host_cur(const fusb302_dev_t *device, uint8_t host_cur_bits)
{
    if (!device) {
        return ESP_ERR_INVALID_ARG;
    }

    const i2c_master_dev_handle_t i2c_dev = device->hw.i2c_dev;
    uint8_t ctl0 = 0;

    ESP_RETURN_ON_ERROR(i2c_rd8(i2c_dev, REG_CONTROL0, &ctl0), TAG, "read CONTROL0");
    ctl0 = (ctl0 & ~CTL0_HOST_CUR_MASK) | host_cur_bits;
    return i2c_wr8(i2c_dev, REG_CONTROL0, ctl0);
}

static esp_err_t fusb302_toggle_disable(const fusb302_dev_t *device)
{
    if (!device) {
        return ESP_ERR_INVALID_ARG;
    }

    const i2c_master_dev_handle_t i2c_dev = device->hw.i2c_dev;
    uint8_t ctl2 = 0;

    ESP_RETURN_ON_ERROR(i2c_rd8(i2c_dev, REG_CONTROL2, &ctl2), TAG, "read CONTROL2");
    if (ctl2 & CTL2_TOGGLE) {
        ESP_RETURN_ON_ERROR(i2c_wr8(i2c_dev, REG_CONTROL2, ctl2 & ~CTL2_TOGGLE), TAG, "disable toggle");
    }
    return ESP_OK;
}

static esp_err_t fusb302_toggle_enable(const fusb302_dev_t *device)
{
    const i2c_master_dev_handle_t i2c_dev = device->hw.i2c_dev;
    uint8_t ctl2;

    ESP_RETURN_ON_ERROR(i2c_rd8(i2c_dev, REG_CONTROL2, &ctl2), TAG, "read CONTROL2");

    const uint8_t base = ctl2 & ~CTL2_TOGGLE;

    // Clear any latched toggle-related causes
    uint8_t tmp;
    (void)i2c_rd8(i2c_dev, REG_INTERRUPTA, &tmp);
    (void)i2c_rd8(i2c_dev, REG_INTERRUPT,  &tmp);
    (void)i2c_rd8(i2c_dev, REG_INTERRUPTB, &tmp);

    // Force a clean 0 -> 1 edge
    ESP_RETURN_ON_ERROR(i2c_wr8(i2c_dev, REG_CONTROL2, base), TAG, "disable toggle");
    esp_rom_delay_us(50);
    ESP_RETURN_ON_ERROR(i2c_wr8(i2c_dev, REG_CONTROL2, base | CTL2_TOGGLE), TAG, "enable toggle");

    return ESP_OK;
}

static esp_err_t fusb302_apply_polarity(const fusb302_dev_t *device, bool cc2_active, bool is_source)
{
    if (!device) {
        return ESP_ERR_INVALID_ARG;
    }

    const i2c_master_dev_handle_t i2c_dev = device->hw.i2c_dev;

    if (is_source) {
        uint8_t sw1 = 0;
        ESP_RETURN_ON_ERROR(i2c_rd8(i2c_dev, REG_SWITCHES1, &sw1), TAG, "read SWITCHES1");
        sw1 = (sw1 & ~(SW1_TXCC1_EN | SW1_TXCC2_EN)) | SW1_POWERROLE |
              (cc2_active ? SW1_TXCC2_EN : SW1_TXCC1_EN);
        ESP_RETURN_ON_ERROR(i2c_wr8(i2c_dev, REG_SWITCHES1, sw1), TAG, "write SWITCHES1 TXCC");
    }

    uint8_t sw0 = 0;
    ESP_RETURN_ON_ERROR(i2c_rd8(i2c_dev, REG_SWITCHES0, &sw0), TAG, "read SWITCHES0");
    sw0 = (sw0 & ~(SW0_MEAS_CC1 | SW0_MEAS_CC2)) |
          (cc2_active ? SW0_MEAS_CC2 : SW0_MEAS_CC1);
    ESP_RETURN_ON_ERROR(i2c_wr8(i2c_dev, REG_SWITCHES0, sw0), TAG, "write SWITCHES0 MEAS");
    esp_rom_delay_us(50);

    return ESP_OK;
}

/* ---------------- Internal device functions ---------------- */

static esp_err_t fusb302_commit_attach(const fusb302_dev_t *device, bool cc2_active, bool is_source)
{
    if (!device) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Stop autonomous toggle before committing orientation/role. */
    ESP_RETURN_ON_ERROR(fusb302_toggle_disable(device), TAG, "disable toggle");
    ESP_RETURN_ON_ERROR(fusb302_apply_polarity(device, cc2_active, is_source), TAG, "apply polarity");

    if (is_source) {
        /* Advertise the configured Rp current after attach. */
        ESP_RETURN_ON_ERROR(fusb302_set_host_cur(device, rp_current_to_host_cur_bits(device->rp_current)),
                            TAG, "set HOST_CUR attach");
        ESP_RETURN_ON_ERROR(i2c_wr8(device->hw.i2c_dev, REG_MEASURE,
                                    rp_current_to_mdac(device->rp_current)),
                            TAG, "set MDAC attach");
        /* Debounce to avoid false detach during the first few ms after attach. */
        vTaskDelay(pdMS_TO_TICKS(SRC_ATTACH_DEBOUNCE_MS));
    }
    return ESP_OK;
}

static esp_err_t fusb302_init(const fusb302_hw_cfg_t *hw, fusb302_dev_t **out)
{
    if (!hw || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_OK;
    fusb302_dev_t *device = calloc(1, sizeof(*device));
    if (!device) {
        return ESP_ERR_NO_MEM;
    }
    device->hw = *hw;
    device->rp_current = (hw->rp_current <= USB_TCPM_RP_3A0) ?
                         hw->rp_current : USB_TCPM_RP_DEFAULT;
    *out = NULL;

    uint8_t device_id;

    // Probe
    ESP_GOTO_ON_ERROR(i2c_rd8(hw->i2c_dev, REG_DEVICE_ID, &device_id), fail, TAG, "probe");
    ESP_LOGI(TAG, "FUSB302 DEVICE_ID=0x%02x", device_id);

    // Soft reset
    ESP_GOTO_ON_ERROR(i2c_wr8(hw->i2c_dev, REG_RESET, RST_SW_RESET), fail, TAG, "reset");
    vTaskDelay(pdMS_TO_TICKS(10));

    // Power all relevant blocks
    ESP_GOTO_ON_ERROR(i2c_wr8(hw->i2c_dev, REG_POWER, PWR_PWR_ALL), fail, TAG, "power");

    ESP_GOTO_ON_ERROR(fusb302_enable_irq(device, true), fail, TAG, "enable irq");
    ESP_LOGI(TAG, "init done");
    *out = device;
    return ESP_OK;

fail:
    free(device);
    return ret;
}

static esp_err_t fusb302_deinit(fusb302_dev_t *device)
{
    esp_err_t ret = ESP_OK;
    if (!device) {
        return ESP_ERR_INVALID_ARG;
    }
    const i2c_master_dev_handle_t i2c_dev = device->hw.i2c_dev;

    ESP_GOTO_ON_ERROR(fusb302_enable_irq(device, false), fail, TAG, "disable irq");
    ESP_GOTO_ON_ERROR(i2c_wr8(i2c_dev, REG_SWITCHES0, 0x00), fail, TAG, "SWITCHES0 Hi-Z");
    ESP_GOTO_ON_ERROR(i2c_wr8(i2c_dev, REG_CONTROL2, CTL2_MODE_NONE), fail, TAG, "CONTROL2 mode none");
    ESP_GOTO_ON_ERROR(i2c_wr8(i2c_dev, REG_POWER, PWR_PWR_LOW), fail, TAG, "power down");

fail:
    free(device);
    return ret;
}

static esp_err_t fusb302_set_role(fusb302_dev_t *device, usb_tcpm_power_role_t role)
{
    if (!device) {
        return ESP_ERR_INVALID_ARG;
    }

    const i2c_master_dev_handle_t i2c_dev = device->hw.i2c_dev;
    uint8_t value;

    switch (role) {
    case USB_TCPM_PWR_SINK: {
        uint8_t prev_status1a = 0;
        uint8_t prev_togss = 0;
        const bool from_drp = (device->role == USB_TCPM_PWR_DRP);

        if (from_drp) {
            (void)i2c_rd8(i2c_dev, REG_STATUS1A, &prev_status1a);
            prev_togss = (prev_status1a >> ST1A_TOGSS_SHIFT) & 0x07;

            /*
             * In DRP this path runs after TOG_DONE in the common attach flow.
             * Preload Rd so manual pull state is correct as soon as control
             * returns to SWITCHES0 bits while leaving autonomous toggle mode.
             */
            ESP_RETURN_ON_ERROR(i2c_wr8(i2c_dev, REG_SWITCHES0, SW0_CC1_PD_EN | SW0_CC2_PD_EN),
                                TAG, "SWITCHES0 Rd preload");
        }

        ESP_RETURN_ON_ERROR(i2c_wr8(i2c_dev, REG_CONTROL2, CTL2_MODE_UFP), TAG, "set UFP mode");

        if (!from_drp) {
            /* Rd on both CC pins */
            ESP_RETURN_ON_ERROR(i2c_wr8(i2c_dev, REG_SWITCHES0, SW0_CC1_PD_EN | SW0_CC2_PD_EN),
                                TAG, "SWITCHES0 Rd");
        }

        const bool already_snk = from_drp &&
                                 (prev_togss == ST1A_TOGSS_SNK1 || prev_togss == ST1A_TOGSS_SNK2);
        if (!already_snk) {
            /* Start/ensure toggle engine running in UFP mode */
            ESP_RETURN_ON_ERROR(fusb302_toggle_enable(device), TAG, "enable toggle (UFP)");
        }

        ESP_LOGI(TAG, "FUSB302 set as Sink (UFP+TOGGLE)");
        break;
    }

    case USB_TCPM_PWR_SOURCE: {
        uint8_t prev_status1a = 0;
        uint8_t prev_togss = 0;
        const bool from_drp = (device->role == USB_TCPM_PWR_DRP);

        if (from_drp) {
            (void)i2c_rd8(i2c_dev, REG_STATUS1A, &prev_status1a);
            prev_togss = (prev_status1a >> 3) & 0x07;
        }
        const bool already_src = from_drp &&
                                 (prev_togss == ST1A_TOGSS_SRC1 || prev_togss == ST1A_TOGSS_SRC2);

        // Set DFP mode WITHOUT toggle first
        ESP_RETURN_ON_ERROR(i2c_wr8(i2c_dev, REG_CONTROL2, CTL2_MODE_DFP), TAG, "set DFP mode");

        /* Rp on both CC pins, clear any Rd leftover from sink */
        ESP_RETURN_ON_ERROR(i2c_wr8(i2c_dev, REG_SWITCHES0, SW0_CC1_PU_EN | SW0_CC2_PU_EN),
                            TAG, "SWITCHES0 Rp");

        if (!already_src) {
            /* Use Default Rp while toggling for reliable TOG_DONE */
            ESP_RETURN_ON_ERROR(fusb302_set_host_cur(device, CTL0_HOST_CUR_DEFAULT), TAG, "set HOST_CUR toggle");
            ESP_RETURN_ON_ERROR(i2c_wr8(device->hw.i2c_dev, REG_MEASURE,
                                        rp_current_to_mdac(USB_TCPM_RP_DEFAULT)),
                                TAG, "set MDAC toggle");
        }

        /* POWERROLE=1, TXCC disabled until polarity is applied */
        uint8_t sw1 = 0;
        ESP_RETURN_ON_ERROR(i2c_rd8(i2c_dev, REG_SWITCHES1, &sw1), TAG, "read SWITCHES1");
        sw1 = (sw1 & ~(SW1_TXCC1_EN | SW1_TXCC2_EN)) | SW1_POWERROLE;
        ESP_RETURN_ON_ERROR(i2c_wr8(i2c_dev, REG_SWITCHES1, sw1), TAG, "write SWITCHES1 role");

        if (!already_src) {
            // Not already attached: arm toggle normally
            ESP_RETURN_ON_ERROR(fusb302_toggle_enable(device), TAG, "enable toggle");
        }

        ESP_LOGI(TAG, "FUSB302 set as Source (DFP+TOGGLE)");
        break;
    }

    case USB_TCPM_PWR_DRP: {  // Dual-Role (DRP toggle)
        /*
         * DRP + TOGGLE:
         *  - FUSB302 alternates between UFP/DFP, reports result in TOGSS.
         *  - Policy layer (later) can look at TOGSS to decide final role.
         */
        /* Set DRP mode first; then force a clean 0->1 toggle edge. */
        ESP_RETURN_ON_ERROR(i2c_wr8(i2c_dev, REG_CONTROL2, CTL2_MODE_DRP),
                            TAG, "set DRP");

        /* Enable Rp when acting as DFP */
        value = SW0_CC1_PU_EN | SW0_CC2_PU_EN;
        ESP_RETURN_ON_ERROR(i2c_wr8(i2c_dev, REG_SWITCHES0, value),
                            TAG, "SWITCHES0 DRP Rp");

        /* HOST_CUR for DFP phases */
        ESP_RETURN_ON_ERROR(i2c_rd8(i2c_dev, REG_CONTROL0, &value),
                            TAG, "read CONTROL0");
        value &= ~CTL0_HOST_CUR_MASK;
        value |= CTL0_HOST_CUR_DEFAULT;
        ESP_RETURN_ON_ERROR(i2c_wr8(i2c_dev, REG_CONTROL0, value),
                            TAG, "set HOST_CUR DRP");

        /* Use Default Rp MDAC while toggling (datasheet toggle example). */
        ESP_RETURN_ON_ERROR(i2c_wr8(device->hw.i2c_dev, REG_MEASURE,
                                    rp_current_to_mdac(USB_TCPM_RP_DEFAULT)),
                            TAG, "set MDAC DRP (default)");

        /* Ensure toggle restarts even if it was already set in a prior role. */
        ESP_RETURN_ON_ERROR(fusb302_toggle_enable(device),
                            TAG, "toggle arm (DRP)");

        ESP_LOGI(TAG, "FUSB302 set as DRP (TOGGLE), Rp advert=%umA",
                 (unsigned)rp_current_to_ma(device->rp_current));
        break;
    }

    default:
        return ESP_ERR_INVALID_ARG;
    }

    device->role = role;
    return ESP_OK;
}

static esp_err_t fusb302_service_irq(const fusb302_dev_t *device,
                                     typec_evt_mask_t *events)
{
    if (!device || !events) {
        return ESP_ERR_INVALID_ARG;
    }

    const i2c_master_dev_handle_t i2c_dev = device->hw.i2c_dev;
    *events = 0;

    // Drain latched causes; exit when no causes AND pin is high
    for (int read_idx = 0; read_idx < MAX_INT_READS; ++read_idx) {
        uint8_t intr_reg = 0;
        uint8_t intr_a_reg = 0;
        uint8_t intr_b_reg = 0;

        // Read-to-clear
        ESP_RETURN_ON_ERROR(i2c_rd8(i2c_dev, REG_INTERRUPT, &intr_reg), TAG, "read INTERRUPT");
        (void)i2c_rd8(i2c_dev, REG_INTERRUPTA, &intr_a_reg);
        (void)i2c_rd8(i2c_dev, REG_INTERRUPTB, &intr_b_reg);

        if (intr_reg & INT_COMP_CHNG) {
            *events |= TYPEC_EVT_CC;
        }
        if (intr_a_reg & INTA_TOG_DONE) {
            *events |= TYPEC_EVT_TOG;
        }
        if (intr_reg & INT_VBUSOK) {
            *events |= TYPEC_EVT_VBUS;
        }
        if (intr_b_reg & INTB_RX_SOP) {
            *events |= TYPEC_EVT_RX;
        }

        ESP_LOGD(TAG, "IRQ: INT=0x%02x INTA=0x%02x INTB=0x%02x -> events=0x%02x",
                 intr_reg, intr_a_reg, intr_b_reg, *events);
        // Done if nothing latched AND INT pin is high (deasserted)
        if ((intr_reg | intr_a_reg | intr_b_reg) == 0 && gpio_get_level(device->hw.gpio_int) == 1) {
            break;
        }

        // Small breath to let the device clear internal latches
        esp_rom_delay_us(200);
    }
    return ESP_OK;
}

static esp_err_t fusb302_enable_irq(const fusb302_dev_t *device, bool enable)
{
    if (!device) {
        return ESP_ERR_INVALID_ARG;
    }

    const i2c_master_dev_handle_t i2c_dev = device->hw.i2c_dev;
    uint8_t control0 = 0;

    if (!enable) {
        ESP_RETURN_ON_ERROR(i2c_wr8(i2c_dev, REG_MASK,  0xFF), TAG, "mask INTERRUPT");
        ESP_RETURN_ON_ERROR(i2c_wr8(i2c_dev, REG_MASKA, 0xFF), TAG, "mask INTERRUPTA");
        ESP_RETURN_ON_ERROR(i2c_wr8(i2c_dev, REG_MASKB, 0xFF), TAG, "mask INTERRUPTB");

        ESP_RETURN_ON_ERROR(i2c_rd8(i2c_dev, REG_CONTROL0, &control0), TAG, "read CONTROL0");
        control0 |= CTL0_INT_MASK;
        ESP_RETURN_ON_ERROR(i2c_wr8(i2c_dev, REG_CONTROL0, control0), TAG, "write CONTROL0");
        return ESP_OK;
    }

    // INT pin enabled: CONTROL0.INT_MASK = 0
    ESP_RETURN_ON_ERROR(i2c_rd8(i2c_dev, REG_CONTROL0, &control0), TAG, "read CONTROL0");
    control0 &= ~CTL0_INT_MASK;
    ESP_RETURN_ON_ERROR(i2c_wr8(i2c_dev, REG_CONTROL0, control0), TAG, "write CONTROL0");

    // Clear any latched interrupts (read-to-clear)
    uint8_t tmp;
    (void)i2c_rd8(i2c_dev, REG_INTERRUPT,  &tmp);
    (void)i2c_rd8(i2c_dev, REG_INTERRUPTA, &tmp);
    (void)i2c_rd8(i2c_dev, REG_INTERRUPTB, &tmp);

    // REG_MASK: only what we actually use
    uint8_t mask = 0xFF;
    mask &= ~INT_VBUSOK;        // sink attach/detach, useful globally
    mask &= ~INT_COMP_CHNG;     // source detach (after MEAS points to active CC)

    ESP_RETURN_ON_ERROR(i2c_wr8(i2c_dev, REG_MASK, mask), TAG, "write MASK");

    // REG_MASKA: TOG_DONE for toggle attach/orientation
    uint8_t maska = 0xFF;
    maska &= ~INTA_TOG_DONE;
    ESP_RETURN_ON_ERROR(i2c_wr8(i2c_dev, REG_MASKA, maska), TAG, "write MASKA");

    // REG_MASKB: RX_SOP when RX handling is enabled
    uint8_t maskb = 0xFF;
    maskb &= ~INTB_RX_SOP;
    ESP_RETURN_ON_ERROR(i2c_wr8(i2c_dev, REG_MASKB, maskb), TAG, "write MASKB");

    return ESP_OK;
}

static esp_err_t fusb302_get_status(const fusb302_dev_t *device, typec_cc_status_t *status)
{
    if (!device || !status) {
        return ESP_ERR_INVALID_ARG;
    }

    const i2c_master_dev_handle_t i2c_dev = device->hw.i2c_dev;
    memset(status, 0, sizeof(*status));

    status->tog_result = TYPEC_TOG_RESULT_NONE;

    uint8_t status0 = 0;
    uint8_t status1a = 0;

    ESP_RETURN_ON_ERROR(i2c_rd8(i2c_dev, REG_STATUS1A, &status1a), TAG, "read STATUS1A");
    const uint8_t togss = (status1a >> ST1A_TOGSS_SHIFT) & 0x07;

    const bool src_att = (togss == ST1A_TOGSS_SRC1) || (togss == ST1A_TOGSS_SRC2);
    const bool snk_att = (togss == ST1A_TOGSS_SNK1) || (togss == ST1A_TOGSS_SNK2);

    if (src_att) {
        status->tog_result = TYPEC_TOG_RESULT_SRC;
    } else if (snk_att) {
        status->tog_result = TYPEC_TOG_RESULT_SNK;
    }

    status->attached = src_att || snk_att;
    status->cc2_active = (togss == ST1A_TOGSS_SRC2) || (togss == ST1A_TOGSS_SNK2);

    ESP_RETURN_ON_ERROR(i2c_rd8(i2c_dev, REG_STATUS0, &status0), TAG, "read STATUS0");
    status->vbus_ok = (status0 & ST0_VBUSOK) != 0;

    if (device->role == USB_TCPM_PWR_SINK) {
        uint8_t bclvl = (status0 & ST0_BC_LVL_MASK);
        if (status->attached && status->vbus_ok) {
            esp_rom_delay_us(1000);
            ESP_RETURN_ON_ERROR(i2c_rd8(i2c_dev, REG_STATUS0, &status0), TAG, "read STATUS0 retry");
            status->vbus_ok = (status0 & ST0_VBUSOK) != 0;
            bclvl = (status0 & ST0_BC_LVL_MASK);
        }
        status->rp_current_ma = bclvl_to_ma(bclvl);
        return ESP_OK;
    }

    status->rp_current_ma = rp_current_to_ma(device->rp_current);
    if (snk_att) {
        status->rp_current_ma = 0;
    }

    if (device->role == USB_TCPM_PWR_SOURCE) {
        /* COMP=0 => below MDAC => Rd present */
        const bool rd_present = ((status0 & ST0_COMP) == 0);
        status->attached = rd_present;
    }

    return ESP_OK;
}

/* ---------------- Backend glue & factory ---------------- */

static esp_err_t fusb302_backend_init(void *ctx, const usb_tcpm_port_config_t *cfg)
{
    if (!ctx || !cfg) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_OK;
    fusb302_port_ctx_t *port = (fusb302_port_ctx_t *)ctx;

    const i2c_device_config_t dev_cfg = {
        .device_address = port->hw_cfg.i2c_addr_7b,
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .scl_speed_hz = 400000, // default to 400 kHz
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(port->hw_cfg.i2c_bus, &dev_cfg, &port->i2c_dev),
                        TAG, "fusb add failed");

    const fusb302_hw_cfg_t hw = {
        .i2c_dev   = port->i2c_dev,
        .gpio_int  = port->hw_cfg.gpio_int,
        .rp_current = cfg->rp_current,
    };

    ESP_GOTO_ON_ERROR(fusb302_init(&hw, &port->device), fail, TAG, "fusb302_init failed");
    return ESP_OK;

fail:
    if (port->i2c_dev) {
        i2c_master_bus_rm_device(port->i2c_dev);
        port->i2c_dev = NULL;
    }
    return ret;
}

static esp_err_t fusb302_backend_deinit(void *ctx)
{
    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }

    fusb302_port_ctx_t *port = (fusb302_port_ctx_t *)ctx;
    esp_err_t ret = ESP_OK;

    if (port->device) {
        ESP_GOTO_ON_ERROR(fusb302_deinit(port->device), clear_device, TAG, "fusb302_deinit failed");
clear_device:
        port->device = NULL;
    }

    if (port->i2c_dev) {
        if (ret == ESP_OK) {
            ESP_GOTO_ON_ERROR(i2c_master_bus_rm_device(port->i2c_dev), clear_i2c_dev, TAG,
                              "fusb302 remove i2c device failed");
        } else {
            (void)i2c_master_bus_rm_device(port->i2c_dev);
        }
clear_i2c_dev:
        port->i2c_dev = NULL;
    }

    free(port);
    return ret;
}

static esp_err_t fusb302_backend_set_role(void *ctx, usb_tcpm_power_role_t role)
{
    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }
    const fusb302_port_ctx_t *port = (const fusb302_port_ctx_t *)ctx;
    if (!port->device) {
        return ESP_ERR_INVALID_STATE;
    }
    return fusb302_set_role(port->device, role);
}

static esp_err_t fusb302_backend_service_irq(void *ctx, typec_evt_mask_t *events)
{
    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }
    const fusb302_port_ctx_t *port = (const fusb302_port_ctx_t *)ctx;
    if (!port->device) {
        return ESP_ERR_INVALID_STATE;
    }
    return fusb302_service_irq(port->device, events);
}

static esp_err_t fusb302_backend_get_status(void *ctx, typec_cc_status_t *status)
{
    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }
    const fusb302_port_ctx_t *port = (const fusb302_port_ctx_t *)ctx;
    if (!port->device) {
        return ESP_ERR_INVALID_STATE;
    }
    return fusb302_get_status(port->device, status);
}

static esp_err_t fusb302_backend_commit_attach(void *ctx, bool cc2_active, bool is_source)
{
    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }
    const fusb302_port_ctx_t *port = (const fusb302_port_ctx_t *)ctx;
    if (!port->device) {
        return ESP_ERR_INVALID_STATE;
    }
    return fusb302_commit_attach(port->device, cc2_active, is_source);
}

static esp_err_t fusb302_backend_get_irq_gpio(void *ctx, gpio_num_t *gpio_num)
{
    if (!ctx || !gpio_num) {
        return ESP_ERR_INVALID_ARG;
    }
    const fusb302_port_ctx_t *port = (const fusb302_port_ctx_t *)ctx;
    *gpio_num = port->hw_cfg.gpio_int;
    return ESP_OK;
}

static const typec_port_backend_t s_fusb302_backend = {
    .init = fusb302_backend_init,
    .deinit = fusb302_backend_deinit,
    .set_role = fusb302_backend_set_role,
    .service_irq = fusb302_backend_service_irq,
    .get_status = fusb302_backend_get_status,
    .commit_attach = fusb302_backend_commit_attach,
    .get_irq_gpio = fusb302_backend_get_irq_gpio,
};

esp_err_t usb_tcpm_port_create_fusb302(const usb_tcpm_port_config_t *port_cfg,
                                       const usb_tcpm_fusb302_config_t *hw_cfg,
                                       usb_tcpm_port_handle_t *port_hdl_ret)
{
    ESP_RETURN_ON_FALSE(port_cfg && hw_cfg && port_hdl_ret,
                        ESP_ERR_INVALID_ARG, TAG, "bad args");

    fusb302_port_ctx_t *ctx = (fusb302_port_ctx_t *)calloc(1, sizeof(*ctx));
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_NO_MEM, TAG, "no mem");

    ctx->hw_cfg = *hw_cfg;

    esp_err_t ret = ESP_OK;
    typec_port_handle_t port = NULL;
    ESP_GOTO_ON_ERROR(typec_port_new(&s_fusb302_backend, ctx, port_cfg, &port),
                      fail, TAG, "typec_port_new failed");

    *port_hdl_ret = (usb_tcpm_port_handle_t)port;
    return ESP_OK;

fail:
    (void)fusb302_backend_deinit(ctx);
    return ret;
}
