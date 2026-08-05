/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "usb/usb_types_ch9.h"
#include "usb/usb_types_cdc.h"

typedef const usb_standard_desc_t *cdc_host_common_func_array_t[];

typedef struct {
    const usb_ep_desc_t *notif_ep;
    const usb_ep_desc_t *in_ep;
    const usb_ep_desc_t *out_ep;
    const usb_intf_desc_t *notif_intf;
    const usb_intf_desc_t *data_intf;
    cdc_host_common_func_array_t *func;
    int func_cnt;
} cdc_host_common_parsed_info_t;

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t cdc_host_common_parse_interface_descriptor(const usb_device_desc_t *device_desc, const usb_config_desc_t *config_desc, uint8_t intf_idx,
                                                     cdc_host_common_parsed_info_t *info_ret);
void cdc_host_common_print_desc(const usb_standard_desc_t *_desc);

#ifdef __cplusplus
}
#endif
