/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include "hal/usb_dwc_hal.h"
#include "hal/usb_dwc_ll.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_private/critical_section.h"
#include "unity.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "dev_hid.h"
#include "hcd_common.h"
#include "mock_hid.h"
#include "hal/usb_dwc_ll.h"     // To be able to read PID of the host channel

static const char *TAG = "INTR";

#define FRAME_DURATION_US       1000    // frame duration in uS
#define UFRAME_DURATION_US      125     // µframe duration in uS
#define MULTI_PKT_NUM_PKTS      3       // Packets per multi-packet URB (shared by the IN and OUT multi-packet tests)

// --------------------------------------------------- Test Cases ------------------------------------------------------

// ------------------------------------------- Interrupt packets scheduling -------------------------------------------

#define URB_DONE_ITERS      50      // Number of URB completions (URB_DONE events) per URB in a test case
/*
Test HCD interrupt pipe URBs
Purpose:
    - Test that an interrupt pipe can be created
    - URBs can be created and enqueued to the interrupt pipe
    - Interrupt pipe returns HCD_PIPE_EVENT_URB_DONE
    - Test that URBs can be aborted when enqueued

The test device is a composite HID mouse with multiple HID interfaces (see dev_hid.h),
each with a different (report length, EP MPS) combination. Each interface is tested
one-by-one:
    - report < MPS: every report is a short packet, the transfer finishes early
    - report == MPS: every report is a full packet, the transfer fills the whole URB

Procedure:
    - Setup HCD and wait for connection
    - Allocate default pipe and enumerate the device
    - For each HID interface: setup interrupt pipe, allocate URBs, enqueue URBs,
      expect HCD_PIPE_EVENT_URB_DONE, check actual_num_bytes, requeue
    - Stop after fixed number of iterations
    - Deallocate URBs
    - Teardown

Note: Some mice will NAK until it is moved, so try moving the mouse around if this test case gets stuck.
*/

#define NUM_URBS                            3
#define NUM_URB_ITERS                       (NUM_URBS * URB_DONE_ITERS)

TEST_CASE("Test HCD interrupt pipe IN URBs", "[intr][tinyusb_mock]")
{
    usb_speed_t port_speed = test_hcd_wait_for_conn(port_hdl);  // Trigger a connection
    vTaskDelay(pdMS_TO_TICKS(100)); // Short delay send of SOF (for FS) or EOPs (for LS)

    hcd_pipe_handle_t default_pipe = test_hcd_pipe_alloc(port_hdl, NULL, 0, port_speed); // Create a default pipe (using a NULL EP descriptor)
    uint8_t dev_addr = test_hcd_enum_device(default_pipe);

    // Test each HID interface of the composite device one-by-one
    for (uint8_t itf_num = 0; itf_num < DEV_HID_NUM_ITF; itf_num++) {
        const dev_hid_info_t *hid_info = dev_hid_get_info(itf_num);
        const usb_ep_desc_t *in_ep_desc = dev_hid_get_in_ep_desc(port_speed, itf_num);
        const int data_buff_size = USB_EP_DESC_GET_MPS(in_ep_desc);
        ESP_LOGI(TAG, "Interface %d: EP 0x%02x, MPS %d, report %d bytes", itf_num, hid_info->in_ep_addr, data_buff_size, hid_info->report_len);

        // Allocate interrupt pipe and URBS
        hcd_pipe_handle_t intr_pipe = test_hcd_pipe_alloc(port_hdl, in_ep_desc, dev_addr, port_speed);
        urb_t *urb_list[NUM_URBS];
        for (int i = 0; i < NUM_URBS; i++) {
            urb_list[i] = test_hcd_alloc_urb(0, data_buff_size);
            urb_list[i]->transfer.num_bytes = data_buff_size;
            urb_list[i]->transfer.context = URB_CONTEXT_VAL;
        }

        // Enqueue URBs
        for (int i = 0; i < NUM_URBS; i++) {
            TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(intr_pipe, urb_list[i]));
        }
        for (int iter_count = NUM_URB_ITERS; iter_count > 0; iter_count--) {
            // Wait for an URB to be done
            TEST_HCD_EXPECT_PIPE_EVENT(intr_pipe, HCD_PIPE_EVENT_URB_DONE);
            // Dequeue the URB and check results
            urb_t *urb = hcd_urb_dequeue(intr_pipe);
            TEST_HCD_EXPECT_TRANSFER_STATUS(urb, USB_TRANSFER_STATUS_COMPLETED);
            TEST_ASSERT_EQUAL(URB_CONTEXT_VAL, urb->transfer.context);
            // Each report is a single packet: full packet when report == MPS, short packet (early termination) otherwise
            TEST_ASSERT_EQUAL(hid_info->report_len, urb->transfer.actual_num_bytes);
            // Byte 1 and 2 contains x and y movement respectively
            ESP_LOGI(TAG, "X mov %d, Y mov %d", (int8_t)urb->transfer.data_buffer[1], (int8_t)urb->transfer.data_buffer[2]);
            // Requeue URB
            if (iter_count > NUM_URBS) {
                TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(intr_pipe, urb));
            }
        }

        // Free URB list and pipe
        for (int i = 0; i < NUM_URBS; i++) {
            test_hcd_free_urb(urb_list[i]);
        }
        test_hcd_pipe_free(intr_pipe);
    }
    test_hcd_pipe_free(default_pipe);
    // Cleanup
    test_hcd_wait_for_disconn(port_hdl, false);
}

/*
Test HCD interrupt pipe multi-packet URBs
Purpose:
    - Test that an interrupt URB spanning multiple MPS-sized packets completes correctly
    - In buffer DMA mode, each packet activation is programmed at data_buffer + actual_bytes
      and the HAL requires a DWORD-aligned DMA address

The test device is a composite HID mouse (see dev_hid.h); each interface is tested one-by-one
with a URB of MULTI_PKT_NUM_PKTS * MPS bytes:
    - report == MPS (DWORD aligned MPS): no short packet, the URB completes once the
      buffer is full (actual_num_bytes == num_bytes)
    - report < MPS: the first packet is a short packet, the transfer finishes early
      (actual_num_bytes == report length). Safe even for a non-DWORD aligned MPS:
      a single packet means no misaligned re-arm
    - report == MPS (non-DWORD aligned MPS, interface 3): buffer DMA cannot continue the
      transfer (re-arm at data_buffer + MPS would be misaligned), so _slot_can_cont() fails
      it with HCD_PIPE_EVENT_ERROR_XFER. Scatter-Gather DMA has no such constraint and
      fills the buffer normally

Note: Some mice will NAK until it is moved, so try moving the mouse around if this test case gets stuck.
*/

#define MULTI_PKT_ITERS     URB_DONE_ITERS

/**
 * @brief Expect a multi-packet URB to be failed by the _slot_can_cont() alignment guard
 *
 * Buffer DMA design limitation: a full-packet report on a non-DWORD aligned MPS would make
 * the second activation's DMA address misaligned, so _slot_can_cont() fails the transfer
 * when its first full packet completes (see the test case note below).
 *
 * Expects HCD_PIPE_EVENT_ERROR_XFER, a halted pipe and the URB retired with
 * USB_TRANSFER_STATUS_ERROR, then flush+clears the pipe back to the active state.
 */
static void test_hcd_expect_xfer_error(hcd_pipe_handle_t intr_pipe, urb_t *urb)
{
    TEST_HCD_EXPECT_PIPE_EVENT(intr_pipe, HCD_PIPE_EVENT_ERROR_XFER);
    TEST_HCD_EXPECT_PIPE_STATE(intr_pipe, HCD_PIPE_STATE_HALTED);
    TEST_ASSERT_EQUAL_PTR(urb, hcd_urb_dequeue(intr_pipe));
    TEST_HCD_EXPECT_TRANSFER_STATUS(urb, USB_TRANSFER_STATUS_ERROR);
    TEST_ASSERT_EQUAL(0, urb->transfer.actual_num_bytes);
    // Return the pipe to the active state for the next iteration
    TEST_ASSERT_EQUAL(ESP_OK, hcd_pipe_command(intr_pipe, HCD_PIPE_CMD_FLUSH));
    TEST_ASSERT_EQUAL(ESP_OK, hcd_pipe_command(intr_pipe, HCD_PIPE_CMD_CLEAR));
    TEST_HCD_EXPECT_PIPE_STATE(intr_pipe, HCD_PIPE_STATE_ACTIVE);
}

