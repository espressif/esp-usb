/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <string.h> // For guid format parsing
#include "esp_log.h"
#include "usb/usb_helpers.h"
#include "uvc_types_priv.h"
#include "uvc_descriptors_priv.h"

static const char *TAG = "UVC-desc";

/**
 * @brief Print UVC specific descriptor in human readable form
 *
 * This is a callback function that is called from USB Host library,
 * when it wants to print full configuration descriptor to stdout.
 *
 * @param[in] _desc UVC specific descriptor
 */
static void uvc_print_desc(const usb_standard_desc_t *_desc)
{
    //@todo
    ESP_LOGI(TAG, "%s NOT IMPLEMENTED YET", __func__);
}

void uvc_host_desc_print(uvc_host_stream_hdl_t stream_hdl)
{
    assert(stream_hdl);
    uvc_stream_t *uvc_stream = (uvc_stream_t *)stream_hdl;

    const usb_device_desc_t *device_desc;
    const usb_config_desc_t *config_desc;
    ESP_ERROR_CHECK_WITHOUT_ABORT(usb_host_get_device_descriptor(uvc_stream->dev_hdl, &device_desc));
    ESP_ERROR_CHECK_WITHOUT_ABORT(usb_host_get_active_config_descriptor(uvc_stream->dev_hdl, &config_desc));
    usb_print_device_descriptor(device_desc);
    usb_print_config_descriptor(config_desc, &uvc_print_desc);
}

esp_err_t uvc_desc_get_streaming_intf_and_ep(
    uvc_stream_t *uvc_stream,
    uint8_t bInterfaceNumber,
    const usb_intf_desc_t **intf_desc_ret,
    const usb_ep_desc_t **ep_desc_ret)
{
    const usb_config_desc_t *cfg_desc;
    const usb_intf_desc_t *intf_desc = NULL;
    const usb_ep_desc_t *ep_desc = NULL;
    int offset = 0;
    usb_device_handle_t usb_dev = uvc_stream->dev_hdl;

    usb_host_get_active_config_descriptor(usb_dev, &cfg_desc);
    UVC_CHECK(cfg_desc, ESP_ERR_NOT_FOUND);
    const uint8_t num_of_alternate = usb_parse_interface_number_of_alternate(cfg_desc, bInterfaceNumber);
    uint16_t last_mps = 0; // Looking for maximum MPS: init to zero
    uint8_t last_mult = UINT8_MAX; // Looking for minimum: init to max
    for (int i = 0; i < num_of_alternate + 1; i++) {
        // Check Interface desc
        intf_desc = usb_parse_interface_descriptor(cfg_desc, bInterfaceNumber, i, &offset);
        UVC_CHECK(intf_desc, ESP_ERR_NOT_FOUND);
        UVC_CHECK(intf_desc->bInterfaceClass == USB_CLASS_VIDEO, ESP_ERR_NOT_FOUND);
        UVC_CHECK(intf_desc->bInterfaceSubClass == UVC_SC_VIDEOSTREAMING, ESP_ERR_NOT_FOUND);
        if (intf_desc->bNumEndpoints == 0 && i == 0) {
            continue; // This is Alternate setting 0 for ISOC cameras.
        }
        UVC_CHECK(intf_desc->bNumEndpoints == 1, ESP_ERR_NOT_FOUND); // Only 1 endpoint is expected

        // Check EP desc
        ep_desc = usb_parse_endpoint_descriptor_by_index(intf_desc, 0, cfg_desc->wTotalLength, &offset);
        UVC_CHECK(ep_desc, ESP_ERR_NOT_FOUND);

        // Here we look for an interface that offers the largest MPS with minimum multiple transactions in a microframe
        const uint16_t current_mps = USB_EP_DESC_GET_MPS(ep_desc);
        const uint8_t current_mult = USB_EP_DESC_GET_MULT(ep_desc);
        if (current_mps >= last_mps && current_mult <= last_mult) {
            last_mps = current_mps;
            last_mult = current_mult;
            *ep_desc_ret = ep_desc;
            *intf_desc_ret = intf_desc;
        } else {
            break;
        }
    }

    return ESP_OK;
}

