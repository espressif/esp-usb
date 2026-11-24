/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/*
Mock HID device based on the TinyUSB device stack.
The device emulates an HID mouse that periodically generates movement reports on its interrupt IN endpoint
Intended to run on a second ESP board, connected to the USB Host DUT.
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
 * @brief SET_REPORT payload byte that switches the mock's interrupt IN report length
 *
 * Test hook: this first byte selects the interrupt IN report length.
 * A zero second byte selects the mouse report; a nonzero byte selects the long report.
 * This lets host tests exercise a multi-packet interrupt IN transfer that ends
 * in a short packet.
 */
#define MOCK_HID_SET_REPORT_MODE_TRIGGER      0x5B

/**
 * @brief Length of the mock's long interrupt IN report in bytes
 *
 * Two full MPS (8-byte) packets followed by a 1-byte short packet
 */
#define MOCK_HID_LONG_REPORT_LEN              17

/**
 * @brief Install the TinyUSB driver and run the mock HID mouse device
 *
 * A FreeRTOS task is started that periodically sends HID mouse movement
 * reports (square pattern) on the interrupt IN endpoint.
 *
 * @note This function must be called only once (from a mock device test case)
 */
void mock_hid_dev_run(void);

#ifdef __cplusplus
}
#endif
