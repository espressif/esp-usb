/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdlib.h>
#include "esp_err.h"
#include "esp_log.h"
#include "husb320_ctrl.h"

// Experimental stub; replace with real register map and logic.
static const char *TAG = "husb320";

/**
 * @brief HUSB320 device context.
 */
struct husb320_dev {
    husb320_hw_cfg_t hw;
    usb_tcpm_power_role_t role;
};

static esp_err_t husb320_init(const husb320_hw_cfg_t *hw, husb320_dev_t **out)
{
    if (!hw || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    husb320_dev_t *device = calloc(1, sizeof(*device));
    if (!device) {
        return ESP_ERR_NO_MEM;
    }
    device->hw   = *hw;
    device->role = USB_TCPM_PWR_SINK;

    // TODO: I2C probe, optional soft reset, clear INTs, enable CC measure
    ESP_LOGI(TAG, "init i2c=%d addr=0x%02x (polling)", hw->i2c_port, hw->i2c_addr);

    *out = device;
    return ESP_OK;
}

static void husb320_deinit(husb320_dev_t *device)
{
    if (!device) {
        return;
    }
    // TODO: put device into safe state if needed
    free(device);
}

static esp_err_t husb320_set_role(husb320_dev_t *device, usb_tcpm_power_role_t role)
{
    if (!device) {
        return ESP_ERR_INVALID_ARG;
    }
    device->role = role;
    // TODO: write Rp/Rd/DRP selection to device
    ESP_LOGI(TAG, "set role=%d", (int)role);
    return ESP_OK;
}

static esp_err_t husb320_read_cc_status(const husb320_dev_t *device, husb320_cc_status_t *status)
{
    if (!device || !status) {
        return ESP_ERR_INVALID_ARG;
    }
    // TODO: read CC comparators / status regs and fill fields.
    // For now, report detached so the task runs harmlessly.
    memset(status, 0, sizeof(*status));
    return ESP_OK;
}
