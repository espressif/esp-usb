/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "usb/usb_types_ch9.h"
#include "usb/uvc_host.h"
#include "usb/usb_types_uvc.h"
#include "uvc_types_priv.h"

#define UVC_DESC_FPS_TO_DWFRAMEINTERVAL(fps) (((fps) != 0) ? 10000000 / (fps) : 0)
#define UVC_DESC_DWFRAMEINTERVAL_TO_FPS(dwFrameInterval) (((dwFrameInterval) != 0) ? 10000000 / (dwFrameInterval) : 0)

// uvc_idx: For USB devices that offer multiple UVC functions. Set to 0 for all simple cameras.
const uvc_vc_header_desc_t *uvc_desc_get_control_interface_header(uvc_stream_t *uvc_stream, unsigned uvc_idx);

const uvc_vs_input_header_desc_t *uvc_desc_get_streaming_input_header(uvc_stream_t *uvc_stream, uint8_t bInterfaceNumber);

int uvc_desc_parse_format(const uvc_format_desc_t *format_desc);

esp_err_t uvc_desc_get_streaming_intf_and_ep(
    uvc_stream_t *uvc_stream,
    uint8_t bInterfaceNumber,
    const usb_intf_desc_t **intf_desc_ret,
    const usb_ep_desc_t **ep_desc_ret);

esp_err_t uvc_desc_get_frame_format_by_index(
    uvc_stream_t *uvc_stream,
    uint8_t bInterfaceNumber,
    uint8_t bFormatIndex,
    uint8_t bFrameIndex,
    const uvc_format_desc_t **format_desc_ret,
    const uvc_frame_desc_t **frame_desc_ret);

esp_err_t uvc_desc_get_frame_format_by_format(
    uvc_stream_t *uvc_stream,
    uint8_t bInterfaceNumber,
    const uvc_host_stream_format_t *vs_format,
    const uvc_format_desc_t **format_desc_ret,
    const uvc_frame_desc_t **frame_desc_ret);

bool uvc_desc_is_format_supported(
    uvc_stream_t *uvc_stream,
    uint8_t bInterfaceNumber,
    const uvc_host_stream_format_t *vs_format);
