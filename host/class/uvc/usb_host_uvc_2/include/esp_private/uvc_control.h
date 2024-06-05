/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "usb/uvc_host.h"


/**
 * @brief Submit transfer to CTRL endpoint
 *
 * Sends Control transfer as described in USB specification chapter 9.
 * This function can be used by device drivers that use custom/vendor specific commands.
 *
 * @param        stream_hdl    UVC stream
 * @param[in]    bmRequestType Field of USB control request
 * @param[in]    bRequest      Field of USB control request
 * @param[in]    wValue        Field of USB control request
 * @param[in]    wIndex        Field of USB control request
 * @param[in]    wLength       Field of USB control request
 * @param[inout] data          Field of USB control request
 * @return
 *     - ESP_OK: Transfer succeeded
 *     - ESP_ERR_TIMEOUT: The transfer could not be completed in time
 *     - ESP_ERR_INVALID_SIZE: The transfer is too big
 *     - ESP_ERR_INVALID_ARG: stream_hdl is NULL, or data is not NULL and wValue is greater than zero
 *     - ESP_ERR_INVALID_RESPONSE: Reply corrupted or too short
 */
esp_err_t uvc_host_usb_ctrl(uvc_host_stream_hdl_t stream_hdl, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex, uint16_t wLength, uint8_t *data);

/**
 * @brief Negotiate UVC stream format
 *
 * @param      stream_hdl UVC stream
 * @param[in]  vs_format  Requested Video Stream format
 * @param[out] vs_result  Negotiation result
 * @return esp_err_t
 */
esp_err_t uvc_host_stream_control_negotiate(uvc_host_stream_hdl_t stream_hdl, const uvc_host_stream_format_t *vs_format, uvc_vs_ctrl_t *vs_result);