int uvc_desc_parse_format(const uvc_format_desc_t *format_desc)
{
    assert(format_desc);
    int ret = UVC_VS_FORMAT_UNDEFINED;

    switch (format_desc->bDescriptorSubType) {
    case UVC_VS_DESC_SUBTYPE_FORMAT_UNCOMPRESSED:
        ret = UVC_VS_FORMAT_UNCOMPRESSED;
        break;
    case UVC_VS_DESC_SUBTYPE_FORMAT_MJPEG:
        ret = UVC_VS_FORMAT_MJPEG;
        break;
    case UVC_VS_DESC_SUBTYPE_FORMAT_FRAME_BASED: {
        const char *guid = (const char *)(format_desc->uncompressed_frame_based.guidFormat);
        // We do not check full guid, but only the first 4 characters that show human readable format
        if (strncmp(guid, "H265", 4) == 0) {
            ret = UVC_VS_FORMAT_H265;
        } else if (strncmp(guid, "H264", 4) == 0) {
            ret = UVC_VS_FORMAT_H264;
        }
        break;
    }
    case UVC_VS_DESC_SUBTYPE_UNDEFINED:
        ret =  UVC_VS_FORMAT_UNDEFINED;
        break;
    default: break;
    }
    return ret;
}

static bool uvc_desc_is_format_desc(const usb_standard_desc_t *_desc)
{
    UVC_CHECK(_desc, false);
    const uvc_format_desc_t *desc = (const uvc_format_desc_t *)_desc;
    if (
        desc->bDescriptorType == UVC_CS_INTERFACE &&
        (
            desc->bDescriptorSubType == UVC_VS_DESC_SUBTYPE_FORMAT_UNCOMPRESSED ||
            desc->bDescriptorSubType == UVC_VS_DESC_SUBTYPE_FORMAT_MJPEG ||
            desc->bDescriptorSubType == UVC_VS_DESC_SUBTYPE_FORMAT_MPEG2TS ||
            desc->bDescriptorSubType == UVC_VS_DESC_SUBTYPE_FORMAT_DV ||
            desc->bDescriptorSubType == UVC_VS_DESC_SUBTYPE_FORMAT_FRAME_BASED ||
            desc->bDescriptorSubType == UVC_VS_DESC_SUBTYPE_FORMAT_STREAM_BASED
        )
    ) {
        return true;
    }
    return false;
}

static bool uvc_desc_format_is_equal(const uvc_frame_desc_t *frame_desc, const uvc_host_stream_format_t *vs_format)
{
    UVC_CHECK(frame_desc && vs_format, false);
    if (frame_desc->wWidth == vs_format->h_res && frame_desc->wHeight == vs_format->v_res) {
        uint8_t bFrameIntervalType;
        uint32_t dwMinFrameInterval, dwMaxFrameInterval, dwFrameIntervalStep;
        const uint32_t *dwFrameInterval;

        // First parse the format specific frame descriptor
        if (frame_desc->bDescriptorSubtype == UVC_VS_DESC_SUBTYPE_FRAME_FRAME_BASED) {
            bFrameIntervalType = frame_desc->frame_based.bFrameIntervalType;
            dwMinFrameInterval = frame_desc->frame_based.dwMinFrameInterval;
            dwMaxFrameInterval = frame_desc->frame_based.dwMaxFrameInterval;
            dwFrameIntervalStep = frame_desc->frame_based.dwFrameIntervalStep;
            dwFrameInterval = frame_desc->frame_based.dwFrameInterval;
        } else {
            bFrameIntervalType = frame_desc->mjpeg_uncompressed.bFrameIntervalType;
            dwMinFrameInterval = frame_desc->mjpeg_uncompressed.dwMinFrameInterval;
            dwMaxFrameInterval = frame_desc->mjpeg_uncompressed.dwMaxFrameInterval;
            dwFrameIntervalStep = frame_desc->mjpeg_uncompressed.dwFrameIntervalStep;
            dwFrameInterval = frame_desc->mjpeg_uncompressed.dwFrameInterval;
        }

        if (bFrameIntervalType == 0) {
            // This stream does not support discrete Frame Interval. Check all supported intervals
            uint32_t current_frame_interval = dwMinFrameInterval;
            while (current_frame_interval <= dwMaxFrameInterval) {
                if (vs_format->fps == UVC_DESC_DWFRAMEINTERVAL_TO_FPS(current_frame_interval)) {
                    return true;
                }
                current_frame_interval += dwFrameIntervalStep;
            }
        } else {
            // This stream support discrete Frame Intervals. Check supported intervals
            for (int i = 0; i < bFrameIntervalType; i++) {
                if (vs_format->fps == UVC_DESC_DWFRAMEINTERVAL_TO_FPS(dwFrameInterval[i])) {
                    return true;
                }
            }
        }
    }
    return false;
}

