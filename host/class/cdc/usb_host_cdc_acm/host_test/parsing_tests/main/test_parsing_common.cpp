/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <catch2/catch_test_macros.hpp>
#include "descriptors/cdc_descriptors.hpp"

// The ACM parser has a header with the same name; exercise the common implementation explicitly.
#include "../../../../usb_host_cdc_common/private_include/cdc_host_descriptor_parsing.h"

static const usb_device_desc_t cdc_device_desc = {
    .bLength = USB_DEVICE_DESC_SIZE,
    .bDescriptorType = USB_B_DESCRIPTOR_TYPE_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = USB_CLASS_COMM,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = 64,
    .idVendor = 0x303A,
    .idProduct = 1,
    .bcdDevice = 0x0100,
    .iManufacturer = 0,
    .iProduct = 0,
    .iSerialNumber = 0,
    .bNumConfigurations = 1,
};

TEST_CASE("CDC common parser accepts single-interface endpoint layouts", "[common_parser]")
{
    uint8_t descriptors[] = {
        9, 2, 44, 0, 1, 1, 0, 0x80, 50,
        9, 4, 0, 0, 3, 2, 2, 1, 0,
        5, 0x24, 0, 0x10, 1,
        7, 5, 0x81, 2, 64, 0, 0,
        7, 5, 0x02, 2, 64, 0, 0,
        7, 5, 0x83, 3, 8, 0, 10,
    };
    bool has_notification = true;

    SECTION("Bulk endpoints without an interrupt endpoint") {
        descriptors[2] -= USB_EP_DESC_SIZE;
        descriptors[13] = 2;
        has_notification = false;
    }
    SECTION("Notification follows both bulk endpoints") {}
    SECTION("Notification precedes both bulk endpoints") {
        std::swap_ranges(descriptors + 23, descriptors + 30, descriptors + 37);
    }
    SECTION("Notification separates the bulk endpoints") {
        std::swap_ranges(descriptors + 30, descriptors + 37, descriptors + 37);
    }

    cdc_host_common_parsed_info_t parsed = {};
    const esp_err_t ret = cdc_host_common_parse_interface_descriptor(&cdc_device_desc, reinterpret_cast<const usb_config_desc_t *>(descriptors), 0, &parsed);
    REQUIRE(ret == ESP_OK);
    REQUIRE(parsed.data_intf == reinterpret_cast<const usb_intf_desc_t *>(descriptors + 9));
    REQUIRE(parsed.notif_intf == parsed.data_intf);
    REQUIRE(parsed.in_ep->bEndpointAddress == 0x81);
    REQUIRE(parsed.out_ep->bEndpointAddress == 0x02);
    REQUIRE((parsed.notif_ep != nullptr) == has_notification);
    if (has_notification) {
        REQUIRE(parsed.notif_ep->bEndpointAddress == 0x83);
    }
    REQUIRE(parsed.func_cnt == 1);
    REQUIRE((*parsed.func)[0] == reinterpret_cast<const usb_standard_desc_t *>(descriptors + 18));
    free(parsed.func);
}

TEST_CASE("CDC common parser accepts separate data interfaces and alternate settings", "[common_parser]")
{
    uint8_t descriptors[] = {
        9, 2, 62, 0, 2, 1, 0, 0x80, 50,
        9, 4, 0, 0, 1, 2, 2, 1, 0,
        5, 0x24, 0, 0x10, 1,
        7, 5, 0x83, 3, 8, 0, 10,
        9, 4, 1, 0, 0, 0x0A, 0, 0, 0,
        9, 4, 1, 1, 2, 0x0A, 0, 0, 0,
        7, 5, 0x81, 2, 64, 0, 0,
        7, 5, 0x02, 2, 64, 0, 0,
    };
    uint8_t alternate = 1;

    SECTION("Data endpoints use the default alternate setting") {
        std::memmove(descriptors + 30, descriptors + 39, sizeof(descriptors) - 39);
        descriptors[2] -= USB_INTF_DESC_SIZE;
        descriptors[33] = 0;
        alternate = 0;
    }
    SECTION("Data endpoints follow an empty alternate setting") {}

    cdc_host_common_parsed_info_t parsed = {};
    const esp_err_t ret = cdc_host_common_parse_interface_descriptor(&cdc_device_desc, reinterpret_cast<const usb_config_desc_t *>(descriptors), 0, &parsed);
    REQUIRE(ret == ESP_OK);
    REQUIRE(parsed.notif_intf->bInterfaceNumber == 0);
    REQUIRE(parsed.notif_ep->bEndpointAddress == 0x83);
    REQUIRE(parsed.data_intf->bInterfaceNumber == 1);
    REQUIRE(parsed.data_intf->bAlternateSetting == alternate);
    REQUIRE(parsed.in_ep->bEndpointAddress == 0x81);
    REQUIRE(parsed.out_ep->bEndpointAddress == 0x02);
    REQUIRE(parsed.func_cnt == 1);
    REQUIRE((*parsed.func)[0] == reinterpret_cast<const usb_standard_desc_t *>(descriptors + 18));
    free(parsed.func);
}

