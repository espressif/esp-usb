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

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "tusb.h"
#include "tinyusb_uvc.h"

/**
 * @brief UVC device state structure
 */
typedef struct {
    bool initialized;                           /*!< Initialization flag */
    bool streaming;                             /*!< Streaming active flag */
    tusb_uvc_callback_t callback_streaming_start; /*!< Streaming start callback */
    tusb_uvc_callback_t callback_streaming_stop;  /*!< Streaming stop callback */
    tusb_uvc_fb_request_cb_t fb_request_cb;    /*!< Frame buffer request callback */
    tusb_uvc_fb_return_cb_t fb_return_cb;      /*!< Frame buffer return callback */
    tusb_uvc_stop_cb_t stop_cb;                /*!< Stop callback */
    uint8_t *uvc_buffer;                       /*!< UVC working buffer */
    size_t uvc_buffer_size;                    /*!< UVC buffer size */
    uint32_t interval_ms;                      /*!< Frame interval in ms */
    TaskHandle_t uvc_task_hdl;                 /*!< UVC task handle */
    EventGroupHandle_t event_group;            /*!< Event group for synchronization */
    uint8_t *current_frame_buffer;             /*!< Currently transmitting frame buffer */
} esp_tusb_uvc_t;

/**
 * @brief Initialize UVC basic object
 *
 * @param itf Interface number
 * @return esp_err_t ESP_OK on success
 */
esp_err_t tinyusb_uvc_init_itf(tinyusb_uvc_itf_t itf);

/**
 * @brief Deinitialize UVC
 *
 * @param itf Interface number
 * @return esp_err_t ESP_OK on success
 */
esp_err_t tinyusb_uvc_deinit_itf(tinyusb_uvc_itf_t itf);

/**
 * @brief Get UVC interface instance
 *
 * @param itf Interface number
 * @return esp_tusb_uvc_t* Pointer to UVC instance or NULL
 */
esp_tusb_uvc_t *tinyusb_uvc_get_intf(tinyusb_uvc_itf_t itf);

#ifdef __cplusplus
}
#endif
