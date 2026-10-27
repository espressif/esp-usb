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

// --------------------------------------------------- Setup packets -----------------------------------------------------

/**
 * @brief HID class-specific bRequest codes
 *
 * See section 7.2 of the HID 1.11 specification for more details
 */
#define HID_REQ_SET_REPORT          0x09

/**
 * @brief Initializer for a HID class SET_REPORT request
 *
 * Sends a report to an interface of a HID class device
 *
 * See section 7.2.2 of the HID 1.11 specification for more details
 */
#define USB_SETUP_PACKET_INIT_HID_SET_REPORT(setup_pkt_ptr, report_type, report_id, intf_num, report_len) ({  \
    (setup_pkt_ptr)->bmRequestType = USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_CLASS | USB_BM_REQUEST_TYPE_RECIP_INTERFACE;   \
    (setup_pkt_ptr)->bRequest = HID_REQ_SET_REPORT;  \
    (setup_pkt_ptr)->wValue = ((report_type) << 8) | ((report_id) & 0xFF); \
    (setup_pkt_ptr)->wIndex = (intf_num);  \
    (setup_pkt_ptr)->wLength = (report_len);  \
})

/**
 * @brief SET_REPORT payload byte that triggers a stall of the mock's interrupt IN endpoint
 *
 * Test hook: the mock device stalls its interrupt IN endpoint only when the first byte of a
 * SET_REPORT report equals this value; any other SET_REPORT is ignored. Gives host tests a
 * deterministic way to trigger an endpoint STALL; the endpoint is recovered by
 * ClearFeature(ENDPOINT_HALT), which also resets its data toggle.
 */
#define MOCK_HID_SET_REPORT_STALL_TRIGGER     0xA5

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
