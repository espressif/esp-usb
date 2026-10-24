/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "usb/usb_helpers.h"
#include "usb/usb_types_cdc.h"
#include "cdc_host_descriptor_parsing.h"

#define USB_SUBCLASS_NULL       0x00
#define USB_SUBCLASS_COMMON     0x02
#define USB_PROTOCOL_NULL       0x00
#define USB_DEVICE_PROTOCOL_IAD 0x01

static const char *TAG = "cdc_common_parsing";

/**
 * @brief Searches interface by index and verifies its CDC-compliance
 *
 * @param[in] device_desc Pointer to Device descriptor
 * @param[in] config_desc Pointer do Configuration descriptor
 * @param[in] intf_idx    Index of the required interface
 * @return true  The required interface is CDC compliant
 * @return false The required interface is NOT CDC compliant
 */
static bool cdc_parse_is_cdc_compliant(const usb_device_desc_t *device_desc, const usb_config_desc_t *config_desc, uint8_t intf_idx)
{
    int desc_offset = 0;

    if (device_desc->bDeviceClass == USB_CLASS_PER_INTERFACE || device_desc->bDeviceClass == USB_CLASS_COMM) {
        const usb_intf_desc_t *intf_desc = usb_parse_interface_descriptor(config_desc, intf_idx, 0, NULL);
        if (intf_desc->bInterfaceClass == USB_CLASS_COMM) {
            // 1. This is a Communication Device Class: Class defined in Interface descriptor
            return true;
        }
    }

    if (((device_desc->bDeviceClass == USB_CLASS_MISC) && (device_desc->bDeviceSubClass == USB_SUBCLASS_COMMON) &&
            (device_desc->bDeviceProtocol == USB_DEVICE_PROTOCOL_IAD)) ||
            ((device_desc->bDeviceClass == USB_CLASS_PER_INTERFACE) && (device_desc->bDeviceSubClass == USB_SUBCLASS_NULL) &&
             (device_desc->bDeviceProtocol == USB_PROTOCOL_NULL))) {
        const usb_standard_desc_t *this_desc = (const usb_standard_desc_t *)config_desc;
        while ((this_desc = usb_parse_next_descriptor_of_type(this_desc, config_desc->wTotalLength, USB_B_DESCRIPTOR_TYPE_INTERFACE_ASSOCIATION, &desc_offset))) {
            const usb_iad_desc_t *iad_desc = (const usb_iad_desc_t *)this_desc;
            if ((iad_desc->bFirstInterface == intf_idx) &&
                    (iad_desc->bInterfaceCount == 2)) {
                // 2. This is a composite device, that uses Interface Association Descriptor
                return true;
            }
        };
    }
    return false;
}

static cdc_host_common_func_array_t *cdc_parse_functional_descriptors(const usb_intf_desc_t *intf_desc, uint16_t total_len, int desc_offset, int *desc_cnt)
{
    int func_desc_cnt = 0;
    int intf_offset = desc_offset;
    const usb_standard_desc_t *cdc_desc = (const usb_standard_desc_t *)intf_desc;
    while ((cdc_desc = usb_parse_next_descriptor(cdc_desc, total_len, &intf_offset))) {
        if (cdc_desc->bDescriptorType != ((USB_CLASS_COMM << 4) | USB_B_DESCRIPTOR_TYPE_INTERFACE)) {
            if (func_desc_cnt == 0) {
                return NULL;
            }
            break;
        }
        func_desc_cnt++;
    }
    *desc_cnt = func_desc_cnt;

    if (func_desc_cnt == 0) {
        return NULL;
    }

    cdc_host_common_func_array_t *func_desc = malloc(func_desc_cnt * sizeof(**func_desc));
    if (!func_desc) {
        ESP_LOGE(TAG, "Out of memory for functional descriptors");
        return NULL;
    }

    intf_offset = desc_offset;
    cdc_desc = (const usb_standard_desc_t *)intf_desc;
    for (int i = 0; i < func_desc_cnt; i++) {
        cdc_desc = usb_parse_next_descriptor(cdc_desc, total_len, &intf_offset);
        (*func_desc)[i] = cdc_desc;
    }
    return func_desc;
}