TEST_CASE("CDC common parser keeps bulk endpoint pairs on one interface", "[common_parser]")
{
    uint8_t descriptors[] = {
        9, 2, 60, 0, 2, 1, 0, 0x80, 50,
        9, 4, 0, 0, 2, 2, 2, 1, 0,
        5, 0x24, 0, 0x10, 1,
        7, 5, 0x81, 2, 64, 0, 0,
        7, 5, 0x02, 2, 64, 0, 0,
        9, 4, 1, 0, 2, 0x0A, 0, 0, 0,
        7, 5, 0x84, 2, 64, 0, 0,
        7, 5, 0x05, 2, 64, 0, 0,
    };

    SECTION("A complete first-interface pair takes precedence") {
        cdc_host_common_parsed_info_t parsed = {};
        REQUIRE(cdc_host_common_parse_interface_descriptor(&cdc_device_desc, reinterpret_cast<const usb_config_desc_t *>(descriptors), 0, &parsed) == ESP_OK);
        REQUIRE(parsed.data_intf->bInterfaceNumber == 0);
        REQUIRE(parsed.in_ep->bEndpointAddress == 0x81);
        REQUIRE(parsed.out_ep->bEndpointAddress == 0x02);
        free(parsed.func);
    }
    SECTION("Partial pairs from different interfaces cannot be combined") {
        descriptors[33] = 3;
        descriptors[49] = 3;
        cdc_host_common_parsed_info_t parsed = {};
        REQUIRE(cdc_host_common_parse_interface_descriptor(&cdc_device_desc, reinterpret_cast<const usb_config_desc_t *>(descriptors), 0, &parsed) == ESP_ERR_NOT_FOUND);
        REQUIRE(parsed.func == nullptr);
    }
}

TEST_CASE("CDC common parser rejects invalid arguments and missing interfaces", "[common_parser]")
{
    const uint8_t descriptors[] = {
        9, 2, 32, 0, 1, 1, 0, 0x80, 50,
        9, 4, 0, 0, 2, 2, 2, 1, 0,
        7, 5, 0x81, 2, 64, 0, 0,
        7, 5, 0x02, 2, 64, 0, 0,
    };
    const auto *config = reinterpret_cast<const usb_config_desc_t *>(descriptors);
    cdc_host_common_parsed_info_t parsed = {};

    SECTION("Null descriptor arguments") {
        REQUIRE(cdc_host_common_parse_interface_descriptor(nullptr, config, 0, &parsed) == ESP_ERR_INVALID_ARG);
        REQUIRE(cdc_host_common_parse_interface_descriptor(&cdc_device_desc, nullptr, 0, &parsed) == ESP_ERR_INVALID_ARG);
        REQUIRE(cdc_host_common_parse_interface_descriptor(&cdc_device_desc, config, 0, nullptr) == ESP_ERR_INVALID_ARG);
    }
    SECTION("Requested interface is absent") {
        REQUIRE(cdc_host_common_parse_interface_descriptor(&cdc_device_desc, config, 1, &parsed) == ESP_ERR_NOT_FOUND);
        REQUIRE(parsed.in_ep == nullptr);
        REQUIRE(parsed.out_ep == nullptr);
        REQUIRE(parsed.func == nullptr);
    }
}

TEST_CASE("CDC common parser rejects incomplete endpoint layouts", "[common_parser]")
{
    uint8_t descriptors[] = {
        9, 2, 37, 0, 1, 1, 0, 0x80, 50,
        9, 4, 0, 0, 2, 2, 2, 1, 0,
        5, 0x24, 0, 0x10, 1,
        7, 5, 0x81, 2, 64, 0, 0,
        7, 5, 0x02, 2, 64, 0, 0,
    };

    SECTION("Configuration ends before the second endpoint") {
        descriptors[2] = 30;
    }
    SECTION("Trailing byte cannot contain an endpoint header") {
        descriptors[2] = 31;
    }
    SECTION("Configuration ends inside the second endpoint payload") {
        descriptors[2] = 32;
    }
    SECTION("Configuration ends inside the interface descriptor") {
        descriptors[2] = 11;
    }
    SECTION("Configuration ends inside a functional descriptor") {
        descriptors[2] = 21;
    }
    SECTION("Functional descriptor omits its subtype") {
        std::memmove(descriptors + 20, descriptors + 23, sizeof(descriptors) - 23);
        descriptors[18] = 2;
        descriptors[2] -= 3;
    }
    SECTION("Endpoint descriptor is shorter than its required fields") {
        descriptors[30] = USB_EP_DESC_SIZE - 1;
    }
    SECTION("Interface descriptor is shorter than its required fields") {
        descriptors[9] = USB_INTF_DESC_SIZE - 1;
    }
    SECTION("Both bulk endpoints have the same direction") {
        descriptors[32] = 0x82;
    }

    cdc_host_common_parsed_info_t parsed = {};
    REQUIRE(cdc_host_common_parse_interface_descriptor(&cdc_device_desc, reinterpret_cast<const usb_config_desc_t *>(descriptors), 0, &parsed) == ESP_ERR_NOT_FOUND);
    REQUIRE(parsed.func == nullptr);
    REQUIRE(parsed.func_cnt == 0);
}

