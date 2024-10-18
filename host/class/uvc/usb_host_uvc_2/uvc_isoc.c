/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <string.h> // For memcpy

#include "esp_log.h"

#include "uvc_types_priv.h"
#include "uvc_check_priv.h"
#include "uvc_frame_priv.h"
#include "uvc_critical_priv.h"

static const char *TAG = "uvc-isoc";

void isoc_transfer_callback(usb_transfer_t *transfer)
{
    ESP_LOGD(TAG, "%s", __FUNCTION__);
    uvc_stream_t *uvc_stream = (uvc_stream_t *)transfer->context;

    UVC_ENTER_CRITICAL();
    bool streaming_on = uvc_stream->streaming;
    UVC_EXIT_CRITICAL();
    if (!streaming_on) {
        return; // If the streaming was turned off, we don't have to do anything
    }

    const uint8_t *payload = transfer->data_buffer;
    for (int i = 0; i < transfer->num_isoc_packets; i++) {
        usb_isoc_packet_desc_t *isoc_desc = &transfer->isoc_packet_desc[i];

        // Check USB status
        switch (isoc_desc->status) {
        case USB_TRANSFER_STATUS_COMPLETED:
            break;
        case USB_TRANSFER_STATUS_NO_DEVICE:
        case USB_TRANSFER_STATUS_CANCELED:
            return; // No need to process the rest
        case USB_TRANSFER_STATUS_ERROR:
        case USB_TRANSFER_STATUS_OVERFLOW:
        case USB_TRANSFER_STATUS_STALL:
            ESP_LOGW(TAG, "usb err %d", isoc_desc->status);
            uvc_stream->skip_current_frame = true;
            goto next_isoc_packet; // Data corrupted

        case USB_TRANSFER_STATUS_TIMED_OUT:
        case USB_TRANSFER_STATUS_SKIPPED:
            goto next_isoc_packet; // Skipped and timed out ISOC transfers are not an issue
        default:
            assert(false);
        }

        // Check for Zero Length Packet
        if (isoc_desc->actual_num_bytes == 0) {
            goto next_isoc_packet;
        }

        // Check for start of new frame
        const uvc_payload_header_t *payload_header = (const uvc_payload_header_t *)payload;
        const bool start_of_frame = (uvc_stream->current_frame_id != payload_header->bmHeaderInfo.frame_id);
        if (start_of_frame) {
            // We detected start of new frame. Update Frame ID and start fetching this frame
            uvc_stream->current_frame_id = payload_header->bmHeaderInfo.frame_id;
            uvc_stream->skip_current_frame = false;

            // Get free frame buffer for this new frame
            UVC_ENTER_CRITICAL();
            bool need_new_frame = (uvc_stream->streaming && !uvc_stream->current_frame);
            if (need_new_frame) {
                UVC_EXIT_CRITICAL();
                uvc_stream->current_frame = uvc_frame_get_empty(uvc_stream);
                if (uvc_stream->current_frame == NULL) {
                    // There is no free frame buffer now, skipping this frame
                    uvc_stream->skip_current_frame = true;
                    goto next_isoc_packet;
                }
            } else {
                // We received SoF but current_frame is not NULL: We missed EoF - reset the frame buffer
                uvc_frame_reset(uvc_stream->current_frame);
                UVC_EXIT_CRITICAL();
            }
        } else if (uvc_stream->skip_current_frame) {
            // Previous packets indicated we must skip this frame
            goto next_isoc_packet;
        }

        // Check for empty data
        if (isoc_desc->actual_num_bytes <= payload_header->bHeaderLength) {
            goto next_isoc_packet;
        }

        // Check for error flag
        if (payload_header->bmHeaderInfo.error) {
            uvc_stream->skip_current_frame = true;
            goto next_isoc_packet;
        }

        // Add received data to frame buffer
        const uint8_t *payload_data = payload + payload_header->bHeaderLength;
        const size_t payload_data_len = isoc_desc->actual_num_bytes - payload_header->bHeaderLength;
        esp_err_t ret = uvc_frame_add_data(uvc_stream->current_frame, payload_data, payload_data_len);
        if (ret != ESP_OK) {
            uvc_stream->skip_current_frame = true;
            goto next_isoc_packet;
        }

        // End of Frame. Pass the frame to user
        if (payload_header->bmHeaderInfo.end_of_frame) {
            bool return_frame = true; // In case streaming is stopped ATM, we must return the frame

            // Check if the user did not stop the stream in the meantime
            UVC_ENTER_CRITICAL();
            uvc_frame_t *this_frame = uvc_stream->current_frame;
            uvc_stream->current_frame = NULL; // Stop writing more data to this frame
            const bool invoke_fb_callback = (uvc_stream->streaming && uvc_stream->frame_cb && this_frame);
            uvc_host_frame_callback_t fb_callback = uvc_stream->frame_cb;
            UVC_EXIT_CRITICAL();
            if (invoke_fb_callback) {
                memcpy((uvc_host_stream_format_t *)&this_frame->vs_format, &uvc_stream->vs_format, sizeof(uvc_host_stream_format_t));
                return_frame = fb_callback(this_frame, uvc_stream->cb_arg);
            }
            if (return_frame) {
                // The user has processed the frame in his callback, return it back to empty queue
                uvc_host_frame_return(uvc_stream, this_frame);
            }
        }
next_isoc_packet:
        payload += isoc_desc->num_bytes;
        continue;
    }

    UVC_ENTER_CRITICAL();
    streaming_on = uvc_stream->streaming;
    UVC_EXIT_CRITICAL();
    if (streaming_on) {
        usb_host_transfer_submit(transfer); // Restart the transfer
    }
}
