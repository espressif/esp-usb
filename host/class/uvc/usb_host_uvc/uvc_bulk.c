/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <string.h> // For memcpy

#include "esp_log.h"

#include "uvc_stream.h" // For uvc_host_stream_pause()
#include "uvc_types_priv.h"
#include "uvc_check_priv.h"
#include "uvc_frame_priv.h"
#include "uvc_critical_priv.h"

static const char *TAG = "uvc-bulk";

static void bulk_complete_frame(uvc_stream_t *uvc_stream)
{
    // Clear the current frame before invoking the callback so no later packet can append to it.
    UVC_ENTER_CRITICAL();
    uvc_host_frame_t *this_frame = uvc_stream->dynamic.current_frame;
    uvc_stream->dynamic.current_frame = NULL;

    const bool invoke_fb_callback = (uvc_stream->dynamic.streaming && uvc_stream->constant.frame_cb && this_frame &&
                                     !uvc_stream->single_thread.skip_current_frame);
    UVC_EXIT_CRITICAL();

    bool return_frame = true;
    if (invoke_fb_callback) {
        return_frame = uvc_stream->constant.frame_cb(this_frame, uvc_stream->constant.cb_arg);
    }
    if (this_frame && return_frame) {
        uvc_host_frame_return(uvc_stream, this_frame);
    }
}

static void bulk_add_frame_data(uvc_stream_t *uvc_stream, const uint8_t *data, size_t data_len)
{
    if (uvc_stream->single_thread.skip_current_frame || data_len == 0) {
        return;
    }

    uvc_host_frame_t *current_frame = UVC_ATOMIC_LOAD(uvc_stream->dynamic.current_frame);
    if (!current_frame) {
        ESP_LOGE(TAG, "missing current frame while appending bulk payload");
        uvc_stream->single_thread.skip_current_frame = true;
        return;
    }

    esp_err_t ret = uvc_frame_add_data(current_frame, data, data_len);
    if (ret != ESP_OK) {
        // Frame buffer overflow
        uvc_stream->single_thread.skip_current_frame = true;

        // Inform the user about the overflow
        uvc_host_stream_callback_t stream_cb = uvc_stream->constant.stream_cb;
        if (stream_cb) {
            const uvc_host_stream_event_data_t event = {
                .type = UVC_HOST_FRAME_BUFFER_OVERFLOW,
            };
            stream_cb(&event, uvc_stream->constant.cb_arg);
        }
    }
}

/**
 * Process one contiguous part of a UVC payload.
 *
 * A USB transfer can contain the end of one UVC payload and the beginning of
 * the next. The caller therefore splits transfers at known payload boundaries
 * and calls this function once for each piece. payload_end is true for both a
 * counted dwMaxPayloadTransferSize boundary and a short-transfer boundary.
 *
 * @return true when an expected payload header was valid
 */