esp_err_t uvc_desc_get_frame_format_by_format(
    uvc_stream_t *uvc_stream,
    uint8_t bInterfaceNumber,
    const uvc_host_stream_format_t *vs_format,
    const uvc_format_desc_t **format_desc_ret,
    const uvc_frame_desc_t **frame_desc_ret)
{
    UVC_CHECK(uvc_stream && vs_format, ESP_ERR_INVALID_ARG);

    const uvc_vs_input_header_desc_t *input_header = uvc_desc_get_streaming_input_header(uvc_stream, bInterfaceNumber);
    UVC_CHECK(input_header, ESP_ERR_NOT_FOUND);

    // Find requested Format descriptors
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    int format_offset = 0;
    const usb_standard_desc_t *current_desc = (const usb_standard_desc_t *) input_header;
    while ((current_desc = usb_parse_next_descriptor_of_type(current_desc, input_header->wTotalLength, UVC_CS_INTERFACE, &format_offset))) {
        if (uvc_desc_is_format_desc(current_desc)) {
            const uvc_format_desc_t *this_format = (const uvc_format_desc_t *)(current_desc);
            if (vs_format->format == uvc_desc_parse_format(this_format)) {
                if (format_desc_ret) {
                    *format_desc_ret = this_format;
                }
                // We found required Format Descriptor
                // Now we look for correct Frame Descriptors which should be directly after Format
                while ((current_desc = usb_parse_next_descriptor_of_type(current_desc, input_header->wTotalLength, UVC_CS_INTERFACE, &format_offset))) {
                    uvc_frame_desc_t *this_frame = (uvc_frame_desc_t *)current_desc;
                    if (uvc_desc_format_is_equal(this_frame, vs_format)) {
                        if (frame_desc_ret) {
                            *frame_desc_ret = this_frame;
                        }
                        ret = ESP_OK;
                        break;
                    }
                }
                break;
            }
        }
    }
    return ret;
}

bool uvc_desc_is_format_supported(
    uvc_stream_t *uvc_stream,
    uint8_t bInterfaceNumber,
    const uvc_host_stream_format_t *vs_format)
{
    return (ESP_OK == uvc_desc_get_frame_format_by_format(uvc_stream, bInterfaceNumber, vs_format, NULL, NULL));
}

const uvc_vs_input_header_desc_t *uvc_desc_get_streaming_input_header(uvc_stream_t *uvc_stream, uint8_t bInterfaceNumber)
{
    UVC_CHECK(uvc_stream, NULL);
    UVC_CHECK(uvc_stream->dev_hdl, NULL);

    // First get Configuration descriptor of this device
    const usb_config_desc_t *cfg_desc;
    usb_device_handle_t usb_dev = uvc_stream->dev_hdl;
    usb_host_get_active_config_descriptor(usb_dev, &cfg_desc);
    UVC_CHECK(cfg_desc, NULL);

    // Find Interface with alternate settings = 0. All Video Streaming descriptors should be after this descriptor.
    int offset = 0;
    const usb_intf_desc_t *intf_desc = usb_parse_interface_descriptor(cfg_desc, bInterfaceNumber, 0, &offset);
    UVC_CHECK(intf_desc, NULL);
    UVC_CHECK(intf_desc->bInterfaceClass == USB_CLASS_VIDEO, NULL);
    UVC_CHECK(intf_desc->bInterfaceSubClass == UVC_SC_VIDEOSTREAMING, NULL);

    // Find Video Input Header Descriptor
    const usb_standard_desc_t *std_desc = usb_parse_next_descriptor_of_type((const usb_standard_desc_t *)intf_desc, cfg_desc->wTotalLength, UVC_CS_INTERFACE, &offset);
    const uvc_vs_input_header_desc_t *input_header = (uvc_vs_input_header_desc_t *)std_desc;
    UVC_CHECK(input_header, NULL);
    UVC_CHECK(input_header->bDescriptorSubType == UVC_VS_DESC_SUBTYPE_INPUT_HEADER, NULL);
    return input_header;
}