TEST_CASE("Test HCD interrupt pipe multi-packet IN URBs", "[intr][tinyusb_mock]")
{
    usb_speed_t port_speed = test_hcd_wait_for_conn(port_hdl);  // Trigger a connection
    vTaskDelay(pdMS_TO_TICKS(100)); // Short delay send of SOF (for FS) or EOPs (for LS)

    hcd_pipe_handle_t default_pipe = test_hcd_pipe_alloc(port_hdl, NULL, 0, port_speed); // Create a default pipe (using a NULL EP descriptor)
    uint8_t dev_addr = test_hcd_enum_device(default_pipe);

    // Test each HID interface of the composite device one-by-one
    for (uint8_t itf_num = 0; itf_num < DEV_HID_NUM_ITF; itf_num++) {
        const dev_hid_info_t *hid_info = dev_hid_get_info(itf_num);
        const usb_ep_desc_t *in_ep_desc = dev_hid_get_in_ep_desc(port_speed, itf_num);
        const int mps = USB_EP_DESC_GET_MPS(in_ep_desc);
        const bool fills_buffer = (hid_info->report_len == mps);
#ifdef CONFIG_USB_HOST_DMA_MODE_BUFFER
        // A full-packet report on a non-DWORD aligned MPS makes the second activation's DMA
        // address misaligned: _slot_can_cont() must fail the transfer on the first full packet
        const bool expect_cont_err = fills_buffer && ((mps % sizeof(uint32_t)) != 0);
#else
        // Scatter-Gather DMA has no per-packet DWORD alignment constraint: the buffer is filled normally
        const bool expect_cont_err = false;
#endif // CONFIG_USB_HOST_DMA_MODE_BUFFER
        ESP_LOGI(TAG, "Interface %d: EP 0x%02x, MPS %d, report %d bytes", itf_num, hid_info->in_ep_addr, mps, hid_info->report_len);

        // Allocate interrupt pipe and a multi-packet URB
        const int num_bytes = MULTI_PKT_NUM_PKTS * mps;
        hcd_pipe_handle_t intr_pipe = test_hcd_pipe_alloc(port_hdl, in_ep_desc, dev_addr, port_speed);
        urb_t *urb = test_hcd_alloc_urb(0, num_bytes);
        urb->transfer.num_bytes = num_bytes;
        urb->transfer.context = URB_CONTEXT_VAL;

        for (int iter_count = MULTI_PKT_ITERS; iter_count > 0; iter_count--) {
            // Enqueue the URB and wait for it to be done
            TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(intr_pipe, urb));
            if (expect_cont_err) {
                // The first full MPS packet completes, then _slot_can_cont() fails the transfer
                test_hcd_expect_xfer_error(intr_pipe, urb);
                continue;
            }
            TEST_HCD_EXPECT_PIPE_EVENT(intr_pipe, HCD_PIPE_EVENT_URB_DONE);
            // Dequeue the URB and check results
            TEST_ASSERT_EQUAL_PTR(urb, hcd_urb_dequeue(intr_pipe));
            TEST_HCD_EXPECT_TRANSFER_STATUS(urb, USB_TRANSFER_STATUS_COMPLETED);
            TEST_ASSERT_EQUAL(URB_CONTEXT_VAL, urb->transfer.context);
            // report == MPS: the transfer completes once the buffer is full (MULTI_PKT_NUM_PKTS full MPS packets)
            // report <  MPS: the first packet is short, the transfer finishes early with a single report
            const int expected_num_bytes = fills_buffer ? num_bytes : hid_info->report_len;
            ESP_LOGI(TAG, "Actual num bytes: %d (expected %d)", urb->transfer.actual_num_bytes, expected_num_bytes);
            TEST_ASSERT_EQUAL(expected_num_bytes, urb->transfer.actual_num_bytes);
            // Each MPS-sized chunk is one HID report. Byte 1 and 2 contains x and y movement respectively
            for (int pkt = 0; pkt * mps < urb->transfer.actual_num_bytes; pkt++) {
                const uint8_t *report = urb->transfer.data_buffer + (pkt * mps);
                ESP_LOGI(TAG, "pkt %d: X mov %d, Y mov %d", pkt, (int8_t)report[1], (int8_t)report[2]);
            }
        }

        // Free URB and pipe
        test_hcd_free_urb(urb);
        test_hcd_pipe_free(intr_pipe);
    }
    test_hcd_pipe_free(default_pipe);
    // Cleanup
    test_hcd_wait_for_disconn(port_hdl, false);
}

// ------------------------------------------- Interrupt OUT transfers -------------------------------------------

#define OUT_URB_PAYLOAD_LEN     5                   // Short packet (< MPS 8): completes the device-side OUT transfer
#define OUT_ECHO_MAX_ITERS      URB_DONE_ITERS      // IN polls to find the looped-back report among the mouse reports

/*
Test HCD interrupt pipe OUT URBs (device loopback)
Purpose:
    - Test that a single-packet interrupt OUT URB completes on the bus
    - Verify the transfer end-to-end: the mock device echoes the received OUT report on its
      interrupt IN endpoint (interface 0 loopback); the host compares the payload

Procedure:
    - Setup HCD and enumerate the mock device
    - Allocate interrupt OUT (EP 0x01) and IN (EP 0x81) pipes on interface 0
    - Enqueue an IN URB (pending poll for the echo), then the OUT URB; expect it to complete
    - Poll the IN pipe until the echo arrives (skipping mouse reports), compare the payload
    - Teardown

Note: the payload is a short packet (< MPS): the device-side OUT transfer completes only on a
short packet or a full CFG_TUD_HID_EP_BUFSIZE buffer. Its first byte must be non-zero (tells the
echo apart from mouse reports) and differ from MOCK_HID_SET_REPORT_STALL_TRIGGER (the mock cannot
tell OUT endpoint data apart from a control SET_REPORT).
*/
TEST_CASE("Test HCD interrupt pipe OUT URBs (loopback)", "[intr][tinyusb_mock]")
{
    usb_speed_t port_speed = test_hcd_wait_for_conn(port_hdl);  // Trigger a connection
    vTaskDelay(pdMS_TO_TICKS(100)); // Short delay send of SOF (for FS) or EOPs (for LS)

    hcd_pipe_handle_t default_pipe = test_hcd_pipe_alloc(port_hdl, NULL, 0, port_speed); // Create a default pipe (using a NULL EP descriptor)
    uint8_t dev_addr = test_hcd_enum_device(default_pipe);

    // Interface 0: interrupt OUT endpoint 0x01, its echo is sent on interrupt IN endpoint 0x81
    const usb_ep_desc_t *out_ep_desc = dev_hid_get_out_ep_desc(port_speed);
    const usb_ep_desc_t *in_ep_desc = dev_hid_get_in_ep_desc(port_speed, DEV_HID_ITF_R5_MPS8);
    const int mps = USB_EP_DESC_GET_MPS(out_ep_desc);
    TEST_ASSERT_EQUAL(mps, USB_EP_DESC_GET_MPS(in_ep_desc));    // The echo fits the IN endpoint MPS
    hcd_pipe_handle_t out_pipe = test_hcd_pipe_alloc(port_hdl, out_ep_desc, dev_addr, port_speed);
    hcd_pipe_handle_t in_pipe = test_hcd_pipe_alloc(port_hdl, in_ep_desc, dev_addr, port_speed);

    // Single-packet OUT URB with a short-packet payload, recognizable among the mouse reports
    urb_t *out_urb = test_hcd_alloc_urb(0, mps);
    out_urb->transfer.num_bytes = OUT_URB_PAYLOAD_LEN;
    out_urb->transfer.context = URB_CONTEXT_VAL;
    for (int i = 0; i < OUT_URB_PAYLOAD_LEN; i++) {
        out_urb->transfer.data_buffer[i] = 0x11 * (i + 1);      // Non-zero first byte, not the STALL trigger
    }
    urb_t *in_urb = test_hcd_alloc_urb(0, mps);
    in_urb->transfer.num_bytes = mps;
    in_urb->transfer.context = URB_CONTEXT_VAL;

    // Enqueue the IN URB first, so a poll is pending when the device queues the echo
    TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(in_pipe, in_urb));
    TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(out_pipe, out_urb));

    // The OUT transfer completes once its single packet is ACKed
    TEST_HCD_EXPECT_PIPE_EVENT(out_pipe, HCD_PIPE_EVENT_URB_DONE);
    TEST_ASSERT_EQUAL_PTR(out_urb, hcd_urb_dequeue(out_pipe));
    TEST_HCD_EXPECT_TRANSFER_STATUS(out_urb, USB_TRANSFER_STATUS_COMPLETED);
    TEST_ASSERT_EQUAL(URB_CONTEXT_VAL, out_urb->transfer.context);
    TEST_ASSERT_EQUAL(OUT_URB_PAYLOAD_LEN, out_urb->transfer.actual_num_bytes);

    // Poll the IN pipe until the looped-back report arrives (mouse reports may complete the first URBs)
    bool echo_received = false;
    for (int i = 0; i < OUT_ECHO_MAX_ITERS; i++) {
        TEST_HCD_EXPECT_PIPE_EVENT(in_pipe, HCD_PIPE_EVENT_URB_DONE);
        TEST_ASSERT_EQUAL_PTR(in_urb, hcd_urb_dequeue(in_pipe));
        TEST_HCD_EXPECT_TRANSFER_STATUS(in_urb, USB_TRANSFER_STATUS_COMPLETED);
        ESP_LOGI(TAG, "IN report: %d bytes", in_urb->transfer.actual_num_bytes);
        ESP_LOG_BUFFER_HEXDUMP(TAG, in_urb->transfer.data_buffer, OUT_URB_PAYLOAD_LEN, ESP_LOG_INFO);
        if (in_urb->transfer.data_buffer[0] == 0) {
            // A mouse report (zero buttons byte): keep polling for the echo
            TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(in_pipe, in_urb));
            continue;
        }
        // The echo (non-zero first byte): verify the looped-back payload byte-for-byte
        TEST_ASSERT_EQUAL(OUT_URB_PAYLOAD_LEN, in_urb->transfer.actual_num_bytes);
        TEST_ASSERT_EQUAL_UINT8_ARRAY(out_urb->transfer.data_buffer, in_urb->transfer.data_buffer, OUT_URB_PAYLOAD_LEN);
        echo_received = true;
        break;  // Echo verified: stop polling, leave the URB dequeued
    }
    TEST_ASSERT_MESSAGE(echo_received, "Did not receive the looped-back OUT report");

    // Free URBs and pipes
    test_hcd_free_urb(in_urb);
    test_hcd_free_urb(out_urb);
    test_hcd_pipe_free(in_pipe);
    test_hcd_pipe_free(out_pipe);
    test_hcd_pipe_free(default_pipe);
    // Cleanup
    test_hcd_wait_for_disconn(port_hdl, false);
}

