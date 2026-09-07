/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

// In this file we want to have clean interface for descriptor parsing
// So we include only files with USB specification definitions
// This interface is also used in host_tests
#include "usb/usb_types_ch9.h"
#include "usb_types_uvc.h"

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
 * We go through all alternate interfaces and pick the one that offers endpoint with MPS that,
 * is smaller than or equal to dwMaxPayloadTransferSize
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

/**
 * @brief Check if the given USB configuration descriptor belongs to a UVC (USB Video Class) device.
 *
 * This function iterates through the descriptors in the provided configuration descriptor to determine
 * if there is any interface descriptor indicating the device is a UVC device.
 *
 * @param[in] cfg_desc Pointer to the USB configuration descriptor.
 *
 * @return
 *      - true: If the configuration descriptor contains a UVC interface.
 *      - false: Otherwise.
 */
bool uvc_desc_is_uvc_device(const usb_config_desc_t *cfg_desc);

/**
 * @brief Print UVC specific descriptor in human readable form
 *
 * This is a callback function that is called from USB Host library,
 * when it wants to print full configuration descriptor to stdout.
 *
 * @param[in] _desc UVC specific descriptor
 */
void uvc_print_desc(const usb_standard_desc_t *_desc);

/**
 * @brief Retrieve the list of frame descriptors for a specific streaming interface in a UVC device.
 *
 * This function extracts all frame descriptors associated with the given interface number
 * and organizes them into a list of `uvc_host_frame_info_t` structures.
 *
 * @param[in] config_desc         Pointer to the USB configuration descriptor.
 * @param[in] uvc_index           Index of UVC function you want to use.
 * @param[out] frame_info_list    Pointer to a list of frame info structures (allocated dynamically).
 * @param[inout] list_size          Pointer to store the number of frames in the list.
 *
 * @return
 *      - ESP_OK: Success.
 *      - ESP_ERR_INVALID_ARG: One or more invalid arguments.
 *      - ESP_ERR_NOT_FOUND: Input header descriptor not found.
 *      - ESP_ERR_NO_MEM: Memory allocation failure.
 */
/**
 * @brief Get the VideoControl interface number of a UVC function
 *
 * Unit and terminal control requests are addressed to the VideoControl interface, not to
 * the VideoStreaming interface the stream was opened on.
 *
 * @param[in]  cfg_desc         Configuration descriptor
 * @param[in]  uvc_index        Index of the UVC function
 * @param[out] bInterfaceNumber VideoControl interface number
 * @return
 *     - ESP_OK: Success
 *     - ESP_ERR_INVALID_ARG: cfg_desc or bInterfaceNumber is NULL
 *     - ESP_ERR_NOT_FOUND: No such UVC function
 */
esp_err_t uvc_desc_get_control_interface_num(const usb_config_desc_t *cfg_desc, uint8_t uvc_index, uint8_t *bInterfaceNumber);

/**
 * @brief Find an Extension Unit by its GUID
 *
 * Unit IDs are assigned per camera, so a GUID is the only portable way to address an
 * extension unit. A camera may expose several.
 *
 * @param[in]  cfg_desc  Configuration descriptor
 * @param[in]  uvc_index Index of the UVC function
 * @param[in]  guid      16-byte GUID, little-endian as it appears in the descriptor
 * @param[out] bUnitID   Unit ID of the matching extension unit
 * @return
 *     - ESP_OK: Success
 *     - ESP_ERR_INVALID_ARG: An argument is NULL
 *     - ESP_ERR_NOT_FOUND: No extension unit with this GUID
 */
esp_err_t uvc_desc_find_extension_unit(const usb_config_desc_t *cfg_desc, uint8_t uvc_index, const uint8_t guid[16], uint8_t *bUnitID);

/**
 * @brief Find a terminal by its type
 *
 * Input and output terminals are both searched. Their standard wTerminalType ranges do not
 * overlap, so the type alone identifies which is wanted.
 *
 * @param[in]  cfg_desc      Configuration descriptor
 * @param[in]  uvc_index     Index of the UVC function
 * @param[in]  terminal_type wTerminalType to look for, e.g. UVC_HOST_ITT_CAMERA
 * @param[out] bTerminalID   Terminal ID of the matching terminal
 * @return
 *     - ESP_OK: Success
 *     - ESP_ERR_INVALID_ARG: cfg_desc or bTerminalID is NULL
 *     - ESP_ERR_NOT_FOUND: This function has no terminal of that type
 */
esp_err_t uvc_desc_find_terminal(const usb_config_desc_t *cfg_desc, uint8_t uvc_index, uint16_t terminal_type, uint8_t *bTerminalID);

/**
 * @brief Check whether a unit or terminal claims a control in its bmControls
 *
 * @param[in]  cfg_desc    Configuration descriptor
 * @param[in]  uvc_index   Index of the UVC function
 * @param[in]  unit_id     bUnitID or bTerminalID to inspect
 * @param[in]  control_bit Bit position in bmControls, counted from D0 across all bytes
 * @param[out] supported   Whether the bit is set
 * @return
 *     - ESP_OK: The unit was found and supported was written
 *     - ESP_ERR_INVALID_ARG: cfg_desc or supported is NULL
 *     - ESP_ERR_NOT_FOUND: No unit with this ID, or it carries no bmControls
 */
esp_err_t uvc_desc_unit_supports_control(const usb_config_desc_t *cfg_desc, uint8_t uvc_index, uint8_t unit_id, uint8_t control_bit, bool *supported);

esp_err_t uvc_desc_get_frame_list(
    const usb_config_desc_t *config_desc,
    uint8_t uvc_index,
    uvc_host_frame_info_t (*frame_info_list)[],
    size_t *list_size);

#ifdef __cplusplus
}
#endif