TEST_CASE("CDC common parser supports vendor bulk ports and validates optional CDC headers", "[common_parser]")
{
    uint8_t descriptors[] = {
        9, 2, 44, 0, 1, 1, 0, 0x80, 50,
        9, 4, 0, 0, 3, 0xFF, 0, 0, 0,
        5, 0x24, 0, 0x10, 1,
        7, 5, 0x81, 2, 64, 0, 0,
        7, 5, 0x02, 2, 64, 0, 0,
        7, 5, 0x83, 3, 8, 0, 10,
    };
    esp_err_t expected = ESP_OK;
    bool has_notification = true;

    SECTION("Vendor port with a CDC header and notification") {}
    SECTION("Vendor port without functional descriptors") {
        std::memmove(descriptors + 18, descriptors + 23, sizeof(descriptors) - 23);
        descriptors[2] -= 5;
    }
    SECTION("Vendor port with only bulk endpoints") {
        std::memmove(descriptors + 18, descriptors + 23, 2 * USB_EP_DESC_SIZE);
        descriptors[2] = 32;
        descriptors[13] = 2;
        has_notification = false;
    }
    SECTION("Unsupported CDC version") {
        descriptors[21] = 0x20;
        expected = ESP_ERR_NOT_FOUND;
    }
    SECTION("Functional descriptor is not a CDC header") {
        descriptors[20] = USB_CDC_DESC_SUBTYPE_ACM;
        expected = ESP_ERR_NOT_FOUND;
    }
    SECTION("CDC header omits part of its version field") {
        std::memmove(descriptors + 22, descriptors + 23, sizeof(descriptors) - 23);
        descriptors[18] = 4;
        descriptors[2] -= 1;
        expected = ESP_ERR_NOT_FOUND;
    }

    cdc_host_common_parsed_info_t parsed = {};
    REQUIRE(cdc_host_common_parse_interface_descriptor(&cdc_device_desc, reinterpret_cast<const usb_config_desc_t *>(descriptors), 0, &parsed) == expected);
    REQUIRE(parsed.func == nullptr);
    REQUIRE(parsed.func_cnt == 0);
    if (expected == ESP_OK) {
        REQUIRE(parsed.in_ep->bEndpointAddress == 0x81);
        REQUIRE(parsed.out_ep->bEndpointAddress == 0x02);
        REQUIRE(parsed.data_intf->bInterfaceNumber == 0);
        REQUIRE((parsed.notif_ep != nullptr) == has_notification);
    }
}

TEST_CASE("CDC common parser does not combine endpoints across alternate settings", "[common_parser]")
{
    uint8_t descriptors[] = {
        9, 2, 87, 0, 2, 1, 0, 0x80, 50,
        9, 4, 0, 0, 0, 2, 2, 1, 0,
        9, 4, 1, 0, 2, 0x0A, 0, 0, 0,
        7, 5, 0x81, 2, 64, 0, 0,
        7, 5, 0x02, 1, 64, 0, 0,
        9, 4, 1, 1, 2, 0x0A, 0, 0, 0,
        7, 5, 0x83, 1, 64, 0, 0,
        7, 5, 0x04, 2, 64, 0, 0,
        9, 4, 1, 2, 2, 0x0A, 0, 0, 0,
        7, 5, 0x85, 2, 64, 0, 0,
        7, 5, 0x06, 2, 64, 0, 0,
    };
    bool complete_pair = true;

    SECTION("Later alternate setting supplies the complete pair") {}
    SECTION("Complementary partial alternate settings are insufficient") {
        descriptors[2] = 64;
        complete_pair = false;
    }

    cdc_host_common_parsed_info_t parsed = {};
    REQUIRE(cdc_host_common_parse_interface_descriptor(&cdc_device_desc, reinterpret_cast<const usb_config_desc_t *>(descriptors), 0, &parsed) == (complete_pair ? ESP_OK : ESP_ERR_NOT_FOUND));
    REQUIRE(parsed.func == nullptr);
    if (complete_pair) {
        REQUIRE(parsed.data_intf->bAlternateSetting == 2);
        REQUIRE(parsed.in_ep->bEndpointAddress == 0x85);
        REQUIRE(parsed.out_ep->bEndpointAddress == 0x06);
    } else {
        REQUIRE(parsed.in_ep == nullptr);
        REQUIRE(parsed.out_ep == nullptr);
    }
}