#define OUT_MULTI_PKT_SHORT_LEN         5                   // Short final packet: completes the device-side OUT transfer
#define OUT_MULTI_PKT_ITERS             URB_DONE_ITERS      // Repetitions: the data toggle must persist across URBs

/*
Test HCD interrupt pipe multi-packet OUT URBs (loopback)
Purpose:
    - Test that an interrupt OUT URB spanning multiple packets completes on the bus
    - In buffer DMA mode, each packet activation is programmed at data_buffer + actual_bytes and
      the HAL requires a DWORD-aligned DMA address: exercise the re-arm path in the OUT direction
      (the IN direction is covered by "Test HCD interrupt pipe multi-packet URBs"). The loopback
      interface's OUT endpoint has a DWORD-aligned MPS, so the transfer is valid in both DMA modes
    - Verify the transfer end-to-end: the mock device echoes the received OUT report on its
      interrupt IN endpoint (interface 0 loopback); the host compares the payload

Procedure:
    - Setup HCD and enumerate the mock device
    - Allocate interrupt OUT (EP 0x01) and IN (EP 0x81) pipes on interface 0
    - Enqueue an IN URB (pending poll for the echo), then the multi-packet OUT URB; expect it to complete
    - Poll the IN pipe until the echo arrives (skipping mouse reports), compare the payload
    - Requeue both URBs, repeat
    - Teardown

Note: the payload ends with a short packet (< MPS): the device-side OUT transfer completes only on a
short packet or a full CFG_TUD_HID_EP_BUFSIZE buffer. Its first byte must be non-zero (tells the
echo apart from mouse reports) and differ from MOCK_HID_SET_REPORT_STALL_TRIGGER (the mock cannot
tell OUT endpoint data apart from a control SET_REPORT).
*/
TEST_CASE("Test HCD interrupt pipe multi-packet OUT URBs", "[intr][tinyusb_mock]")
{
    usb_speed_t port_speed = test_hcd_wait_for_conn(port_hdl);  // Trigger a connection
    vTaskDelay(pdMS_TO_TICKS(100)); // Short delay send of SOF (for FS) or EOPs (for LS)

    hcd_pipe_handle_t default_pipe = test_hcd_pipe_alloc(port_hdl, NULL, 0, port_speed); // Create a default pipe (using a NULL EP descriptor)
    uint8_t dev_addr = test_hcd_enum_device(default_pipe);

    // Interface 0: interrupt OUT endpoint 0x01, its echo is sent on interrupt IN endpoint 0x81
    const usb_ep_desc_t *out_ep_desc = dev_hid_get_out_ep_desc(port_speed);
    const usb_ep_desc_t *in_ep_desc = dev_hid_get_in_ep_desc(port_speed, DEV_HID_ITF_R5_MPS8);
    const int mps = USB_EP_DESC_GET_MPS(out_ep_desc);
    TEST_ASSERT_EQUAL(mps, USB_EP_DESC_GET_MPS(in_ep_desc));    // The echo fits the IN endpoint MPS
    hcd_pipe_handle_t out_pipe = test_hcd_pipe_alloc(port_hdl, out_ep_desc, dev_addr, port_speed);
    hcd_pipe_handle_t in_pipe = test_hcd_pipe_alloc(port_hdl, in_ep_desc, dev_addr, port_speed);

    // Multi-packet OUT URB (full packets + a short final packet), recognizable among the mouse reports
    const int out_num_bytes = (MULTI_PKT_NUM_PKTS - 1) * mps + OUT_MULTI_PKT_SHORT_LEN;
    urb_t *out_urb = test_hcd_alloc_urb(0, out_num_bytes);
    out_urb->transfer.num_bytes = out_num_bytes;
    out_urb->transfer.context = URB_CONTEXT_VAL;
    for (int i = 0; i < out_num_bytes; i++) {
        out_urb->transfer.data_buffer[i] = 0x11 * (i + 1);      // Non-zero first byte, not the STALL trigger
    }
    // IN URB: a non-zero multiple of MPS (HCD enqueue requirement), completed early by a short packet
    urb_t *in_urb = test_hcd_alloc_urb(0, MULTI_PKT_NUM_PKTS * mps);
    in_urb->transfer.num_bytes = MULTI_PKT_NUM_PKTS * mps;
    in_urb->transfer.context = URB_CONTEXT_VAL;

    for (int iter_count = OUT_MULTI_PKT_ITERS; iter_count > 0; iter_count--) {
        // Enqueue the IN URB first, so a poll is pending when the device queues the echo
        TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(in_pipe, in_urb));
        TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(out_pipe, out_urb));

        // The OUT transfer completes once its last packet is ACKed
        TEST_HCD_EXPECT_PIPE_EVENT(out_pipe, HCD_PIPE_EVENT_URB_DONE);
        TEST_ASSERT_EQUAL_PTR(out_urb, hcd_urb_dequeue(out_pipe));
        TEST_HCD_EXPECT_TRANSFER_STATUS(out_urb, USB_TRANSFER_STATUS_COMPLETED);
        TEST_ASSERT_EQUAL(URB_CONTEXT_VAL, out_urb->transfer.context);
        TEST_ASSERT_EQUAL(out_num_bytes, out_urb->transfer.actual_num_bytes);

        // Poll the IN pipe until the looped-back report arrives (mouse reports may complete the first URBs)
        bool echo_received = false;
        for (int i = 0; i < OUT_ECHO_MAX_ITERS; i++) {
            TEST_HCD_EXPECT_PIPE_EVENT(in_pipe, HCD_PIPE_EVENT_URB_DONE);
            TEST_ASSERT_EQUAL_PTR(in_urb, hcd_urb_dequeue(in_pipe));
            TEST_HCD_EXPECT_TRANSFER_STATUS(in_urb, USB_TRANSFER_STATUS_COMPLETED);
            ESP_LOGI(TAG, "IN report: %d bytes", in_urb->transfer.actual_num_bytes);
            ESP_LOG_BUFFER_HEXDUMP(TAG, in_urb->transfer.data_buffer, OUT_URB_PAYLOAD_LEN, ESP_LOG_INFO);
            if (in_urb->transfer.data_buffer[0] == 0) {
                // A mouse report (zero buttons byte): keep polling for the echo
                TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(in_pipe, in_urb));
                continue;
            }
            // The echo (non-zero first byte): verify the looped-back payload byte-for-byte
            TEST_ASSERT_EQUAL(out_num_bytes, in_urb->transfer.actual_num_bytes);
            TEST_ASSERT_EQUAL_UINT8_ARRAY(out_urb->transfer.data_buffer, in_urb->transfer.data_buffer, out_num_bytes);
            echo_received = true;
            break;  // Echo verified: stop polling, the URB is re-enqueued by the next iteration
        }
        TEST_ASSERT_MESSAGE(echo_received, "Did not receive the looped-back OUT report");
    }

    // Free URBs and pipes
    test_hcd_free_urb(in_urb);
    test_hcd_free_urb(out_urb);
    test_hcd_pipe_free(in_pipe);
    test_hcd_pipe_free(out_pipe);
    test_hcd_pipe_free(default_pipe);
    // Cleanup
    test_hcd_wait_for_disconn(port_hdl, false);
}