static bool bulk_process_payload_piece(uvc_stream_t *uvc_stream, const uint8_t *payload,
                                       size_t payload_len, bool payload_end)
{
    const uint8_t *payload_data = payload;
    size_t payload_data_len = payload_len;

    switch (uvc_stream->single_thread.next_bulk_packet) {
    case UVC_STREAM_BULK_PACKET_SOF: {
label_sof:
        // This label is also reached after an FID toggle from the EOF state.
        // Restore the unconsumed payload before stripping its header as SoF.
        payload_data = payload;
        payload_data_len = payload_len;
        const uvc_payload_header_t *payload_header = (const uvc_payload_header_t *)payload;
        if (!uvc_frame_payload_header_validate(payload_header, payload_len)) {
            ESP_LOGW(TAG, "invalid UVC payload header, %02x, %02x, len:%zu",
                     payload_len > 0 ? payload[0] : 0, payload_len > 1 ? payload[1] : 0, payload_len);
            uvc_stream->single_thread.skip_current_frame = true;
            return false;
        }

        uvc_stream->single_thread.current_frame_id = payload_header->bmHeaderInfo.frame_id;
        uvc_stream->single_thread.skip_current_frame = payload_header->bmHeaderInfo.error;
        uvc_stream->single_thread.bulk_payload_frame_end = payload_header->bmHeaderInfo.end_of_frame;
        payload_data += payload_header->bHeaderLength;
        payload_data_len -= payload_header->bHeaderLength;

        if (payload_data_len == 0) {
            ESP_LOGD(TAG, "header-only payload, skipping");
            goto skip_sof;
        }

        if (payload_header->bmHeaderInfo.error) {
            ESP_LOGW(TAG, "frame error flag set");
            uvc_stream->single_thread.skip_current_frame = true;
            goto skip_sof;
        }

        if (uvc_stream->dynamic.vs_format.format == UVC_VS_FORMAT_MJPEG &&
                (payload_data_len < 2 || payload_data[0] != JPEG_MARKER || payload_data[1] != JPEG_SOI)) {
            uvc_stream->single_thread.skip_current_frame = true;
            ESP_LOGW(TAG, "invalid MJPEG SOI");
            goto skip_sof;
        }

        UVC_ENTER_CRITICAL();
        const bool need_new_frame = (uvc_stream->dynamic.streaming && !uvc_stream->dynamic.current_frame);
        if (need_new_frame) {
            UVC_EXIT_CRITICAL();
            uvc_stream->dynamic.current_frame = uvc_frame_get_empty(uvc_stream);
            if (uvc_stream->dynamic.current_frame == NULL) {
                uvc_stream->single_thread.skip_current_frame = true;

                uvc_host_stream_callback_t stream_cb = uvc_stream->constant.stream_cb;
                if (stream_cb) {
                    const uvc_host_stream_event_data_t event = {
                        .type = UVC_HOST_FRAME_BUFFER_UNDERFLOW,
                    };
                    stream_cb(&event, uvc_stream->constant.cb_arg);
                }
            }
        } else {
            uvc_frame_reset(uvc_stream->dynamic.current_frame);
            UVC_EXIT_CRITICAL();
        }
skip_sof:
        uvc_stream->single_thread.next_bulk_packet = UVC_STREAM_BULK_PACKET_DATA;
        __attribute__((fallthrough));
    }
    case UVC_STREAM_BULK_PACKET_DATA:
        bulk_add_frame_data(uvc_stream, payload_data, payload_data_len);
        if (payload_end) {
            if (uvc_stream->single_thread.bulk_payload_frame_end) {
                // EOF describes the whole payload. Delay completion until all
                // pieces of that payload have been appended.
                bulk_complete_frame(uvc_stream);
                uvc_stream->single_thread.bulk_payload_frame_end = false;
                uvc_stream->single_thread.next_bulk_packet = UVC_STREAM_BULK_PACKET_SOF;
            } else {
                // The next payload starts with a header, which determines
                // whether it continues this frame or starts a new frame.
                uvc_stream->single_thread.next_bulk_packet = UVC_STREAM_BULK_PACKET_EOF;
            }
        }
        break;

    case UVC_STREAM_BULK_PACKET_EOF: {
        // EOF means that the previous payload ended, not necessarily that the
        // video frame ended. Inspect the next payload header for EOF or FID.
        const uvc_payload_header_t *payload_header = (const uvc_payload_header_t *)payload;
        if (!uvc_frame_payload_header_validate(payload_header, payload_len)) {
            ESP_LOGD(TAG, "invalid UVC payload header in EOF, %02x, %02x, len:%zu",
                     payload_len > 0 ? payload[0] : 0, payload_len > 1 ? payload[1] : 0, payload_len);
            uvc_stream->single_thread.skip_current_frame = true;
            return false;
        }

        uvc_stream->single_thread.skip_current_frame |= payload_header->bmHeaderInfo.error;
        const bool start_of_frame =
            (uvc_stream->single_thread.current_frame_id != payload_header->bmHeaderInfo.frame_id);
        uvc_stream->single_thread.bulk_payload_frame_end = payload_header->bmHeaderInfo.end_of_frame;
        payload_data += payload_header->bHeaderLength;
        payload_data_len -= payload_header->bHeaderLength;

        if (!start_of_frame) {
            // The payload belongs to the current frame even when its header
            // carries EOF, so append its data before completing the frame.
            bulk_add_frame_data(uvc_stream, payload_data, payload_data_len);
            if (payload_header->bmHeaderInfo.end_of_frame) {
                if (payload_end) {
                    bulk_complete_frame(uvc_stream);
                    uvc_stream->single_thread.bulk_payload_frame_end = false;
                    uvc_stream->single_thread.next_bulk_packet = UVC_STREAM_BULK_PACKET_SOF;
                } else {
                    // More pieces of this EOF payload follow in later URBs.
                    uvc_stream->single_thread.bulk_payload_frame_end = true;
                    uvc_stream->single_thread.next_bulk_packet = UVC_STREAM_BULK_PACKET_DATA;
                }
            } else {
                // Another payload of the same frame. A short piece must leave
                // us expecting a header at the start of the next payload.
                uvc_stream->single_thread.next_bulk_packet =
                    payload_end ? UVC_STREAM_BULK_PACKET_EOF : UVC_STREAM_BULK_PACKET_DATA;
            }
            break;
        }

        bulk_complete_frame(uvc_stream);
        uvc_stream->single_thread.next_bulk_packet = UVC_STREAM_BULK_PACKET_SOF;

        if (start_of_frame && payload_data_len > 0) {
            // Some cameras never set EOF. A FID toggle both completes the old
            // frame and makes this payload the start of the next frame.
            goto label_sof;
        }
        uvc_stream->single_thread.bulk_payload_frame_end = false;
        break;
    }
    default:
        abort();
    }

    return true;
}

