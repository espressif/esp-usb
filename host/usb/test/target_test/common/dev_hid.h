/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include "usb/usb_types_ch9.h"
#include "usb/usb_types_stack.h"

/*
Some tests where the ESP (acting as host) will require that a particular test
device acting as an HID mouse be connected. This header contains
functions to get information and descriptors about that test device.

The descriptors match the TinyUSB based mock HID mouse device (see mock_hid.c):
a composite device with 3 HID interfaces, each with its own interrupt IN
endpoint and (report length, EP MPS) combination:
  - Interface 0 (EP 0x81): 5-byte report, MPS 8 (DWORD)      - report < MPS, short packet
  - Interface 1 (EP 0x82): 4-byte report, MPS 4 (DWORD)      - report == MPS, multi-packet fill
  - Interface 2 (EP 0x83): 4-byte report, MPS 5 (non-DWORD)  - report < MPS, short packet
If you are connecting a different HID mouse, please update the descriptors in
dev_hid.c accordingly.
*/

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Number of HID interfaces of the composite test device
 */
#define DEV_HID_NUM_ITF     3

/**
 * @brief HID test device information
 *
 * Structure containing basic information about one HID interface on
 * the test device.
 */
typedef struct {
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t in_ep_addr;
    uint16_t in_ep_mps;     // Interrupt IN endpoint MPS
    uint8_t report_len;     // HID report length in bytes
} dev_hid_info_t;

/**
 * @brief Initialize the test device
 *
 * @note Call this before running tests. This is necessary due to IDF-9886
 */
void dev_hid_init(void);

/**
 * @brief Get information about one of the test device's HID interfaces
 *
 * @param[in] itf_num Interface number (0 .. DEV_HID_NUM_ITF - 1)
 * @return Information object
 */
const dev_hid_info_t *dev_hid_get_info(uint8_t itf_num);

/**
 * @brief Get the test device's descriptor
 *
 * @param[in] speed Test device's current speed
 * @return Device descriptor
 */
const usb_device_desc_t *dev_hid_get_dev_desc(usb_speed_t speed);

/**
 * @brief Get the test device's configuration descriptor
 *
 * @param[in] speed Test device's current speed
 * @return Configuration descriptor
 */
const usb_config_desc_t *dev_hid_get_config_desc(usb_speed_t speed);

/**
 * @brief Get one of the test device's HID interface descriptors
 *
 * @param[in] speed Test device's current speed
 * @param[in] itf_num Interface number (0 .. DEV_HID_NUM_ITF - 1)
 * @return HID interface descriptor
 */
const usb_intf_desc_t *dev_hid_get_intf_desc(usb_speed_t speed, uint8_t itf_num);

/**
 * @brief Get one of the test device's HID interrupt IN endpoint descriptors
 *
 * @param[in] speed Test device's current speed
 * @param[in] itf_num Interface number (0 .. DEV_HID_NUM_ITF - 1)
 * @return Interrupt IN endpoint descriptor
 */
const usb_ep_desc_t *dev_hid_get_in_ep_desc(usb_speed_t speed, uint8_t itf_num);

/**
 * @brief Get the test device's manufacturer string descriptor
 *
 * @return Manufacturer string descriptor
 */
const usb_str_desc_t *dev_hid_get_str_desc_manu(void);

/**
 * @brief Get the test device's product string descriptor
 *
 * @return Product string descriptor
 */
const usb_str_desc_t *dev_hid_get_str_desc_prod(void);

/**
 * @brief Get the test device's serial number string descriptor
 *
 * @return Serial number string descriptor
 */
const usb_str_desc_t *dev_hid_get_str_desc_ser(void);

#ifdef __cplusplus
}
#endif