// ------------------------------------------- Data toggle resync handling -------------------------------------------

#define HID_REPORT_TYPE_OUTPUT      0x02

/**
 * @brief Send a HID SET_REPORT request on the default pipe, triggering an endpoint STALL
 *
 * The report payload carries MOCK_HID_SET_REPORT_STALL_TRIGGER, upon which the mock HID device
 * (mock_hid.c) stalls its interrupt IN endpoint, giving this test a deterministic STALL trigger.
 */
static void test_hcd_hid_set_report(hcd_pipe_handle_t default_pipe, urb_t *urb, const dev_hid_info_t *hid_info)
{
    usb_setup_packet_t *setup = (usb_setup_packet_t *)urb->transfer.data_buffer;
    USB_SETUP_PACKET_INIT_HID_SET_REPORT(setup, HID_REPORT_TYPE_OUTPUT, 0, hid_info->bInterfaceNumber, hid_info->report_len);
    // First payload byte triggers the mock device to stall its interrupt IN endpoint (see mock_hid.h)
    urb->transfer.data_buffer[sizeof(usb_setup_packet_t)] = MOCK_HID_SET_REPORT_STALL_TRIGGER;
    urb->transfer.num_bytes = sizeof(usb_setup_packet_t) + setup->wLength;
    urb->transfer.context = URB_CONTEXT_VAL;

    TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(default_pipe, urb));
    TEST_HCD_EXPECT_PIPE_EVENT(default_pipe, HCD_PIPE_EVENT_URB_DONE);
    TEST_ASSERT_EQUAL_PTR(urb, hcd_urb_dequeue(default_pipe));
    TEST_HCD_EXPECT_TRANSFER_STATUS(urb, USB_TRANSFER_STATUS_COMPLETED);
}

/**
 * @brief Send ClearFeature(ENDPOINT_HALT) to an endpoint on the default pipe
 *
 * Resets the device endpoint's data toggle to DATA0 (USB 2.0 §9.4.5)
 */
static void test_hcd_clear_feature_ep_halt(hcd_pipe_handle_t default_pipe, urb_t *urb, uint8_t ep_addr)
{
    usb_setup_packet_t *setup = (usb_setup_packet_t *)urb->transfer.data_buffer;
    USB_SETUP_PACKET_INIT_CLEAR_EP_FEATURE(setup, ep_addr, ENDPOINT_HALT);
    urb->transfer.num_bytes = sizeof(usb_setup_packet_t);
    urb->transfer.context = URB_CONTEXT_VAL;

    TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(default_pipe, urb));
    TEST_HCD_EXPECT_PIPE_EVENT(default_pipe, HCD_PIPE_EVENT_URB_DONE);
    TEST_ASSERT_EQUAL_PTR(urb, hcd_urb_dequeue(default_pipe));
    TEST_HCD_EXPECT_TRANSFER_STATUS(urb, USB_TRANSFER_STATUS_COMPLETED);
}

/**
 * @brief Read the data PID of the host channel serving a given endpoint
 *
 * Scans the DWC host channels for the one programmed with the given device address and endpoint,
 * and returns its core-maintained HCTSIZ.Pid: the data toggle the channel uses for its next transaction
 *
 * @return Logical PID (0 = DATA0, 1 = DATA1), or -1 if no matching channel was found
 */
static int test_hcd_chan_get_pid(uint8_t dev_addr, uint8_t ep_addr)
{
    usb_dwc_dev_t *hw = USB_DWC_LL_GET_HW(TEST_PORT_NUM);
    const unsigned chan_num = usb_dwc_ll_ghwcfg_get_channel_num(hw);
    for (int i = 0; i < chan_num; i++) {
        volatile usb_dwc_host_chan_regs_t *chan = usb_dwc_ll_chan_get_regs(hw, i);
        if (chan->hcchar_reg.devaddr == dev_addr
                && chan->hcchar_reg.epnum == (ep_addr & 0x7F)
                && chan->hcchar_reg.epdir == ((ep_addr & 0x80) ? 1 : 0)) {
            return usb_dwc_ll_hctsiz_get_pid(chan);
        }
    }
    return -1;
}

