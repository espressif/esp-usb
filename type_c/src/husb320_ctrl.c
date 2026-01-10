/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "husb320_ctrl.h"

// TODO: replace with real register map
#define TAG "husb320"

struct husb320_dev {
    husb320_hw_cfg_t hw;
    esp_typec_power_role_t role;
};

esp_err_t husb320_init(const husb320_hw_cfg_t *hw, husb320_dev_t **out)
{
    if (!hw || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    husb320_dev_t *dev = calloc(1, sizeof(*dev));
    if (!dev) {
        return ESP_ERR_NO_MEM;
    }
    dev->hw   = *hw;
    dev->role = HUSB320_ROLE_SINK;

    // TODO: I2C probe, optional soft reset, clear INTs, enable CC measure
    ESP_LOGI(TAG, "init i2c=%d addr=0x%02x (polling)", hw->i2c_port, hw->i2c_addr);

    *out = dev;
    return ESP_OK;
}

void husb320_deinit(husb320_dev_t *dev)
{
    if (!dev) {
        return;
    }
    // TODO: put device into safe state if needed
    free(dev);
}

esp_err_t husb320_set_role(husb320_dev_t *dev, esp_typec_power_role_t role)
{
    if (!dev) {
        return ESP_ERR_INVALID_ARG;
    }
    dev->role = role;
    // TODO: write Rp/Rd/DRP selection to device
    ESP_LOGI(TAG, "set role=%d", (int)role);
    return ESP_OK;
}

esp_err_t husb320_read_cc_status(husb320_dev_t *dev, husb320_cc_status_t *st)
{
    if (!dev || !st) {
        return ESP_ERR_INVALID_ARG;
    }
    // TODO: read CC comparators / status regs and fill fields.
    // For now, report detached so the task runs harmlessly.
    memset(st, 0, sizeof(*st));
    return ESP_OK;
}
