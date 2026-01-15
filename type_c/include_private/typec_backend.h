/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TYPEC_TOG_RESULT_NONE = 0, /**< Toggle engine still running / no result */
    TYPEC_TOG_RESULT_SRC,      /**< Partner is a sink; we should act as source */
    TYPEC_TOG_RESULT_SNK,      /**< Partner is a source; we should act as sink  */
} typec_tog_result_t;

/**
 * @brief Simple CC status snapshot for FUSB302.
 * @param attached True when exactly one CC detects Rp (or toggle result says attached)
 * @param cc2_active True if CC2 is the active/connected CC
 * @param rp_cur_ma Advertised Rp current (0 / 500 / 1500 / 3000 mA)
 * @param vbus_ok True if VBUSOK is asserted
 * @param tog_result Result from toggle engine (if any)
 */
typedef struct {
    bool     attached;    /**< True when exactly one CC detects Rp */
    bool     cc2_active;  /**< True if CC2 is the active/connected CC */
    uint32_t rp_cur_ma;   /**< 0 / 500 / 1500 / 3000 (mA) */
    bool     vbus_ok;
    typec_tog_result_t tog_result; /**< Result from toggle engine (if any) */
} typec_cc_status_t;

/**
 * @brief Backend event mask bits returned by the Type-C controller.
 */
typedef enum {
    TYPEC_EVT_CC    = (1u << 0),  /* CC comparator / BC_LVL change */
    TYPEC_EVT_VBUS  = (1u << 1),  /* VBUSOK change */
    TYPEC_EVT_RX    = (1u << 2),  /* RX message available (reserved) */
    TYPEC_EVT_HRST  = (1u << 3),  /* Hard reset detected */
    TYPEC_EVT_FAULT = (1u << 4),  /* OCP/OTP/etc. */
    TYPEC_EVT_TOG   = (1u << 5),  /* Toggle state change (TOG_DONE) */
} typec_evt_mask_t;
