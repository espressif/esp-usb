/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Simple CC status snapshot for FUSB302.
 * @param attached True when exactly one CC detects Rp
 * @param cc2_active True if CC2 is the active/connected CC
 * @param rp_cur_ma Advertised Rp current (0 / 500 / 1500 / 3000 mA)
 */
typedef struct {
    bool     attached;    /**< True when exactly one CC detects Rp */
    bool     cc2_active;  /**< True if CC2 is the active/connected CC */
    uint32_t rp_cur_ma;   /**< 0 / 500 / 1500 / 3000 (mA) */
    bool     vbus_ok;
} typec_cc_status_t;

typedef enum {
    PD_EVT_CC    = (1u << 0),  /* CC comparator / BC_LVL change */
    PD_EVT_VBUS  = (1u << 1),  /* VBUSOK change */
    PD_EVT_RX    = (1u << 2),  /* PD message available */
    PD_EVT_HRST  = (1u << 3),  /* Hard reset detected */
    PD_EVT_FAULT = (1u << 4),  /* OCP/OTP/etc. */
    PD_EVT_TOG   = (1u << 5),  /* Toggle state change (TOG_DONE) */
} typec_evt_mask_t;