/*
Test HCD interrupt pipe STALL data toggle resync
Purpose:
    - Verify that after a device STALL, HCD_PIPE_CMD_RESET_DATA_TOGGLE resyncs the pipe's data toggle to DATA0
    - ClearFeature(ENDPOINT_HALT) resets the device endpoint's data toggle to DATA0 (USB 2.0 §9.4.5),
      so the host channel's core-maintained HCTSIZ.Pid must be resynced to DATA0 as part of the
      endpoint halt recovery

Procedure:
    - Setup HCD, enumerate the mock HID device, allocate an interrupt IN pipe
    - Prime the data toggle to DATA1 by completing one interrupt IN transfer (a report is a single packet)
    - Check that HCD_PIPE_CMD_RESET_DATA_TOGGLE is rejected on an active pipe
    - Trigger a deterministic STALL via SET_REPORT (the mock device stalls its interrupt IN endpoint)
    - Enqueue an URB, expect HCD_PIPE_EVENT_ERROR_STALL, dequeue the errored URB
    - Resync the halted channel with HCD_PIPE_CMD_RESET_DATA_TOGGLE, check that HCTSIZ.Pid is DATA0
    - Clear the pipe with HCD_PIPE_CMD_CLEAR
    - Send ClearFeature(ENDPOINT_HALT) to the device endpoint (resets its toggle to DATA0)
    - Enqueue an URB and expect it to complete: the resynced channel accepts the device's DATA0 packet
    - Teardown
*/
TEST_CASE("Test HCD interrupt pipe STALL data toggle resync", "[intr][tinyusb_mock]")
{
    usb_speed_t port_speed = test_hcd_wait_for_conn(port_hdl);  // Trigger a connection
    vTaskDelay(pdMS_TO_TICKS(100)); // Short delay send of SOF (for FS) or EOPs (for LS)

    hcd_pipe_handle_t default_pipe = test_hcd_pipe_alloc(port_hdl, NULL, 0, port_speed); // Create a default pipe (using a NULL EP descriptor)
    uint8_t dev_addr = test_hcd_enum_device(default_pipe);

    // Allocate the interrupt IN pipe and URBs
    const dev_hid_info_t *hid_info = dev_hid_get_info(DEV_HID_ITF_R5_MPS8);
    const usb_ep_desc_t *in_ep_desc = dev_hid_get_in_ep_desc(port_speed, DEV_HID_ITF_R5_MPS8);
    const int data_buff_size = USB_EP_DESC_GET_MPS(in_ep_desc);
    hcd_pipe_handle_t intr_pipe = test_hcd_pipe_alloc(port_hdl, in_ep_desc, dev_addr, port_speed);
    urb_t *intr_urb = test_hcd_alloc_urb(0, data_buff_size);
    intr_urb->transfer.num_bytes = data_buff_size;
    intr_urb->transfer.context = URB_CONTEXT_VAL;
    urb_t *ctrl_urb = test_hcd_alloc_urb(0, sizeof(usb_setup_packet_t) + hid_info->report_len);

    // Prime the data toggle: one report is a single packet, so the next expected PID is DATA1
    TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(intr_pipe, intr_urb));
    TEST_HCD_EXPECT_PIPE_EVENT(intr_pipe, HCD_PIPE_EVENT_URB_DONE);
    TEST_ASSERT_EQUAL_PTR(intr_urb, hcd_urb_dequeue(intr_pipe));
    TEST_HCD_EXPECT_TRANSFER_STATUS(intr_urb, USB_TRANSFER_STATUS_COMPLETED);

    // The data toggle resync requires a halted pipe
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, hcd_pipe_command(intr_pipe, HCD_PIPE_CMD_RESET_DATA_TOGGLE));

    // Trigger a deterministic STALL on the mock device's interrupt IN endpoint
    test_hcd_hid_set_report(default_pipe, ctrl_urb, hid_info);

    // The next interrupt IN URB must fail with a STALL
    TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(intr_pipe, intr_urb));
    TEST_HCD_EXPECT_PIPE_EVENT(intr_pipe, HCD_PIPE_EVENT_ERROR_STALL);
    TEST_ASSERT_EQUAL_PTR(intr_urb, hcd_urb_dequeue(intr_pipe));
    TEST_HCD_EXPECT_TRANSFER_STATUS(intr_urb, USB_TRANSFER_STATUS_STALL);

    // The halted channel's HCTSIZ.Pid must still hold the stale DATA1 toggle
    int pid = test_hcd_chan_get_pid(dev_addr, hid_info->in_ep_addr);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(-1, pid, "Host channel for the interrupt IN pipe not found");
    TEST_ASSERT_EQUAL_MESSAGE(1, pid, "Channel PID unexpectedly changed on STALL");

    // Resync the halted channel's HCTSIZ.Pid to DATA0 (USB 2.0 §9.4.5), then clear the pipe
    TEST_ASSERT_EQUAL(ESP_OK, hcd_pipe_command(intr_pipe, HCD_PIPE_CMD_RESET_DATA_TOGGLE));
    pid = test_hcd_chan_get_pid(dev_addr, hid_info->in_ep_addr);
    TEST_ASSERT_EQUAL_MESSAGE(0, pid, "Channel PID not resynced to DATA0");
    TEST_ASSERT_EQUAL(ESP_OK, hcd_pipe_command(intr_pipe, HCD_PIPE_CMD_CLEAR));

    // Clear ENDPOINT_HALT on the device endpoint (resets its data toggle to DATA0)
    test_hcd_clear_feature_ep_halt(default_pipe, ctrl_urb, hid_info->in_ep_addr);

    // The pipe must accept the device's DATA0 packet: the transfer completes
    TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(intr_pipe, intr_urb));
    TEST_HCD_EXPECT_PIPE_EVENT(intr_pipe, HCD_PIPE_EVENT_URB_DONE);
    TEST_ASSERT_EQUAL_PTR(intr_urb, hcd_urb_dequeue(intr_pipe));
    TEST_HCD_EXPECT_TRANSFER_STATUS(intr_urb, USB_TRANSFER_STATUS_COMPLETED);

    // Free URBs and pipes
    test_hcd_free_urb(ctrl_urb);
    test_hcd_free_urb(intr_urb);
    test_hcd_pipe_free(intr_pipe);
    test_hcd_pipe_free(default_pipe);
    // Cleanup
    test_hcd_wait_for_disconn(port_hdl, false);
}

/*
Test HCD interrupt pipe suspend/resume preserves the data toggle
Purpose:
    - Verify that the suspend/resume sequence does NOT resync the pipe's data toggle:
      a suspended device maintains its internal status, including the endpoint data toggles
      (USB 2.0 §9.1.1.6), so the host channel's core-maintained HCTSIZ.Pid must be preserved
      across the pipe halt/flush and clear issued on suspend and resume

Procedure:
    - Setup HCD, enumerate the mock HID device, allocate an interrupt IN pipe
    - Prime the data toggle to DATA1 by completing one interrupt IN transfer (a report is a single packet)
    - Suspend the root port (all pipes halted and flushed), then resume it (all pipes cleared)
    - Check that the channel's HCTSIZ.Pid is still DATA1
    - Enqueue an URB and expect it to complete: host and device toggles are still in sync
    - Teardown
*/
TEST_CASE("Test HCD interrupt pipe suspend/resume keeps data toggle", "[intr][tinyusb_mock]")
{
    usb_speed_t port_speed = test_hcd_wait_for_conn(port_hdl);  // Trigger a connection
    vTaskDelay(pdMS_TO_TICKS(100)); // Short delay send of SOF (for FS) or EOPs (for LS)

    hcd_pipe_handle_t default_pipe = test_hcd_pipe_alloc(port_hdl, NULL, 0, port_speed); // Create a default pipe (using a NULL EP descriptor)
    uint8_t dev_addr = test_hcd_enum_device(default_pipe);

    // Allocate the interrupt IN pipe and an URB
    const dev_hid_info_t *hid_info = dev_hid_get_info(DEV_HID_ITF_R5_MPS8);
    const usb_ep_desc_t *in_ep_desc = dev_hid_get_in_ep_desc(port_speed, DEV_HID_ITF_R5_MPS8);
    const int data_buff_size = USB_EP_DESC_GET_MPS(in_ep_desc);
    hcd_pipe_handle_t intr_pipe = test_hcd_pipe_alloc(port_hdl, in_ep_desc, dev_addr, port_speed);
    urb_t *intr_urb = test_hcd_alloc_urb(0, data_buff_size);
    intr_urb->transfer.num_bytes = data_buff_size;
    intr_urb->transfer.context = URB_CONTEXT_VAL;

    // Prime the data toggle: one report is a single packet, so the next expected PID is DATA1
    TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(intr_pipe, intr_urb));
    TEST_HCD_EXPECT_PIPE_EVENT(intr_pipe, HCD_PIPE_EVENT_URB_DONE);
    TEST_ASSERT_EQUAL_PTR(intr_urb, hcd_urb_dequeue(intr_pipe));
    TEST_HCD_EXPECT_TRANSFER_STATUS(intr_urb, USB_TRANSFER_STATUS_COMPLETED);

    // Suspend and resume the root port: pipes are halted+flushed on suspend, cleared on resume
    hcd_pipe_handle_t pipe_list[2] = {default_pipe, intr_pipe};
    test_hcd_root_port_suspend_multi_pipe(port_hdl, pipe_list, 2);
    test_hcd_root_port_resume_multi_pipe(port_hdl, pipe_list, 2);

    // The channel's PID must be preserved at DATA1
    const int pid = test_hcd_chan_get_pid(dev_addr, hid_info->in_ep_addr);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(-1, pid, "Host channel for the interrupt IN pipe not found");
    TEST_ASSERT_EQUAL_MESSAGE(1, pid, "Channel PID resynced across suspend/resume");

    // Host and device toggles are still in sync: the transfer completes
    TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(intr_pipe, intr_urb));
    TEST_HCD_EXPECT_PIPE_EVENT(intr_pipe, HCD_PIPE_EVENT_URB_DONE);
    TEST_ASSERT_EQUAL_PTR(intr_urb, hcd_urb_dequeue(intr_pipe));
    TEST_HCD_EXPECT_TRANSFER_STATUS(intr_urb, USB_TRANSFER_STATUS_COMPLETED);

    // Free URB and pipes
    test_hcd_free_urb(intr_urb);
    test_hcd_pipe_free(intr_pipe);
    test_hcd_pipe_free(default_pipe);
    // Cleanup
    test_hcd_wait_for_disconn(port_hdl, false);
}

// --------------------------------------------- bInterval related tests ---------------------------------------------

