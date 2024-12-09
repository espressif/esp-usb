/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h> // For memcpy

#include "esp_check.h"
#include "usb/uvc_host.h"
#include "uvc_frame_priv.h"
#include "uvc_types_priv.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "UVC-frame";

esp_err_t uvc_host_frame_return(uvc_host_stream_hdl_t stream_hdl, uvc_frame_t *frame)
{
    UVC_CHECK(stream_hdl && frame, ESP_ERR_INVALID_ARG);
    uvc_stream_t *uvc_stream = (uvc_stream_t *)stream_hdl;
    uvc_frame_reset(frame);
    BaseType_t result = xQueueSend(uvc_stream->empty_fb_queue, &frame, 0);
    UVC_CHECK(pdPASS == result, ESP_FAIL);
    return ESP_OK;
}

esp_err_t uvc_frame_allocate(uvc_stream_t *uvc_stream, int nb_of_fb, size_t fb_size)
{
    UVC_CHECK(uvc_stream, ESP_ERR_INVALID_ARG);
    esp_err_t ret;

    // We will be passing the frame buffers by reference
    uvc_stream->empty_fb_queue = xQueueCreate(nb_of_fb, sizeof(uvc_frame_t *));
    UVC_CHECK(uvc_stream->empty_fb_queue, ESP_ERR_NO_MEM);
    for (int i = 0; i < nb_of_fb; i++) {
        // Allocate the frame buffer
        uvc_frame_t *this_fb = malloc(sizeof(uvc_frame_t));
        ESP_GOTO_ON_FALSE(this_fb, ESP_ERR_NO_MEM, err, TAG, "Not enough memory for frame buffers %d", fb_size);
        uint8_t *this_data = malloc(fb_size);
        if (!this_data) {
            free(this_fb);
            ret = ESP_ERR_NO_MEM;
            ESP_LOGE(TAG, "Not enough memory for frame buffers %d", fb_size);
            goto err;
        }

        // Set members to default
        this_fb->data = this_data;
        this_fb->data_buffer_len = fb_size;
        this_fb->data_len = 0;

        // Add the frame to Queue of empty frames
        const BaseType_t result = xQueueSend(uvc_stream->empty_fb_queue, &this_fb, 0);
        assert(pdPASS == result);
    }
    return ESP_OK;

err:
    uvc_frame_free(uvc_stream);
    return ret;
}

void uvc_frame_free(uvc_stream_t *uvc_stream)
{
    if (!uvc_stream || !uvc_stream->empty_fb_queue) {
        return;
    }

    // Free all Frame Buffers and the Queue itself
    uvc_frame_t *this_fb;
    while (xQueueReceive(uvc_stream->empty_fb_queue, &this_fb, 0) == pdPASS) {
        free(this_fb->data);
        free(this_fb);
    }
    vQueueDelete(uvc_stream->empty_fb_queue);
    uvc_stream->empty_fb_queue = NULL;
}

bool uvc_frame_are_all_returned(uvc_stream_t *uvc_stream)
{
    UVC_CHECK(uvc_stream, false);
    UVC_CHECK(uvc_stream->empty_fb_queue, false);

    // In case the user returns 'false' from uvc_host_frame_callback_t, he must return the frame buffers with uvc_host_frame_return()
    // Here we check whether all allocated frame buffers are in the 'empty_fb_queue'
    const UBaseType_t uxSpacesAvailable = uxQueueSpacesAvailable(uvc_stream->empty_fb_queue);
    return (uxSpacesAvailable == 0);
}

uvc_frame_t *uvc_frame_get_empty(uvc_stream_t *uvc_stream)
{
    UVC_CHECK(uvc_stream, NULL);
    uvc_frame_t *this_fb;
    if (xQueueReceive(uvc_stream->empty_fb_queue, &this_fb, 0) == pdPASS) {
        return this_fb;
    } else {
        return NULL;
    }
}

esp_err_t uvc_frame_add_data(uvc_frame_t *frame, const uint8_t *data, size_t data_len)
{
    UVC_CHECK(frame && data, ESP_ERR_INVALID_ARG);
    UVC_CHECK(frame->data_len + data_len <= frame->data_buffer_len, ESP_ERR_INVALID_SIZE);

    memcpy(frame->data + frame->data_len, data, data_len);
    frame->data_len += data_len;
    return ESP_OK;
}

void uvc_frame_reset(uvc_frame_t *frame)
{
    assert(frame);
    frame->data_len = 0;
}
