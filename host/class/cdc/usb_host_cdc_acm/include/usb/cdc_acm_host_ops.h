/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "usb/cdc_host_types.h"
#include "usb/usb_types_cdc.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief SetLineCoding function
 *
 * @see Chapter 6.3.10, USB CDC-PSTN specification rev. 1.2
 *
 * @param     cdc_hdl     CDC handle obtained from cdc_acm_host_open()
 * @param[in] line_coding Line Coding structure
 * @return esp_err_t
 */
esp_err_t cdc_acm_host_line_coding_set(cdc_acm_dev_hdl_t cdc_hdl, const cdc_acm_line_coding_t *line_coding);

/**
 * @brief GetLineCoding function
 *
 * @see Chapter 6.3.11, USB CDC-PSTN specification rev. 1.2
 *
 * @param      cdc_hdl     CDC handle obtained from cdc_acm_host_open()
 * @param[out] line_coding Line Coding structure to be filled
 * @return esp_err_t
 */
esp_err_t cdc_acm_host_line_coding_get(cdc_acm_dev_hdl_t cdc_hdl, cdc_acm_line_coding_t *line_coding);

/**
 * @brief SetControlLineState function
 *
 * @see Chapter 6.3.12, USB CDC-PSTN specification rev. 1.2
 *
 * @param     cdc_hdl CDC handle obtained from cdc_acm_host_open()
 * @param[in] dtr     Indicates to DCE if DTE is present or not. This signal corresponds to V.24 signal 108/2 and RS-232 signal Data Terminal Ready.
 * @param[in] rts     Carrier control for half duplex modems. This signal corresponds to V.24 signal 105 and RS-232 signal Request To Send.
 * @return esp_err_t
 */
esp_err_t cdc_acm_host_set_control_line_state(cdc_acm_dev_hdl_t cdc_hdl, bool dtr, bool rts);

/**
 * @brief SendBreak function
 *
 * This function will block until the duration_ms has passed.
 *
 * @see Chapter 6.3.13, USB CDC-PSTN specification rev. 1.2
 *
 * @param     cdc_hdl     CDC handle obtained from cdc_acm_host_open()
 * @param[in] duration_ms Duration of the Break signal in [ms]
 * @return esp_err_t
 */
esp_err_t cdc_acm_host_send_break(cdc_acm_dev_hdl_t cdc_hdl, uint16_t duration_ms);

#ifdef __cplusplus
}
#endif