/*
Test HCD interrupt pipe long intervals
Purpose:
    - Verify long, valid speed-specific bInterval values
    - Verify that a second packet of an interrupt URB is not immediately scheduled after the
      first packet completes

The mock HID endpoint descriptor is copied solely to override bInterval. The device accepts
the IN polls independently of the interval advertised by its original descriptor.
*/
TEST_CASE("Test HCD interrupt pipe long bInterval", "[intr][tinyusb_mock]")
{
    usb_speed_t port_speed = test_hcd_wait_for_conn(port_hdl);
    hcd_pipe_handle_t default_pipe = test_hcd_pipe_alloc(port_hdl, NULL, 0, port_speed);
    uint8_t dev_addr = test_hcd_enum_device(default_pipe);
    const usb_ep_desc_t *in_ep_desc = dev_hid_get_in_ep_desc(port_speed, DEV_HID_ITF_R4_MPS4); // Full-sized reports

    const uint8_t hs_intervals[] = {15, 16};  // 2048 ms and 4096 ms
    const uint8_t fsls_intervals[] = {127, 255}; // 127 ms and 255 ms
    const uint8_t *long_intervals = (port_speed == USB_SPEED_HIGH) ? hs_intervals : fsls_intervals;
    const size_t long_intervals_len = (port_speed == USB_SPEED_HIGH) ? sizeof(hs_intervals) : sizeof(fsls_intervals);
    for (size_t i = 0; i < long_intervals_len; i++) {
        ESP_LOGI(TAG, "Long binterval: %d", long_intervals[i]);
        usb_ep_desc_t long_interval_ep_desc = *in_ep_desc;
        long_interval_ep_desc.bInterval = long_intervals[i];

        hcd_pipe_handle_t intr_pipe = test_hcd_pipe_alloc(port_hdl, &long_interval_ep_desc, dev_addr, port_speed);
        const int mps = USB_EP_DESC_GET_MPS(&long_interval_ep_desc);
        urb_t *urb = test_hcd_alloc_urb(0, 2 * mps);
        urb->transfer.num_bytes = 2 * mps;

        const int64_t start_time_us = esp_timer_get_time();
        TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(intr_pipe, urb));
        ESP_LOGI(TAG, "Expecting URB_DONE event..");
        // Accommodates a valid initial NAK before the bInterval=16 transfer
        TEST_HCD_EXPECT_PIPE_EVENT_MS(intr_pipe, HCD_PIPE_EVENT_URB_DONE, 10000);
        const int64_t elapsed_us = esp_timer_get_time() - start_time_us;

        // One packet is polled immediately; the second must wait bInterval in the speed-specific unit.
        const int64_t interval_us = (port_speed == USB_SPEED_HIGH)
                                    ? (1LL << (long_intervals[i] - 1)) * 125
                                    : (int64_t)long_intervals[i] * 1000;
        TEST_ASSERT_MESSAGE(elapsed_us >= interval_us - 200000,
                            "Interrupt URB completed before its long bInterval elapsed");
        TEST_ASSERT_EQUAL_PTR(urb, hcd_urb_dequeue(intr_pipe));
        TEST_HCD_EXPECT_TRANSFER_STATUS(urb, USB_TRANSFER_STATUS_COMPLETED);
        TEST_ASSERT_EQUAL(2 * mps, urb->transfer.actual_num_bytes);

        test_hcd_free_urb(urb);
        test_hcd_pipe_free(intr_pipe);
    }
    test_hcd_pipe_free(default_pipe);
    test_hcd_wait_for_disconn(port_hdl, false);
}

#ifdef CONFIG_USB_HOST_DMA_MODE_BUFFER

#define HFNUM_MAX               USB_DWC_LL_HFNUM_FRAME_COUNT    // DWC2 HW frame number counter (14 bit)
#define HFNUM_PRE_WRAP_HS       1024                            // Number of µframes left before wrap for HS
#define HFNUM_PRE_WRAP_FS_LS    128                             // Number of frames left before wrap for FS/LS
/*
Test HCD interrupt pipe HFNUM rollover
Purpose:
    - Start a multi-packet interrupt URB shortly before the hardware's 14-bit HFNUM counter wraps
    - Verify that its second packet is not prematurely scheduled before the wrap
    - This test tests buffer DMA SW scheduler, not relevant for Scatter-Gather DMA

The endpoint descriptor is copied solely to use an interval longer than the time remaining to HFNUM
rollover. The first packet is serviced immediately; the second must be serviced after HFNUM has
wrapped. The old HFNUM-based deadline comparison schedules the second packet before the wrap instead.
*/
TEST_CASE("Test HCD interrupt pipe HFNUM rollover", "[intr][tinyusb_mock]")
{
    usb_speed_t port_speed = test_hcd_wait_for_conn(port_hdl);
    hcd_pipe_handle_t default_pipe = test_hcd_pipe_alloc(port_hdl, NULL, 0, port_speed);
    uint8_t dev_addr = test_hcd_enum_device(default_pipe);
    const usb_ep_desc_t *in_ep_desc = dev_hid_get_in_ep_desc(port_speed, DEV_HID_ITF_R4_MPS4); // Full-sized reports, every packet is a full packet
    usb_ep_desc_t rollover_ep_desc = *in_ep_desc;

    /*
     * Override the mock endpoint's bInterval to force the second packet across HFNUM rollover:
     * HS bInterval is exponent-encoded: T = (2^(binterval - 1)) * 125 uS = 2^(12 -1) * 125 uS = 256 ms (2048 µframes)
     * FS/LS bInterval is in milliseconds, T = 255 * 1 mS = 255 ms (255 frames)
     * Both exceed the 128 ms deliberately left before rollover below.
     */
    rollover_ep_desc.bInterval = (port_speed == USB_SPEED_HIGH) ? 12 : 255;

    // Time deliberately left before the wrap:
    // HS: 16384 − 1024 = 15360 → at most 1024 µframes remain → 1024 × 125 µs = 128 ms
    // FS/LS: 16384 − 128 = 16256 → at most 128 frames remain → 128 ms
    // Keep 128 ms before rollover: 1024 HS microframes or 128 FS/LS frames.
    // The second packet's 256 ms (HS) or 255 ms (FS/LS) interval must therefore cross rollover.

    // 128ms before the wrap (half of the bInterval lies before the wrap, half lies after the wrap)
    const uint32_t hfnum_before_wrap = (port_speed == USB_SPEED_HIGH) ? (HFNUM_MAX - HFNUM_PRE_WRAP_HS) : (HFNUM_MAX - HFNUM_PRE_WRAP_FS_LS);
    const int64_t hfnum_wait_timeout_us = (port_speed == USB_SPEED_HIGH) ? 3000000 : 18000000;

    // Wait until the frame number approaches 1024 µframes (128 frames) → 128ms before the wrap
    // Thus wait:
    // HS: 15360 µframes -> 15360 * 125 µs = 1.920 s
    // FS: 16256 frames -> 16256 * 1ms = 16.256 s
    ESP_LOGI(TAG, "Waiting (%.3f s) for pre-rollover frame number, to schedule a first packet", (port_speed == USB_SPEED_HIGH) ? (float)1.92 : (float)16.256);
    const int64_t wait_start_us = esp_timer_get_time();
    while (usb_dwc_ll_hfnum_get_frame_num(USB_DWC_LL_GET_HW(TEST_PORT_NUM)) < hfnum_before_wrap) {
        TEST_ASSERT_MESSAGE(esp_timer_get_time() - wait_start_us < hfnum_wait_timeout_us,
                            "Timed out waiting for HFNUM to approach its rollover");
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    // Get frame number  1024 µframes (128 frames) before the wrap
    const uint32_t start_hfnum = usb_dwc_ll_hfnum_get_frame_num(USB_DWC_LL_GET_HW(TEST_PORT_NUM));
    ESP_LOGI(TAG, "start frame number: %d\n", start_hfnum);

    // HFNUM delta tolerances as number of pre-wrap frames / 2
    const int start_hfnum_delta = (port_speed == USB_SPEED_HIGH) ? HFNUM_PRE_WRAP_HS / 2 : HFNUM_PRE_WRAP_FS_LS / 2;
    // Expected start frame number shall be within the HFRNUM - hfnum delta
    const int start_hfnum_expected = HFNUM_MAX - start_hfnum_delta;
    TEST_ASSERT_INT_WITHIN_MESSAGE(start_hfnum_delta, start_hfnum_expected, start_hfnum, "HFNUM was outside the expected pre-rollover range");

    hcd_pipe_handle_t intr_pipe = test_hcd_pipe_alloc(port_hdl, &rollover_ep_desc, dev_addr, port_speed);
    const int mps = USB_EP_DESC_GET_MPS(&rollover_ep_desc);
    urb_t *urb = test_hcd_alloc_urb(0, 2 * mps);
    urb->transfer.num_bytes = 2 * mps;
    // Enqueue URB 2 packet long
    // First packet is enqueued instantly, the second after the binterval (after 255 ms)
    // This timing forces the HFNUM rollover at at the second packet
    TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(intr_pipe, urb));
    TEST_HCD_EXPECT_PIPE_EVENT(intr_pipe, HCD_PIPE_EVENT_URB_DONE);

    // Get frame number after the urb is done, after the second packet has been processed
    const uint32_t completion_hfnum = usb_dwc_ll_hfnum_get_frame_num(USB_DWC_LL_GET_HW(TEST_PORT_NUM));

    // The completion hfnum shall be less than the start hfnum, thus forcing the rollover
    TEST_ASSERT_MESSAGE(completion_hfnum < start_hfnum, "Second periodic packet completed before HFNUM wrapped");

    /*
     * The second packet completes after its interval and rollover:
     * - HS: 2048 µframes minus the at-most 1024 µframes before rollover gives [1024, 2048).
     * - FS/LS: 255 frames minus the at-most 128 frames before rollover gives [127, 255).
     * Allow one extra interval when the first IN attempt is NAKed, giving enclosing ranges [1024, 4096)
     * and [127, 512). The midpoint/tolerance pairs below encode those ranges for Unity.
     */
    const int completion_hfnum_expected = (port_speed == USB_SPEED_HIGH) ? 2560 : 319;
    const int completion_hfnum_delta = (port_speed == USB_SPEED_HIGH) ? 1536 : 192;
    TEST_ASSERT_INT_WITHIN_MESSAGE(completion_hfnum_delta, completion_hfnum_expected, completion_hfnum, "Second periodic packet completed outside the expected post-rollover range");

    TEST_ASSERT_EQUAL_PTR(urb, hcd_urb_dequeue(intr_pipe));
    TEST_HCD_EXPECT_TRANSFER_STATUS(urb, USB_TRANSFER_STATUS_COMPLETED);
    TEST_ASSERT_EQUAL(2 * mps, urb->transfer.actual_num_bytes);

    test_hcd_free_urb(urb);
    test_hcd_pipe_free(intr_pipe);
    test_hcd_pipe_free(default_pipe);
    test_hcd_wait_for_disconn(port_hdl, false);
}