TEST_CASE("CDC common parser stops functional descriptors at the first endpoint", "[common_parser]")
{
    const uint8_t descriptors[] = {
        9, 2, 45, 0, 1, 1, 0, 0x80, 50,
        9, 4, 0, 0, 2, 2, 2, 1, 0,
        5, 0x24, 0, 0x10, 1,
        4, 0x24, 2, 2,
        7, 5, 0x81, 2, 64, 0, 0,
        7, 5, 0x02, 2, 64, 0, 0,
        4, 0x24, 2, 0,
    };
    cdc_host_common_parsed_info_t parsed = {};
    REQUIRE(cdc_host_common_parse_interface_descriptor(&cdc_device_desc, reinterpret_cast<const usb_config_desc_t *>(descriptors), 0, &parsed) == ESP_OK);
    REQUIRE(parsed.func_cnt == 2);
    REQUIRE((*parsed.func)[0] == reinterpret_cast<const usb_standard_desc_t *>(descriptors + 18));
    REQUIRE((*parsed.func)[1] == reinterpret_cast<const usb_standard_desc_t *>(descriptors + 23));
    free(parsed.func);
}

TEST_CASE("CDC common parser validates composite interface associations", "[common_parser]")
{
    uint8_t descriptors[] = {
        9, 2, 54, 0, 2, 1, 0, 0x80, 50,
        8, 0x0B, 0, 2, 2, 2, 1, 0,
        9, 4, 0, 0, 0, 2, 2, 1, 0,
        5, 0x24, 0, 0x10, 1,
        9, 4, 1, 0, 2, 0x0A, 0, 0, 0,
        7, 5, 0x81, 2, 64, 0, 0,
        7, 5, 0x02, 2, 64, 0, 0,
    };
    usb_device_desc_t device = cdc_device_desc;
    device.bDeviceClass = USB_CLASS_MISC;
    device.bDeviceSubClass = 2;
    device.bDeviceProtocol = 1;
    esp_err_t expected = ESP_OK;

    SECTION("Complete IAD associates communication and data interfaces") {}
    SECTION("Non-CDC function class is rejected") {
        descriptors[13] = USB_CLASS_WIRELESS_CONTROLLER;
        expected = ESP_ERR_NOT_FOUND;
    }
    SECTION("Configuration ends inside the IAD") {
        descriptors[2] = 11;
        expected = ESP_ERR_NOT_FOUND;
    }
    SECTION("IAD descriptor omits its final field") {
        std::memmove(descriptors + 16, descriptors + 17, sizeof(descriptors) - 17);
        descriptors[9] = USB_IAD_DESC_SIZE - 1;
        descriptors[2] -= 1;
        expected = ESP_ERR_NOT_FOUND;
    }

    cdc_host_common_parsed_info_t parsed = {};
    REQUIRE(cdc_host_common_parse_interface_descriptor(&device, reinterpret_cast<const usb_config_desc_t *>(descriptors), 0, &parsed) == expected);
    if (expected == ESP_OK) {
        REQUIRE(parsed.notif_intf->bInterfaceNumber == 0);
        REQUIRE(parsed.data_intf->bInterfaceNumber == 1);
        REQUIRE(parsed.in_ep->bEndpointAddress == 0x81);
        REQUIRE(parsed.out_ep->bEndpointAddress == 0x02);
        REQUIRE(parsed.func_cnt == 1);
        free(parsed.func);
    } else {
        REQUIRE(parsed.func == nullptr);
        REQUIRE(parsed.in_ep == nullptr);
        REQUIRE(parsed.out_ep == nullptr);
    }
}

TEST_CASE("CDC common parser rejects A7672E wireless interfaces", "[common_parser]")
{
    const uint8_t *configs[] = {sima7672e_config_desc_fs, sima7672e_config_desc_hs};
    for (const uint8_t *config : configs) {
        cdc_host_common_parsed_info_t parsed = {};
        const auto *device = reinterpret_cast<const usb_device_desc_t *>(sima7672e_device_desc_fs_hs);
        const auto *descriptor = reinterpret_cast<const usb_config_desc_t *>(config);
        REQUIRE(cdc_host_common_parse_interface_descriptor(device, descriptor, 0, &parsed) == ESP_ERR_NOT_FOUND);
        REQUIRE(parsed.func == nullptr);
        REQUIRE(parsed.in_ep == nullptr);
        REQUIRE(parsed.out_ep == nullptr);
        REQUIRE(cdc_host_common_parse_interface_descriptor(device, descriptor, 3, &parsed) == ESP_OK);
        free(parsed.func);
    }
}