/**
 * @brief Callback function for handling Bulk USB transfers from a UVC camera.
 *
 * This function processes Bulk transfer packets, managing frame data according to a state machine. The characteristics
 * and requirements of Bulk transfers are as follows:
 *
 * - **CRC Included**: Ensures no errors in frame data.
 * - **ACK Mechanism**: Missed packets are retransmitted, ensuring reliable data delivery.
 * - **Payload Headers**: Every UVC bulk payload starts with a header. A payload ends at the negotiated
 *   dwMaxPayloadTransferSize or at a short USB transfer.
 *
 * To process these packets, a state machine is implemented to track the next expected Bulk packet type:
 * - Start of Frame (SoF), usually followed by data packets in the same transfer
 * - Data packets (no header)
 * - End of Frame (EoF)
 *
 * The function handles USB transfer statuses, manages frame buffers, and invokes user-defined callbacks for
 * completed frames.
 *
 * @param[in] transfer Pointer to the completed USB transfer structure.
 */
void bulk_transfer_callback(usb_transfer_t *transfer)
{
    ESP_LOGD(TAG, "%s", __FUNCTION__);
    uvc_stream_t *uvc_stream = (uvc_stream_t *)transfer->context;

    // Check USB transfer status
    switch (transfer->status) {
    case USB_TRANSFER_STATUS_COMPLETED:
        break;
    case USB_TRANSFER_STATUS_NO_DEVICE:
    case USB_TRANSFER_STATUS_CANCELED:
    case USB_TRANSFER_STATUS_ERROR:
    case USB_TRANSFER_STATUS_OVERFLOW:
    case USB_TRANSFER_STATUS_STALL:
        // On Bulk errors we stop the stream
        //@todo Stall, error and overflow errors should be propagated to the user
        ESP_ERROR_CHECK(uvc_host_stream_pause(uvc_stream)); // This should never fail
        break;
    case USB_TRANSFER_STATUS_TIMED_OUT:
    case USB_TRANSFER_STATUS_SKIPPED: // Should never happen to BULK transfer
    default:
        assert(false);
    }

    if (!UVC_ATOMIC_LOAD(uvc_stream->dynamic.streaming)) {
        return; // If the streaming was turned off, we don't have to do anything
    }

    const bool transfer_is_short = transfer->actual_num_bytes < transfer->data_buffer_size;
    const size_t max_payload_size = uvc_stream->dynamic.dwMaxPayloadTransferSize;

    // Byte counting is needed only when a maximum-size payload can end on an
    // MPS boundary without producing a short USB packet. If the negotiated
    // size is not an MPS multiple, USB supplies an unambiguous short packet
    // and the original short-transfer parser remains sufficient.
    const bool count_payload_bytes = max_payload_size > 0 && uvc_stream->constant.bulk_mps > 0 &&
                                     max_payload_size % uvc_stream->constant.bulk_mps == 0;

    if (!count_payload_bytes) {
        bulk_process_payload_piece(uvc_stream, transfer->data_buffer, transfer->actual_num_bytes, transfer_is_short);
        goto resubmit;
    }

    if (uvc_stream->single_thread.bulk_payload_resync) {
        // An invalid header means the byte counter can no longer be trusted.
        // Ignore input until a short transfer provides an absolute payload
        // boundary, then expect a header in the following transfer.
        if (transfer_is_short) {
            uvc_stream->single_thread.bulk_payload_resync = false;
            uvc_stream->single_thread.bulk_payload_bytes_left = 0;
            uvc_stream->single_thread.bulk_payload_frame_end = false;
            uvc_stream->single_thread.next_bulk_packet = UVC_STREAM_BULK_PACKET_EOF;
        }
        goto resubmit;
    }

    // A ZLP after an exactly full payload is padding, not another payload with a missing header.
    if (transfer->actual_num_bytes == 0) {
        if (uvc_stream->single_thread.bulk_payload_bytes_left != 0 &&
                uvc_stream->single_thread.next_bulk_packet == UVC_STREAM_BULK_PACKET_DATA) {
            // A ZLP before the negotiated maximum terminates this payload in
            // the same way as any other short transfer.
            uvc_stream->single_thread.next_bulk_packet = UVC_STREAM_BULK_PACKET_EOF;
            uvc_stream->single_thread.bulk_payload_bytes_left = 0;
        }
        goto resubmit;
    }

    size_t offset = 0;
    while (offset < transfer->actual_num_bytes) {
        // One URB can cross one or more UVC payload boundaries. Process only
        // up to the next boundary so an embedded payload header is never
        // copied into the frame buffer as image data.
        if (uvc_stream->single_thread.bulk_payload_bytes_left == 0) {
            uvc_stream->single_thread.bulk_payload_bytes_left = max_payload_size;
        }

        const size_t bytes_available = transfer->actual_num_bytes - offset;
        const size_t piece_len = bytes_available < uvc_stream->single_thread.bulk_payload_bytes_left
                                 ? bytes_available : uvc_stream->single_thread.bulk_payload_bytes_left;
        const bool counted_payload_end = piece_len == uvc_stream->single_thread.bulk_payload_bytes_left;
        const bool physical_payload_end = transfer_is_short && piece_len == bytes_available;
        const bool payload_end = counted_payload_end || physical_payload_end;
        const bool header_expected =
            uvc_stream->single_thread.next_bulk_packet != UVC_STREAM_BULK_PACKET_DATA;

        const bool valid_header = bulk_process_payload_piece(
                                      uvc_stream, transfer->data_buffer + offset, piece_len, payload_end);
        offset += piece_len;

        if (!valid_header && header_expected) {
            // A physical short packet already restored alignment. Otherwise
            // stop counting now and wait for the next short transfer.
            uvc_stream->single_thread.bulk_payload_bytes_left = 0;
            uvc_stream->single_thread.bulk_payload_frame_end = false;
            uvc_stream->single_thread.next_bulk_packet = UVC_STREAM_BULK_PACKET_EOF;
            if (!physical_payload_end) {
                uvc_stream->single_thread.bulk_payload_resync = true;
            }
            break;
        }

        if (payload_end) {
            uvc_stream->single_thread.bulk_payload_bytes_left = 0;
        } else {
            uvc_stream->single_thread.bulk_payload_bytes_left -= piece_len;
        }
    }

resubmit:
    if (UVC_ATOMIC_LOAD(uvc_stream->dynamic.streaming)) {
        usb_host_transfer_submit(transfer); // Restart the transfer
    }
}
