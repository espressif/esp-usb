/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdlib.h>
#include "usb/usb_types_ch9.h"
#include "usb/usb_types_stack.h"
#include "dev_hid.h"

/*
Some tests where the ESP (acting as host) will require that a particular test
device acting as an HID mouse be connected. That test device's information and
descriptors are defined in this file.

The descriptors match the TinyUSB based mock HID mouse device (see mock_hid.c):
a composite device with 3 HID interfaces, each with its own interrupt IN
endpoint and (report length, EP MPS) combination. If you are connecting a
different HID mouse, please update the descriptor and getter functions
accordingly.

------------------------------ Device Descriptor -------------------------------
bLength                     : 0x12      (18 bytes)
bDescriptorType             : 0x01      (Device Descriptor)
bcdUSB                      : 0x0200    (2.00)
bDeviceClass                : 0x00
bDeviceSubClass             : 0x00
bDeviceProtocol             : 0x00
bMaxPacketSize0             : 0x40      (64 bytes)
idVendor                    : 0x303A    (Espressif Systems)
idProduct                   : 0x4004    (Mock HID Mouse)
bcdDevice                   : 0x0100    (1.00)
iManufacturer               : 1
iProduct                    : 2
iSerial                     : 3
bNumConfigurations          : 1

--------------------------- Configuration Descriptor ---------------------------
bLength                     : 0x09      (9 bytes)
bDescriptorType             : 0x02      (Configuration Descriptor)
wTotalLength                : 0x0054    (84 bytes: 9 + 3 * (9 + 9 + 7))
bNumInterfaces              : 0x03      (3 Interfaces)
bConfigurationValue         : 0x01      (Configuration 1)
iConfiguration              : 0x00      (No String Descriptor)
bmAttributes                : 0xA0
 D7: Reserved, set 1            : 0x01
 D6: Self Powered               : 0x00  (no)
 D5: Remote Wakeup              : 0x01  (yes)
 D4..0: Reserved, set 0         : 0x00
MaxPower                    : 0x32      (100 mA)

----------------------------- Interface Descriptor -----------------------------
One per HID interface (bInterfaceNumber = 0, 1, 2):
bLength                     : 0x09      (9 bytes)
bDescriptorType             : 0x04      (Interface Descriptor)
bAlternateSetting           : 0x00
bNumEndpoints               : 0x01      (1 Endpoint)
bInterfaceClass             : 0x03      (HID - Human Interface Device)
bInterfaceSubClass          : 0x01      (Boot Interface)
bInterfaceProtocol          : 0x02      (Mouse)
iInterface                  : 0x00      (No String Descriptor)

-------------------------------- HID Descriptor --------------------------------
One per HID interface:
bLength                     : 0x09      (9 bytes)
bDescriptorType             : 0x21      (HID Descriptor)
bcdHID                      : 0x0111    (HID Version 1.11)
bCountryCode                : 0x00      (00 = not localized)
bNumDescriptors             : 0x01
Descriptor 1:
bDescriptorType             : 0x22      (Class=Report)
wDescriptorLength           : 0x004D    (77 bytes: interface 0, 5-byte report)
                            : 0x0034    (52 bytes: interfaces 1, 2, 4-byte report)

------------------------------ Endpoint Descriptor -----------------------------
One per HID interface:
bLength                     : 0x07      (7 bytes)
bDescriptorType             : 0x05      (Endpoint Descriptor)
bmAttributes                : 0x03      (TransferType=Interrupt)
bInterval                   : 0x0A      (10 ms)
Interface 0: bEndpointAddress : 0x81, wMaxPacketSize : 0x0008, report length 5
Interface 1: bEndpointAddress : 0x82, wMaxPacketSize : 0x0004, report length 4
Interface 2: bEndpointAddress : 0x83, wMaxPacketSize : 0x0005, report length 4

---------------------------- String Descriptor Manu ----------------------------
bLength                     : 0x10      (16 bytes)
bDescriptorType             : 0x03      (String Descriptor)
wData                       : "TinyUSB"

---------------------------- String Descriptor Prod ----------------------------
bLength                     : 0x1E      (30 bytes)
bDescriptorType             : 0x03      (String Descriptor)
wData                       : "Mock HID Mouse"

---------------------------- String Descriptor Set -----------------------------
bLength                     : 0x0E      (14 bytes)
bDescriptorType             : 0x03      (String Descriptor)
wData                       : "123456"
*/

// --------------------------- Device Information ------------------------------