static void cdc_parse_data_endpoints(const usb_config_desc_t *config_desc, const usb_intf_desc_t *intf_desc, int desc_offset,
                                     cdc_host_common_parsed_info_t *info_ret)
{
    int temp_offset = desc_offset;
    for (int i = 0; i < intf_desc->bNumEndpoints; i++) {
        const usb_ep_desc_t *this_ep = usb_parse_endpoint_descriptor_by_index(intf_desc, i, config_desc->wTotalLength, &desc_offset);
        assert(this_ep);
        if (USB_EP_DESC_GET_XFERTYPE(this_ep) == USB_TRANSFER_TYPE_INTR) {
            info_ret->notif_intf = intf_desc;
            info_ret->notif_ep = this_ep;
        } else if (USB_EP_DESC_GET_XFERTYPE(this_ep) == USB_TRANSFER_TYPE_BULK) {
            info_ret->data_intf = intf_desc;
            if (USB_EP_DESC_GET_EP_DIR(this_ep)) {
                info_ret->in_ep = this_ep;
            } else {
                info_ret->out_ep = this_ep;
            }
        }
        desc_offset = temp_offset;
    }
}

esp_err_t cdc_host_common_parse_interface_descriptor(const usb_device_desc_t *device_desc, const usb_config_desc_t *config_desc, uint8_t intf_idx,
                                                     cdc_host_common_parsed_info_t *info_ret)
{
    int desc_offset = 0;

    ESP_RETURN_ON_FALSE(device_desc && config_desc && info_ret, ESP_ERR_INVALID_ARG, TAG, "Invalid descriptor argument");
    memset(info_ret, 0, sizeof(cdc_host_common_parsed_info_t));

    const usb_intf_desc_t *first_intf_desc = usb_parse_interface_descriptor(config_desc, intf_idx, 0, &desc_offset);
    ESP_RETURN_ON_FALSE(first_intf_desc, ESP_ERR_NOT_FOUND, TAG, "Required interface no %d was not found", intf_idx);

    if (first_intf_desc->bInterfaceClass == USB_CLASS_VENDOR_SPEC) {
        // Support iot_usbh_cdc vendor-specific CDC-like interfaces with two bulk endpoints and optional notification endpoint.
        int func_offset = desc_offset;
        if (first_intf_desc->bNumEndpoints == 3) {
            const usb_standard_desc_t *this_desc = (const usb_standard_desc_t *)first_intf_desc;
            this_desc = usb_parse_next_descriptor_of_type(
                            this_desc, config_desc->wTotalLength, ((USB_CLASS_COMM << 4) | USB_B_DESCRIPTOR_TYPE_INTERFACE), &func_offset);
            if (this_desc) {
                const cdc_header_desc_t *cdc_header = (const cdc_header_desc_t *)this_desc;
                ESP_RETURN_ON_FALSE(cdc_header->bDescriptorSubtype == USB_CDC_DESC_SUBTYPE_HEADER, ESP_ERR_NOT_FOUND,
                                    TAG, "Interface %d is not a supported CDC interface", intf_idx);
                ESP_RETURN_ON_FALSE(cdc_header->bcdCDC == 0x0110, ESP_ERR_NOT_FOUND, TAG, "Interface %d is not a supported CDC interface", intf_idx);
            }
        }
        cdc_parse_data_endpoints(config_desc, first_intf_desc, desc_offset, info_ret);
    } else {
        const bool cdc_compliant = cdc_parse_is_cdc_compliant(device_desc, config_desc, intf_idx);
        if (cdc_compliant) {
            info_ret->notif_intf = first_intf_desc;
            // Keep functional descriptors for common descriptor query APIs while using iot_usbh_cdc endpoint matching.
            info_ret->func = cdc_parse_functional_descriptors(first_intf_desc, config_desc->wTotalLength, desc_offset, &info_ret->func_cnt);
            if (info_ret->func == NULL && info_ret->func_cnt > 0) {
                return ESP_ERR_NO_MEM;
            }

            const usb_ep_desc_t *this_ep = usb_parse_endpoint_descriptor_by_index(first_intf_desc, 0, config_desc->wTotalLength, &desc_offset);
            if (this_ep && USB_EP_DESC_GET_XFERTYPE(this_ep) == USB_TRANSFER_TYPE_INTR) {
                info_ret->notif_ep = this_ep;
            }

            const int num_of_alternate = usb_parse_interface_number_of_alternate(config_desc, intf_idx + 1);
            for (int i = 0; i < num_of_alternate + 1; i++) {
                const usb_intf_desc_t *second_intf_desc = usb_parse_interface_descriptor(config_desc, intf_idx + 1, i, &desc_offset);
                if (second_intf_desc && second_intf_desc->bNumEndpoints == 2) {
                    cdc_parse_data_endpoints(config_desc, second_intf_desc, desc_offset, info_ret);
                    break;
                }
            }
        }
    }

    if (info_ret->notif_ep) {
        ESP_LOGI(TAG, "Found NOTIF endpoint: %d", USB_EP_DESC_GET_EP_NUM(info_ret->notif_ep));
    }
    if (info_ret->out_ep) {
        ESP_LOGI(TAG, "Found OUT endpoint: %d", USB_EP_DESC_GET_EP_NUM(info_ret->out_ep));
    }
    if (info_ret->in_ep) {
        ESP_LOGI(TAG, "Found IN endpoint: %d", USB_EP_DESC_GET_EP_NUM(info_ret->in_ep));
    }

    return (info_ret->in_ep && info_ret->out_ep) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

void cdc_host_common_print_desc(const usb_standard_desc_t *_desc)
{
    if (_desc->bDescriptorType != ((USB_CLASS_COMM << 4) | USB_B_DESCRIPTOR_TYPE_INTERFACE)) {
        return;
    }

    switch (((cdc_header_desc_t *)_desc)->bDescriptorSubtype) {
    case USB_CDC_DESC_SUBTYPE_HEADER: {
        cdc_header_desc_t *desc = (cdc_header_desc_t *)_desc;
        printf("\t*** CDC Header Descriptor ***\n");
        printf("\tbcdCDC: %d.%d0\n", ((desc->bcdCDC >> 8) & 0xF), ((desc->bcdCDC >> 4) & 0xF));
        break;
    }
    case USB_CDC_DESC_SUBTYPE_CALL: {
        cdc_acm_call_desc_t *desc = (cdc_acm_call_desc_t *)_desc;
        printf("\t*** CDC Call Descriptor ***\n");
        printf("\tbmCapabilities: 0x%02X\n", desc->bmCapabilities.val);
        printf("\tbDataInterface: %d\n", desc->bDataInterface);
        break;
    }
    case USB_CDC_DESC_SUBTYPE_ACM: {
        cdc_acm_acm_desc_t *desc = (cdc_acm_acm_desc_t *)_desc;
        printf("\t*** CDC ACM Descriptor ***\n");
        printf("\tbmCapabilities: 0x%02X\n", desc->bmCapabilities.val);
        break;
    }
    case USB_CDC_DESC_SUBTYPE_UNION: {
        cdc_union_desc_t *desc = (cdc_union_desc_t *)_desc;
        printf("\t*** CDC Union Descriptor ***\n");
        printf("\tbControlInterface: %d\n", desc->bControlInterface);
        printf("\tbSubordinateInterface[0]: %d\n", desc->bSubordinateInterface[0]);
        break;
    }
    default:
        ESP_LOGW(TAG, "Unsupported CDC specific descriptor");
        break;
    }
}
