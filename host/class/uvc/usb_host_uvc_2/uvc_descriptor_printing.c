/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "usb/usb_host.h"
#include "usb/uvc_host.h"
#include "uvc_types_priv.h"

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
    printf("%s NOT IMPLEMENTED YET\n", __func__);
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