#define STRICT_INTERVAL_PACKET_COUNT     16
#define STRICT_INTERVAL_MAX_HFNUM_DELTA  (STRICT_INTERVAL_PACKET_COUNT + 8)

DEFINE_CRIT_SECTION_LOCK_STATIC(s_strict_sof_stress_lock);
static TaskHandle_t s_strict_sof_test_task;

/**
 * @brief Stress task that blocks interrupts for one full (micro)frame at a time
 *
 * The bInterval 1 scheduling test uses strict endpoint polling which is polled every (micro)frame, this task holds
 * a critical section for a full (micro)frame per iteration, creating worst-case HCD interrupt latency. The test
 * then verifies the packets are still serviced within the allowed frame delta.
 *
 * The task waits for a start notification, runs the stress iterations, notifies completion and deletes itself.
 *
 * @param arg Stress delay per iteration in microseconds (one full (micro)frame)
 */
static void strict_sof_stress_task(void *arg)
{
    const uint32_t delay_us = (uint32_t)(uintptr_t)arg;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // SOF stress iteration
    for (int i = 0; i < 8; i++) {
        esp_os_enter_critical(&s_strict_sof_stress_lock);
        esp_rom_delay_us(delay_us);
        esp_os_exit_critical(&s_strict_sof_stress_lock);
    }
    xTaskNotifyGive(s_strict_sof_test_task);
    vTaskDelete(NULL);
}

/*
Test HCD bInterval 1 scheduling
Purpose:
    - Transfer a sequence of full packets from the strict bInterval-1 HID endpoint
    - Verify they complete within the expected number of USB frames/microframes

The strict mock endpoint queues its next full report from its report-complete callback. This
keeps an IN packet available for every host poll, so the HFNUM delta reflects host scheduling.
*/
TEST_CASE("Test HCD interrupt pipe bInterval 1 scheduling", "[intr][tinyusb_mock]")
{
    usb_speed_t port_speed = test_hcd_wait_for_conn(port_hdl);
    hcd_pipe_handle_t default_pipe = test_hcd_pipe_alloc(port_hdl, NULL, 0, port_speed);
    uint8_t dev_addr = test_hcd_enum_device(default_pipe);
    const usb_ep_desc_t *in_ep_desc = dev_hid_get_in_ep_desc(port_speed, DEV_HID_STRICT_ITF);
    const int mps = USB_EP_DESC_GET_MPS(in_ep_desc);
    hcd_pipe_handle_t intr_pipe = test_hcd_pipe_alloc(port_hdl, in_ep_desc, dev_addr, port_speed);
    urb_t *urb = test_hcd_alloc_urb(0, STRICT_INTERVAL_PACKET_COUNT * mps);
    urb->transfer.num_bytes = STRICT_INTERVAL_PACKET_COUNT * mps;

    // Let the strict endpoint's task queue its first report before the initial poll.
    vTaskDelay(pdMS_TO_TICKS(20));

    // Create stress task
    ulTaskNotifyTake(pdTRUE, 0); // Clear any stale task notification before using it for stress completion.
    const uint32_t stress_delay_us = (port_speed == USB_SPEED_HIGH) ? UFRAME_DURATION_US : FRAME_DURATION_US;
    s_strict_sof_test_task = xTaskGetCurrentTaskHandle();
    const UBaseType_t test_task_priority = uxTaskPriorityGet(NULL);
    const UBaseType_t stress_task_priority = (test_task_priority < configMAX_PRIORITIES - 1)
                                             ? test_task_priority + 1
                                             : test_task_priority;
    TaskHandle_t stress_task;
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreatePinnedToCore(strict_sof_stress_task, "strict_sof_stress", 2048,
                                                      (void *)(uintptr_t)stress_delay_us, stress_task_priority, &stress_task, xPortGetCoreID()));

    // Check the scheduling
    const uint32_t start_hfnum = usb_dwc_ll_hfnum_get_frame_num(USB_DWC_LL_GET_HW(TEST_PORT_NUM));
    TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(intr_pipe, urb));
    xTaskNotifyGive(stress_task);
    TEST_HCD_EXPECT_PIPE_EVENT(intr_pipe, HCD_PIPE_EVENT_URB_DONE);
    TEST_ASSERT_NOT_EQUAL(0, ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10000)));
    s_strict_sof_test_task = NULL;
    const uint32_t completion_hfnum = usb_dwc_ll_hfnum_get_frame_num(USB_DWC_LL_GET_HW(TEST_PORT_NUM));
    const uint32_t hfnum_delta = (completion_hfnum - start_hfnum) & (USB_DWC_LL_HFNUM_FRAME_COUNT - 1);

    ESP_LOGI(TAG, "HFNUM start: %d, HFNUM stop: %d, HFNUM delta: %d packets serviced: %d", start_hfnum, completion_hfnum, hfnum_delta, STRICT_INTERVAL_PACKET_COUNT);
    TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(STRICT_INTERVAL_MAX_HFNUM_DELTA, hfnum_delta,
                                      "bInterval 1 packets were not serviced every USB (micro)frame");
    TEST_ASSERT_EQUAL_PTR(urb, hcd_urb_dequeue(intr_pipe));
    TEST_HCD_EXPECT_TRANSFER_STATUS(urb, USB_TRANSFER_STATUS_COMPLETED);
    TEST_ASSERT_EQUAL(STRICT_INTERVAL_PACKET_COUNT * mps, urb->transfer.actual_num_bytes);

    test_hcd_free_urb(urb);
    test_hcd_pipe_free(intr_pipe);
    test_hcd_pipe_free(default_pipe);
    test_hcd_wait_for_disconn(port_hdl, false);
}