static const dev_hid_info_t dev_info[DEV_HID_NUM_ITF] = {
    {.bInterfaceNumber = 0, .bAlternateSetting = 0, .in_ep_addr = 0x81, .in_ep_mps = 8, .report_len = 5},
    {.bInterfaceNumber = 1, .bAlternateSetting = 0, .in_ep_addr = 0x82, .in_ep_mps = 4, .report_len = 4},
    {.bInterfaceNumber = 2, .bAlternateSetting = 0, .in_ep_addr = 0x83, .in_ep_mps = 5, .report_len = 4},
};

// ------------------------------- Descriptors ---------------------------------

static const usb_device_desc_t dev_desc = {
    .bLength = USB_DEVICE_DESC_SIZE,
    .bDescriptorType = USB_B_DESCRIPTOR_TYPE_DEVICE,
    .bcdUSB = 0x0200,       // 2.00
    .bDeviceClass = USB_CLASS_PER_INTERFACE,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = 64,
    .idVendor = 0x303A,     // Espressif Systems
    .idProduct = 0x4004,    // Mock HID Mouse
    .bcdDevice = 0x0100,    // 1.00
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

static const usb_config_desc_t config_desc = {
    .bLength = USB_CONFIG_DESC_SIZE,
    .bDescriptorType = USB_B_DESCRIPTOR_TYPE_CONFIGURATION,
    .wTotalLength = 0x0054,         // 84 bytes (9 + 3 * (9 + 9 + 7))
    .bNumInterfaces = DEV_HID_NUM_ITF,
    .bConfigurationValue = 1,
    .iConfiguration = 0,
    .bmAttributes = 0xA0,
    .bMaxPower = 0x32,              // 100 mA
};

static const usb_intf_desc_t intf_desc[DEV_HID_NUM_ITF] = {
    {
        .bLength = USB_INTF_DESC_SIZE,
        .bDescriptorType = USB_B_DESCRIPTOR_TYPE_INTERFACE,
        .bInterfaceNumber = 0,
        .bAlternateSetting = 0,
        .bNumEndpoints = 1,
        .bInterfaceClass = USB_CLASS_HID,
        .bInterfaceSubClass = 0x01,     // Boot Interface
        .bInterfaceProtocol = 0x02,     // Mouse
        .iInterface = 0,                // (No String Descriptor)
    },
    {
        .bLength = USB_INTF_DESC_SIZE,
        .bDescriptorType = USB_B_DESCRIPTOR_TYPE_INTERFACE,
        .bInterfaceNumber = 1,
        .bAlternateSetting = 0,
        .bNumEndpoints = 1,
        .bInterfaceClass = USB_CLASS_HID,
        .bInterfaceSubClass = 0x01,     // Boot Interface
        .bInterfaceProtocol = 0x02,     // Mouse
        .iInterface = 0,                // (No String Descriptor)
    },
    {
        .bLength = USB_INTF_DESC_SIZE,
        .bDescriptorType = USB_B_DESCRIPTOR_TYPE_INTERFACE,
        .bInterfaceNumber = 2,
        .bAlternateSetting = 0,
        .bNumEndpoints = 1,
        .bInterfaceClass = USB_CLASS_HID,
        .bInterfaceSubClass = 0x01,     // Boot Interface
        .bInterfaceProtocol = 0x02,     // Mouse
        .iInterface = 0,                // (No String Descriptor)
    },
};

static const usb_ep_desc_t in_ep_desc[DEV_HID_NUM_ITF] = {
    {
        .bLength = USB_EP_DESC_SIZE,
        .bDescriptorType = USB_B_DESCRIPTOR_TYPE_ENDPOINT,
        .bEndpointAddress = 0x81,       // EP 1 IN
        .bmAttributes = USB_BM_ATTRIBUTES_XFER_INT,
        .wMaxPacketSize = 0x0008,
        .bInterval = 0x0A,              // 10 ms
    },
    {
        .bLength = USB_EP_DESC_SIZE,
        .bDescriptorType = USB_B_DESCRIPTOR_TYPE_ENDPOINT,
        .bEndpointAddress = 0x82,       // EP 2 IN
        .bmAttributes = USB_BM_ATTRIBUTES_XFER_INT,
        .wMaxPacketSize = 0x0004,
        .bInterval = 0x0A,              // 10 ms
    },
    {
        .bLength = USB_EP_DESC_SIZE,
        .bDescriptorType = USB_B_DESCRIPTOR_TYPE_ENDPOINT,
        .bEndpointAddress = 0x83,       // EP 3 IN
        .bmAttributes = USB_BM_ATTRIBUTES_XFER_INT,
        .wMaxPacketSize = 0x0005,
        .bInterval = 0x0A,              // 10 ms
    },
};

/*
String descriptors are dynamically initialized due to issues with static
initialization of variable length array members. See IDF-9886.
*/

static const usb_str_desc_t str_desc_manu_base = {
    .bLength = sizeof(usb_str_desc_t) + (7 * sizeof(uint16_t)),
    .bDescriptorType = USB_B_DESCRIPTOR_TYPE_STRING,
};
static const uint16_t str_desc_manu_data[] = {
    0x0054,         // 'T'
    0x0069,         // 'i'
    0x006E,         // 'n'
    0x0079,         // 'y'
    0x0055,         // 'U'
    0x0053,         // 'S'
    0x0042,         // 'B'
};
static uint8_t *str_desc_manu[sizeof(str_desc_manu_base) + sizeof(str_desc_manu_data)];

static const usb_str_desc_t str_desc_prod_base = {
    .bLength = sizeof(usb_str_desc_t) + (14 * sizeof(uint16_t)),
    .bDescriptorType = USB_B_DESCRIPTOR_TYPE_STRING,
};
static const uint16_t str_desc_prod_data[] = {
    /*
    The following string encoded in UTF-16LE

    "Mock HID Mouse"
    */
    0x004D, 0x006F, 0x0063, 0x006B, 0x0020, 0x0048, 0x0049, 0x0044, 0x0020,
    0x004D, 0x006F, 0x0075, 0x0073, 0x0065,
};
static uint8_t *str_desc_prod[sizeof(str_desc_prod_base) + sizeof(str_desc_prod_data)];

static const usb_str_desc_t str_desc_ser_base = {
    .bLength = sizeof(usb_str_desc_t) + (6 * sizeof(uint16_t)),
    .bDescriptorType = USB_B_DESCRIPTOR_TYPE_STRING,
};
static const uint16_t str_desc_ser_data[] = {
    0x0031,         // '1'
    0x0032,         // '2'
    0x0033,         // '3'
    0x0034,         // '4'
    0x0035,         // '5'
    0x0036,         // '6'
};
static uint8_t *str_desc_ser[sizeof(str_desc_ser_base) + sizeof(str_desc_ser_data)];

// -------------------------------- Functions ----------------------------------

void dev_hid_init(void)
{
    // Dynamically initialize string descriptors due to compiler limitations (see IDF-9886)
    // Still relevant for older IDF releases
    uint8_t *ptr;

    // Initialize manufacturer string descriptor
    ptr = (uint8_t *)str_desc_manu;
    memcpy(ptr, &str_desc_manu_base, sizeof(str_desc_manu_base));
    ptr += sizeof(str_desc_manu_base);
    memcpy(ptr, &str_desc_manu_data, sizeof(str_desc_manu_data));

    // Initialize product string descriptor
    ptr = (uint8_t *)str_desc_prod;
    memcpy(ptr, &str_desc_prod_base, sizeof(str_desc_prod_base));
    ptr += sizeof(str_desc_prod_base);
    memcpy(ptr, &str_desc_prod_data, sizeof(str_desc_prod_data));

    // Initialize serial string descriptor
    ptr = (uint8_t *)str_desc_ser;
    memcpy(ptr, &str_desc_ser_base, sizeof(str_desc_ser_base));
    ptr += sizeof(str_desc_ser_base);
    memcpy(ptr, &str_desc_ser_data, sizeof(str_desc_ser_data));
}

const dev_hid_info_t *dev_hid_get_info(uint8_t itf_num)
{
    return &dev_info[itf_num];
}

const usb_device_desc_t *dev_hid_get_dev_desc(usb_speed_t speed)
{
    return &dev_desc;
}

const usb_config_desc_t *dev_hid_get_config_desc(usb_speed_t speed)
{
    return &config_desc;
}

const usb_intf_desc_t *dev_hid_get_intf_desc(usb_speed_t speed, uint8_t itf_num)
{
    return &intf_desc[itf_num];
}

const usb_ep_desc_t *dev_hid_get_in_ep_desc(usb_speed_t speed, uint8_t itf_num)
{
    return &in_ep_desc[itf_num];
}

const usb_str_desc_t *dev_hid_get_str_desc_manu(void)
{
    return (const usb_str_desc_t *)str_desc_manu;
}

const usb_str_desc_t *dev_hid_get_str_desc_prod(void)
{
    return (const usb_str_desc_t *)str_desc_prod;
}

const usb_str_desc_t *dev_hid_get_str_desc_ser(void)
{
    return (const usb_str_desc_t *)str_desc_ser;
}
