/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "usb/uvc_host.h"
#include "uvc_types_priv.h"

/**
 * @brief Allocate frame buffers for UVC stream
 *
 * @param[in] stream_hdl UVC stream handle
 * @param[in] nb_of_fb   Number of frame buffers to allocate
 * @param[in] fb_size    Size of 1 frame buffer in bytes
 * @return
 *     - ESP_OK: Success
 *     - ESP_ERR_NO_MEM: Not enough memory for frame buffers
 *     - ESP_ERR_INVALID_ARG: Invalid count or size of frame buffers
 */
esp_err_t uvc_frame_allocate(uvc_stream_t *uvc_stream, int nb_of_fb, size_t fb_size);

/**
 * @brief Free allocated frame buffers
 *
 * @attention The caller must ensure that the frame buffers are not accessed after this call and that streaming is not on
 * @param[in] stream_hdl UVC stream
 */
void uvc_frame_free(uvc_stream_t *uvc_stream);

bool uvc_frame_are_all_returned(uvc_stream_t *uvc_stream);

uvc_frame_t *uvc_frame_get_empty(uvc_stream_t *uvc_stream);

esp_err_t uvc_frame_add_data(uvc_frame_t *frame, const uint8_t *data, size_t data_len);

void uvc_frame_reset(uvc_frame_t *frame);
