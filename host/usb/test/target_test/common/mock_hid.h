/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/*
Mock HID device based on the TinyUSB device stack.
The device emulates a composite HID mouse with 4 HID interfaces, each with its own
interrupt IN endpoint and (report length, EP MPS) combination:
  - Interface 0 (EP 0x81): 5-byte report, MPS 8 (DWORD)      - report < MPS, short packet
  - Interface 1 (EP 0x82): 4-byte report, MPS 4, bInterval 1 - report == MPS, multi-packet fill, strict (never NAKs)
  - Interface 2 (EP 0x83): 4-byte report, MPS 5 (non-DWORD)  - report < MPS, short packet
  - Interface 3 (EP 0x84): 5-byte report, MPS 5 (non-DWORD)  - report == MPS, full packets
Each interface periodically generates mouse movement reports on its interrupt IN endpoint.
Interface 0 additionally has an interrupt OUT endpoint (EP 0x01, MPS 8): received OUT
reports are echoed back on its interrupt IN endpoint (loopback), so host tests can verify
interrupt OUT transfers end-to-end.
Intended to run on a second ESP board, connected to the USB Host DUT.

Requires CONFIG_TINYUSB_HID_COUNT=4 (one TinyUSB HID instance per interface),
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
 * @brief Mock HID device behavior modes
 *
 * Selected at runtime by a SET_REPORT carrying MOCK_HID_SET_REPORT_MODE_TRIGGER. The device
 * falls back to MOCK_HID_DEVICE_MODE_MOUSE_REPORT when unmounted (the host powers off the
 * port at the end of every test case).
 */
typedef enum {
    MOCK_HID_DEVICE_MODE_MOUSE_REPORT,  /**< Mouse reports on all interfaces (default) */
    MOCK_HID_DEVICE_MODE_LOOPBACK_ONLY, /**< Loopback interface sends only echoes */
} mock_hid_device_mode_t;

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
 * @brief SET_REPORT payload byte that selects the mock's device mode
 *
 * Test hook: this first byte identifies a device-mode request. The second byte
 * contains the mock_hid_device_mode_t value to select.
 *
 * @note The value 0x5B is an arbitrary test-only magic value. It has no USB or
 * HID protocol meaning.
 */
#define MOCK_HID_SET_REPORT_MODE_TRIGGER      0x5B

/**
 * @brief Interrupt IN endpoint addresses (one per HID interface)
 */
#define MOCK_HID_EP_IN_R5_MPS8      0x81    // Interface 0
#define MOCK_HID_EP_IN_R4_MPS4      0x82    // Interface 1 (strict)
#define MOCK_HID_EP_IN_R4_MPS5      0x83    // Interface 2
#define MOCK_HID_EP_IN_R5_MPS5      0x84    // Interface 3

/**
 * @brief Interrupt OUT endpoint of the loopback interface (interface 0): received reports echo on EP 0x81
 */
#define MOCK_HID_EP_OUT_LOOPBACK    0x01

/**
 * @brief Interrupt endpoint MPS per interface (TUD_HID_INOUT_DESCRIPTOR uses one MPS for both EPs)
 */
#define MOCK_HID_EP_MPS_ITF0        8       // DWORD aligned
#define MOCK_HID_EP_MPS_ITF1        4       // DWORD aligned
#define MOCK_HID_EP_MPS_ITF2        5       // Non-DWORD aligned
#define MOCK_HID_EP_MPS_ITF3        5       // Non-DWORD aligned

/**
 * @brief HID report lengths
 */
#define MOCK_HID_REPORT_LEN_5B      5       // buttons, X, Y, wheel, AC pan
#define MOCK_HID_REPORT_LEN_4B      4       // buttons, X, Y, wheel

/**
 * @brief Endpoint polling intervals (bInterval)
 */
#define MOCK_HID_EP_INTERVAL_MS     10
#define MOCK_HID_EP_STRICT_INTERVAL 1

/**
 * @brief Number of HID interfaces of the mock composite device
 */
#define MOCK_HID_NUM_ITF        4
#define MOCK_HID_STRICT_ITF     1
#define MOCK_HID_LOOPBACK_ITF   0   // Interface with the interrupt OUT endpoint (echoes on its IN endpoint)

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
