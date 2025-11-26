/*
 * SPDX-FileCopyrightText: 2022-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdint.h>

// Define test HID mock device types
enum test_hid_mock_device {
    TEST_HID_MOCK_DEVICE_WITH_ONE_IFACE = 0,
    TEST_HID_MOCK_DEVICE_WITH_TWO_IFACES,
    TEST_HID_MOCK_DEVICE_WITH_REPORT_DESC_1905B,
    TEST_HID_MOCK_DEVICE_WITH_REPORT_DESC_32KB,
    TEST_HID_MOCK_DEVICE_MAX,
};

/**
 * @brief Set HID mock device test mode
 *
 * Note: Should be called before hid_mock_device_run()
 *
 * @param[in] mode   Test mode to set
 */
void hid_mock_device_set_mode(const enum test_hid_mock_device mode);

/**
 * @brief Run HID mock device
 *
 * Sets up and starts the TinyUSB device stack with the selected test mode
 * Waits for the device to be connected
 *
 * Note: Should be called after hid_mock_device_set_mode()
 */
void hid_mock_device_run(void);
