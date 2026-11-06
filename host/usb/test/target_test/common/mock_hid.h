/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/*
Mock HID device based on the TinyUSB device stack.
The device emulates a composite HID mouse with 3 HID interfaces, each with its own
interrupt IN endpoint and (report length, EP MPS) combination:
  - Interface 0 (EP 0x81): 5-byte report, MPS 8 (DWORD)      - report < MPS, short packet
  - Interface 1 (EP 0x82): 4-byte report, MPS 4 (DWORD)      - report == MPS, multi-packet fill
  - Interface 2 (EP 0x83): 4-byte report, MPS 5 (non-DWORD)  - report < MPS, short packet
Each interface periodically generates mouse movement reports on its interrupt IN endpoint.
Intended to run on a second ESP board, connected to the USB Host DUT.

Requires CONFIG_TINYUSB_HID_COUNT=3 (one TinyUSB HID instance per interface),
enforced in the test app's sdkconfig.defaults.
*/

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Number of HID interfaces of the mock composite device
 */
#define MOCK_HID_NUM_ITF    3

/**
 * @brief Install the TinyUSB driver and run the mock HID mouse device
 *
 * A FreeRTOS task is started that periodically sends HID mouse movement
 * reports (square pattern) on the interrupt IN endpoint of each interface.
 *
 * @note This function must be called only once (from a mock device test case)
 */
void mock_hid_dev_run(void);

#ifdef __cplusplus
}
#endif