/*
Test HCD interrupt pipe IN data overrun (Buffer DMA only)
Purpose:
    - A device returning a larger payload than expected must fail the transfer (USB 2.0
      §5.7.3): expect HCD_PIPE_EVENT_ERROR_OVERFLOW, a halted pipe and
      USB_TRANSFER_STATUS_OVERFLOW, then flush+clear
    - Interrupt IN transfers are armed for exactly MPS (check_xfer_size() only accepts
      non-zero multiples of MPS), so a surplus payload is caught by the core as a packet
      babble error. In Scatter/Gather DMA it surfaces as a qTD babble error instead

The mock device's 5-byte-report endpoint (interface 0, MPS 8) is used with the MPS
overridden to 4: its 5-byte report then exceeds the channel's MPS (packet babble).
*/
TEST_CASE("Test HCD interrupt pipe IN data overrun", "[intr][tinyusb_mock]")
{
    usb_speed_t port_speed = test_hcd_wait_for_conn(port_hdl);  // Trigger a connection
    vTaskDelay(pdMS_TO_TICKS(100)); // Short delay send of SOF (for FS) or EOPs (for LS)

    // Allocate default pipe and enumerate the device
    hcd_pipe_handle_t default_pipe = test_hcd_pipe_alloc(port_hdl, NULL, 0, port_speed); // Create a default pipe (using a NULL EP descriptor)
    uint8_t dev_addr = test_hcd_enum_device(default_pipe);

    // Copy the mock device's 5-byte-report endpoint (interface 0, MPS 8) with the MPS overridden to 4:
    // its 5-byte report then exceeds the channel's MPS - a packet babble (data overrun)
    const usb_ep_desc_t *in_ep_desc = dev_hid_get_in_ep_desc(port_speed, DEV_HID_ITF_R5_MPS8);
    usb_ep_desc_t overrun_ep_desc = *in_ep_desc;
    overrun_ep_desc.wMaxPacketSize = 4;
    const int mps = USB_EP_DESC_GET_MPS(&overrun_ep_desc);
    hcd_pipe_handle_t intr_pipe = test_hcd_pipe_alloc(port_hdl, &overrun_ep_desc, dev_addr, port_speed);

    // Allocate a URB with buffer room for the whole report, but program only one MPS packet
    urb_t *urb = test_hcd_alloc_urb(0, USB_EP_DESC_GET_MPS(in_ep_desc));
    urb->transfer.num_bytes = mps;
    urb->transfer.context = URB_CONTEXT_VAL;

    TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(intr_pipe, urb));
    TEST_HCD_EXPECT_PIPE_EVENT(intr_pipe, HCD_PIPE_EVENT_ERROR_OVERFLOW);
    TEST_HCD_EXPECT_PIPE_STATE(intr_pipe, HCD_PIPE_STATE_HALTED);

    // The failed URB is retired with an overflow status and no byte count
    TEST_ASSERT_EQUAL_PTR(urb, hcd_urb_dequeue(intr_pipe));
    TEST_HCD_EXPECT_TRANSFER_STATUS(urb, USB_TRANSFER_STATUS_OVERFLOW);
    TEST_ASSERT_EQUAL(0, urb->transfer.actual_num_bytes);

    // Return the pipe to the active state
    TEST_ASSERT_EQUAL(ESP_OK, hcd_pipe_command(intr_pipe, HCD_PIPE_CMD_FLUSH));
    TEST_ASSERT_EQUAL(ESP_OK, hcd_pipe_command(intr_pipe, HCD_PIPE_CMD_CLEAR));
    TEST_HCD_EXPECT_PIPE_STATE(intr_pipe, HCD_PIPE_STATE_ACTIVE);

    test_hcd_free_urb(urb);
    test_hcd_pipe_free(intr_pipe);
    test_hcd_pipe_free(default_pipe);
    // Cleanup
    test_hcd_wait_for_disconn(port_hdl, false);
}

#endif // CONFIG_USB_HOST_DMA_MODE_BUFFER

/*
Test HCD interrupt pipe allocation, when bInterval = 0
Purpose:
    - Test that an interrupt pipe cause no panic

Procedure:
    - Setup HCD and wait for connection
    - Allocate interrupt pipe with bInterval = 0
    - Expect ESP_ERR_NOT_SUPPORTED
    - Cleanup
*Note: for all speeds (LS, FS, HS)
*/
TEST_CASE("Test HCD interrupt pipe alloc: bInterval=0", "[intr][low_speed][full_speed][high_speed]")
{
    // Trigger a connection
    test_hcd_wait_for_conn(port_hdl);
    // The device speed not needed, we need just the connection to unlock pipe allocation
    vTaskDelay(pdMS_TO_TICKS(100)); // Short delay send of SOF (for FS) or EOPs (for LS)

    usb_ep_desc_t test_ep_intr_desc = {
        .bLength = USB_EP_DESC_SIZE,
        .bDescriptorType = USB_B_DESCRIPTOR_TYPE_ENDPOINT,
        .bEndpointAddress = 0x81, // IN endpoint
        .bmAttributes = USB_BM_ATTRIBUTES_XFER_INT,
        .wMaxPacketSize = 8,
        .bInterval = 0, // bInterval = 0
    };

    hcd_pipe_config_t pipe_config = {
        .callback = NULL,               // No callback
        .callback_arg = NULL,           // No callback argument
        .context = NULL,                // No context
        .ep_desc = &test_ep_intr_desc,  // IN endpoint descriptor
        .dev_addr = 1,                  // Any device address
    };

    for (usb_speed_t test_speed = USB_SPEED_LOW; test_speed <= USB_SPEED_HIGH; test_speed++) {
        // Create an interrupt pipe (using a EP descriptor)
        hcd_pipe_handle_t pipe_hdl = NULL;
        pipe_config.dev_speed = test_speed;
        TEST_ASSERT_EQUAL_MESSAGE(ESP_ERR_NOT_SUPPORTED, hcd_pipe_alloc(port_hdl, &pipe_config, &pipe_hdl), "Create pipe with bInterval=0 should fail");
        TEST_ASSERT_NULL_MESSAGE(pipe_hdl, "Pipe handle should be NULL");
    }
    // Cleanup
    test_hcd_wait_for_disconn(port_hdl, false);
}

/*
Test HCD interrupt pipe allocation, when bInterval in the interval [0x01..0xFF]
Purpose:
    - Test that an interrupt pipe can be created with bInterval in the interval [0x01..0xFF]
    - Test that an interrupt pipe creation cause no panic with any bInterval

Procedure:
    - Setup HCD and wait for connection
    - Allocate interrupt pipe with bInterval in the interval [0x01..0xFF]
    - Expect ESP_OK
    - Cleanup
*Note: for all speeds (LS, FS, HS)
*/
TEST_CASE("Test HCD interrupt pipe alloc: bInterval [0x1..0xff]", "[intr][low_speed][full_speed][high_speed]")
{
    // Trigger a connection
    test_hcd_wait_for_conn(port_hdl);
    // The device speed not needed, we need just the connection to unlock pipe allocation
    vTaskDelay(pdMS_TO_TICKS(100)); // Short delay send of SOF (for FS) or EOPs (for LS)

    for (int test_bInterval = 1; test_bInterval <= 0xFF; test_bInterval++) {
        usb_ep_desc_t test_ep_intr_desc = {
            .bLength = USB_EP_DESC_SIZE,
            .bDescriptorType = USB_B_DESCRIPTOR_TYPE_ENDPOINT,
            .bEndpointAddress = 0x81, // IN endpoint
            .bmAttributes = USB_BM_ATTRIBUTES_XFER_INT,
            .wMaxPacketSize = 8,
            .bInterval = test_bInterval,
        };

        hcd_pipe_config_t pipe_config = {
            .callback = NULL,               // No callback
            .callback_arg = NULL,           // No callback argument
            .context = NULL,                // No context
            .ep_desc = &test_ep_intr_desc,  // IN endpoint descriptor
            .dev_addr = 1,                  // Any device address
        };

        for (usb_speed_t test_speed = USB_SPEED_LOW; test_speed <= USB_SPEED_HIGH; test_speed++) {
            // Create an interrupt pipe (using a EP descriptor)
            hcd_pipe_handle_t pipe_hdl = NULL;
            pipe_config.dev_speed = test_speed;
            TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, hcd_pipe_alloc(port_hdl, &pipe_config, &pipe_hdl), "Create pipe should not fail");
            TEST_ASSERT_NOT_NULL_MESSAGE(pipe_hdl, "Pipe handle should not be NULL");
            TEST_ASSERT_EQUAL(ESP_OK, hcd_pipe_free(pipe_hdl));
        }
    }
    // Cleanup
    test_hcd_wait_for_disconn(port_hdl, false);
}
