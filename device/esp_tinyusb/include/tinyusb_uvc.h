/*
 * SPDX-FileCopyrightText: 2022-2026 Espressif Systems (Shanghai) CO LTD
 *                         2026 Daniel Kampert (DanielKampert@Kampis-Elektroecke.de)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <esp_err.h>

#include <stdint.h>
#include <stdbool.h>

#include <sdkconfig.h>

#if (CONFIG_TINYUSB_UVC_ENABLED != 1)
#error "TinyUSB UVC driver must be enabled in menuconfig"
#endif

/** @brief UVC camera interfaces available.
 */
typedef enum {
    TINYUSB_UVC_ITF_0 = 0,
#if CONFIG_TINYUSB_UVC_SUPPORT_TWO_CAM
    TINYUSB_UVC_ITF_1,
#endif
    TINYUSB_UVC_ITF_MAX
} tinyusb_uvc_itf_t;

/** @brief Types of UVC events.
 */
typedef enum {
    UVC_EVENT_FRAME_START,      /*!< Frame transmission started */
    UVC_EVENT_FRAME_END,        /*!< Frame transmission ended */
    UVC_EVENT_STREAMING_START,  /*!< Streaming started by host */
    UVC_EVENT_STREAMING_STOP,   /*!< Streaming stopped by host */
} uvc_event_type_t;

/** @brief Describes an event passing to the input of a callback
 */
typedef struct {
    uvc_event_type_t type;      /*!< Event type */
} uvc_event_t;

/** @brief UVC callback type
 */
typedef void(*tusb_uvc_callback_t)(int itf, uvc_event_t *event);

/** @brief              Callback for frame buffer request.
 *  @param itf          Interface number
 *  @param buffer       Pointer to store the frame buffer address
 *  @param buffer_size  Pointer to store the frame buffer size
 *  @return             true if buffer is available, false otherwise
 */
typedef bool(*tusb_uvc_fb_request_cb_t)(int itf, uint8_t **buffer, size_t *buffer_size);

/** @brief          Callback for frame buffer return.
 *  @param itf      Interface number
 *  @param buffer   Frame buffer address
 */
typedef void(*tusb_uvc_fb_return_cb_t)(int itf, uint8_t *buffer);

/** @brief      Callback for stop notification.
 *  @param itf  Interface number
 */
typedef void(*tusb_uvc_stop_cb_t)(int itf);

/** @brief Configuration structure for UVC.
 */
typedef struct {
    tinyusb_uvc_itf_t uvc_port;                     /*!< UVC interface number */
    tusb_uvc_callback_t callback_streaming_start;   /*!< Streaming start callback */
    tusb_uvc_callback_t callback_streaming_stop;    /*!< Streaming stop callback */
    tusb_uvc_fb_request_cb_t fb_request_cb;         /*!< Frame buffer request callback */
    tusb_uvc_fb_return_cb_t fb_return_cb;           /*!< Frame buffer return callback */
    tusb_uvc_stop_cb_t stop_cb;                     /*!< Stop callback */
    const uint8_t *uvc_buffer;                      /*!< UVC working buffer (read-only) */
    size_t uvc_buffer_size;                         /*!< UVC buffer size */
} tinyusb_config_uvc_t;

/** @brief      Initialize UVC interface.
 *  @param cfg  Configuration structure
 *  @return     ESP_OK on success
 */
esp_err_t tinyusb_uvc_init(const tinyusb_config_uvc_t *cfg);

/** @brief      Deinitialize UVC interface.
 *  @param itf  Interface number
 *  @return     ESP_OK on success
 */
esp_err_t tinyusb_uvc_deinit(tinyusb_uvc_itf_t itf);

/** @brief      Check if UVC streaming is active.
 *  @param itf  Interface number
 *  @return     true if streaming is active
 */
bool tinyusb_uvc_streaming_active(tinyusb_uvc_itf_t itf);

/** @brief          Transmit a video frame.
 *  @param itf      Interface number
 *  @param frame    Frame buffer
 *  @param len      Frame length
 *  @return         ESP_OK on success
 */
esp_err_t tinyusb_uvc_transmit_frame(tinyusb_uvc_itf_t itf, uint8_t *frame, size_t len);

#ifdef __cplusplus
}
#endif