const uvc_vc_header_desc_t *uvc_desc_get_control_interface_header(uvc_stream_t *uvc_stream, unsigned uvc_idx)
{
    UVC_CHECK(uvc_stream, NULL);
    UVC_CHECK(uvc_stream->dev_hdl, NULL);

    // First get Configuration descriptor of this device
    const uvc_vc_header_desc_t *header_desc_ret = NULL;
    const usb_config_desc_t *cfg_desc;
    usb_device_handle_t usb_dev = uvc_stream->dev_hdl;
    usb_host_get_active_config_descriptor(usb_dev, &cfg_desc);
    UVC_CHECK(cfg_desc, NULL);

    // Find IAD UVC descriptor with desired index
    int offset = 0;
    int uvc_iad_idx = 0;
    const usb_standard_desc_t *current_desc = (const usb_standard_desc_t *)cfg_desc;
    while ((current_desc = usb_parse_next_descriptor_of_type(current_desc, cfg_desc->wTotalLength, USB_B_DESCRIPTOR_TYPE_INTERFACE_ASSOCIATION, &offset))) {
        const usb_iad_desc_t *iad_desc = (usb_iad_desc_t *)current_desc;
        if (iad_desc->bFunctionClass == USB_CLASS_VIDEO && iad_desc->bFunctionSubClass == UVC_SC_VIDEO_INTERFACE_COLLECTION) {
            if (uvc_idx == uvc_iad_idx) {
                // This is the IAD that we are looking for. Find its first Video Control interface header descriptor
                header_desc_ret = (const uvc_vc_header_desc_t *)usb_parse_next_descriptor_of_type(current_desc, cfg_desc->wTotalLength, UVC_CS_INTERFACE, &offset);
                UVC_CHECK(header_desc_ret->bDescriptorSubType == UVC_VC_DESC_SUBTYPE_HEADER, NULL);
                break;
            } else {
                // The user requires next UVC function
                uvc_iad_idx++;
            }
        }
    }
    return header_desc_ret;
}

esp_err_t uvc_desc_get_frame_format_by_index(
    uvc_stream_t *uvc_stream,
    uint8_t bInterfaceNumber,
    uint8_t bFormatIndex,
    uint8_t bFrameIndex,
    const uvc_format_desc_t **format_desc_ret,
    const uvc_frame_desc_t **frame_desc_ret)
{
    ESP_LOGV(TAG, "Looking for format %d frame %d", bFormatIndex, bFrameIndex);
    UVC_CHECK(bFormatIndex > 0, ESP_ERR_INVALID_ARG); // Formats are indexed from 1
    UVC_CHECK(bFrameIndex > 0, ESP_ERR_INVALID_ARG); // Frames are indexed from 1
    UVC_CHECK(format_desc_ret && frame_desc_ret, ESP_ERR_INVALID_ARG);

    const uvc_vs_input_header_desc_t *input_header = uvc_desc_get_streaming_input_header(uvc_stream, bInterfaceNumber);
    UVC_CHECK(input_header, ESP_ERR_NOT_FOUND);
    UVC_CHECK(input_header->bNumFormats >= bFormatIndex, ESP_ERR_NOT_FOUND);

    // Find requested Format and Frame descriptors
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    int format_offset = 0;
    const usb_standard_desc_t *current_desc = (const usb_standard_desc_t *) input_header;
    while ((current_desc = usb_parse_next_descriptor_of_type(current_desc, input_header->wTotalLength, UVC_CS_INTERFACE, &format_offset))) {
        const uvc_format_desc_t *this_format = (const uvc_format_desc_t *)current_desc;
        if (uvc_desc_is_format_desc(current_desc)) {
            if (this_format->bFormatIndex == bFormatIndex) {
                UVC_CHECK(this_format->bNumFrameDescriptors >= bFrameIndex, ESP_ERR_NOT_FOUND);
                *format_desc_ret = (const uvc_format_desc_t *)this_format;
                // We found required Format Descriptor
                // Now we look for correct Frame Descriptors which should be directly after Format
                while ((current_desc = usb_parse_next_descriptor_of_type(current_desc, input_header->wTotalLength, UVC_CS_INTERFACE, &format_offset))) {
                    uvc_frame_desc_t *this_frame = (uvc_frame_desc_t *)current_desc;
                    if (this_frame->bFrameIndex == bFrameIndex) {
                        *frame_desc_ret = this_frame;
                        ret = ESP_OK;
                        break;
                    }
                }
                break;
            }
        }
    }
    return ret;
}
