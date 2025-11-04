/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "usb/uvc_host.h"
#include "uvc_types_priv.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JPEG_MARKER 0xFF
#define JPEG_SOI    0xD8

/**
 * @brief Allocate frame buffers for UVC stream
 *
 * @param[in] uvc_stream UVC stream handle
 * @param[in] nb_of_fb   Number of frame buffers to allocate
 * @param[in] fb_size    Size of 1 frame buffer in bytes
 * @param[in] fb_caps    Memory capabilities of memory for frame buffers
 * @param[in] user_frame_buffers Optional user-provided frame buffers. If not NULL, use these instead of allocating
 * @return
 *     - ESP_OK: Success
 *     - ESP_ERR_NO_MEM: Not enough memory for frame buffers
 *     - ESP_ERR_INVALID_ARG: Invalid count or size of frame buffers
 */
esp_err_t uvc_frame_allocate(uvc_stream_t *uvc_stream, int nb_of_fb, size_t fb_size, uint32_t fb_caps, uint8_t **user_frame_buffers);

/**
 * @brief Free allocated frame buffers
 *
 * @attention The caller must ensure that the frame buffers are not accessed after this call and that streaming is not on
 * @param[in] uvc_stream UVC stream
 */
void uvc_frame_free(uvc_stream_t *uvc_stream);

/**
 * @brief Check if all frame buffers are returned to this driver
 *
 * @param[in] uvc_stream UVC stream
 * @return
 *     - true:  All frames returned
 *     - false: All frames not returned
 */
bool uvc_frame_are_all_returned(uvc_stream_t *uvc_stream);

/**
 * @brief Get empty frame buffer
 *
 * @param[in] uvc_stream UVC stream
 * @return Pointer to empty frame buffer. Can be NULL if not frame buffer is available.
 */
uvc_host_frame_t *uvc_frame_get_empty(uvc_stream_t *uvc_stream);

/**
 * @brief Add data to the frame buffer
 *
 * @param[in] frame    Frame buffer
 * @param[in] data     Pointer to data
 * @param[in] data_len Data length in bytes
 * @return
 *     - ESP_OK: Data added to the frame buffer
 *     - ESP_ERR_INVALID_ARG: frame or data is NULL
 *     - ESP_ERR_INVALID_SIZE: Frame buffer overflow
 */
esp_err_t uvc_frame_add_data(uvc_host_frame_t *frame, const uint8_t *data, size_t data_len);

/**
 * @brief Reset a frame buffer
 *
 * All data in the frame buffer will be lost.
 *
 * @param[in] frame Frame buffer
 */
static inline void uvc_frame_reset(uvc_host_frame_t *frame)
{
    assert(frame);
    frame->data_len = 0;
}

/**
 * @brief Saves format to all frame buffers
 *
 * All frames must be returned when this function is called
 *
 * @param[in] uvc_stream UVC stream
 * @param[in] vs_format  VS format to save to frame buffers
 */
void uvc_frame_format_update(uvc_stream_t *uvc_stream, const uvc_host_stream_format_t *vs_format);

/**
 * @brief Check if UVC payload header is valid
 *
 * @param[in] hdr Pointer to UVC payload header
 * @param[in] packet_len Length of packet in bytes
 * @return
 *     - true:  Header is valid
 *     - false: Header is invalid
 */
bool uvc_frame_payload_header_validate(const uvc_payload_header_t *hdr, size_t packet_len);

#ifdef __cplusplus
}
#endif
