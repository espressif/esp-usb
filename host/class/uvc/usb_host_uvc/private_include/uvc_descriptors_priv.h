/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

// In this file we want to have clean interface for descriptor parsing
// So we include only files with USB specification definitions
// This interface is also used in host_tests
#include "usb/usb_types_ch9.h"
#include "usb/usb_types_uvc.h"

#define UVC_DESC_FPS_TO_DWFRAMEINTERVAL(fps) (((fps) != 0) ? 10000000.0f / (fps) : 0)
#define UVC_DESC_DWFRAMEINTERVAL_TO_FPS(dwFrameInterval) (((dwFrameInterval) != 0) ? 10000000.0f / ((float)(dwFrameInterval)) : 0)

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Helper to convert UVC format desc to this driver format
 *
 * @param[in] format_desc UVC format descriptor
 * @return enum uvc_host_stream_format Format enum
 */
int uvc_desc_parse_format(const uvc_format_desc_t *format_desc);

esp_err_t uvc_desc_get_streaming_interface_num(
    const usb_config_desc_t *cfg_desc,
    uint8_t uvc_index,
    const uvc_host_stream_format_t *vs_format,
    uint16_t *bcdUVC,
    uint8_t *bInterfaceNumber);

/**
 * @brief Get Streaming Interface and Endpoint descriptors
 *
 * We go through all alternate interfaces and pick the one that offers endpoint with MPS that:
 * * Is lower than or equal to dwMaxPayloadTransferSize
 * * Has as few as possible extra transactions per microframe (HS only)
 *
 * @note The caller is responsible for dwMaxPayloadTransferSize fitting in the IN FIFO
 *
 * @param[in] cfg_desc                 Configuration descriptor
 * @param[in] bInterfaceNumber         Index of Streaming interface
 * @param[in] dwMaxPayloadTransferSize Maximum requested MPS
 * @param[out] intf_desc_ret           Interface descriptor
 * @param[out] ep_desc_ret             Endpoint descriptor
 * @return
 *     - ESP_OK: Success
 *     - ESP_ERR_INVALID_ARG: cfg_desc, intf_desc_ret or ep_desc_ret is NULL
 *     - ESP_ERR_NOT_FOUND: Could not find interface with required parameters
 */
esp_err_t uvc_desc_get_streaming_intf_and_ep(
    const usb_config_desc_t *cfg_desc,
    uint8_t bInterfaceNumber,
    uint16_t dwMaxPayloadTransferSize,
    const usb_intf_desc_t **intf_desc_ret,
    const usb_ep_desc_t **ep_desc_ret);

esp_err_t uvc_desc_get_frame_format_by_index(
    const usb_config_desc_t *cfg_desc,
    uint8_t bInterfaceNumber,
    uint8_t bFormatIndex,
    uint8_t bFrameIndex,
    const uvc_format_desc_t **format_desc_ret,
    const uvc_frame_desc_t **frame_desc_ret);

esp_err_t uvc_desc_get_frame_format_by_format(
    const usb_config_desc_t *cfg_desc,
    uint8_t bInterfaceNumber,
    const uvc_host_stream_format_t *vs_format,
    const uvc_format_desc_t **format_desc_ret,
    const uvc_frame_desc_t **frame_desc_ret);

#ifdef __cplusplus
}
#endif
