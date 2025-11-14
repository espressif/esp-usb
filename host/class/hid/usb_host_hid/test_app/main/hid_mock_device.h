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
    TEST_HID_MOCK_DEVICE_WITH_LARGE_REPORT,
    TEST_HID_MOCK_DEVICE_MAX,
};

void hid_mock_device_set_mode(const enum test_hid_mock_device mode);

void hid_mock_device_run(void);
