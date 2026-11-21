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
#include "dev_hid.h"
#include "hcd_common.h"
#include "mock_hid.h"
#include "hal/usb_dwc_ll.h"     // To be able to read PID of the host channel

static const char *TAG = "INTR";

#define MULTI_PKT_NUM_PKTS      3       // Packets per multi-packet URB (shared by the IN and OUT multi-packet tests)
#define URB_DONE_ITERS          20      // Number of URB completions (URB_DONE events) per URB in a test case
#define HID_REPORT_TYPE_OUTPUT  0x02

/**
 * @brief Set the mock HID trigger in a SET_REPORT payload
 *
 * @param[in] urb Control URB containing the SET_REPORT request
 * @param[in] trigger Mock HID trigger value
 */
static void set_mock_device_report_trigger(urb_t *urb, uint8_t trigger)
{
    urb->transfer.data_buffer[sizeof(usb_setup_packet_t)] = trigger;
}

/**
 * @brief Send a HID SET_REPORT request selecting the mock device mode
 *
 * The payload carries MOCK_HID_SET_REPORT_MODE_TRIGGER and the mode value; the mock device
 * (mock_hid.c) switches to the selected mode. The mode falls back to
 * MOCK_HID_DEVICE_MODE_MOUSE_REPORT when the device is unmounted.
 *
 * @param[in] default_pipe Default pipe to send the request on
 * @param[in] urb Control URB to use for the request
 * @param[in] hid_info Mock HID interface the request is addressed to
 * @param[in] mode Device mode to select
 */
static void test_hcd_hid_set_device_mode(hcd_pipe_handle_t default_pipe, urb_t *urb, const dev_hid_info_t *hid_info, mock_hid_device_mode_t mode)
{
    usb_setup_packet_t *setup = (usb_setup_packet_t *)urb->transfer.data_buffer;
    USB_SETUP_PACKET_INIT_HID_SET_REPORT(setup, HID_REPORT_TYPE_OUTPUT, 0, hid_info->bInterfaceNumber, hid_info->report_len);
    // First payload byte is the mode trigger, second selects the mode (see mock_hid.h)
    set_mock_device_report_trigger(urb, MOCK_HID_SET_REPORT_MODE_TRIGGER);
    urb->transfer.data_buffer[sizeof(usb_setup_packet_t) + 1] = mode;
    urb->transfer.num_bytes = sizeof(usb_setup_packet_t) + setup->wLength;
    urb->transfer.context = URB_CONTEXT_VAL;

    TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(default_pipe, urb));
    TEST_HCD_EXPECT_PIPE_EVENT(default_pipe, HCD_PIPE_EVENT_URB_DONE);
    TEST_ASSERT_EQUAL_PTR(urb, hcd_urb_dequeue(default_pipe));
    TEST_HCD_EXPECT_TRANSFER_STATUS(urb, USB_TRANSFER_STATUS_COMPLETED);
}

// --------------------------------------------------- Test Cases ------------------------------------------------------

// ------------------------------------------- Interrupt packets scheduling -------------------------------------------

/*
Test HCD interrupt pipe IN URBs
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
Test HCD interrupt pipe multi-packet IN URBs
Purpose:
    - Test that an interrupt URB spanning multiple MPS-sized packets completes correctly

The test device is a composite HID mouse (see dev_hid.h); each interface is tested one-by-one
with a URB of MULTI_PKT_NUM_PKTS * MPS bytes:
    - report == MPS: no short packet, the URB completes once the buffer is full
      (actual_num_bytes == num_bytes). Interface 3 does this with a non-DWORD MPS (5):
      per-packet DMA re-arms at unaligned offsets, which host scatter-gather DMA supports
    - report < MPS: the first packet is a short packet, the transfer finishes early
      (actual_num_bytes == report length)
*/

#define MULTI_PKT_ITERS     URB_DONE_ITERS

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
#define OUT_ECHO_MAX_ITERS      URB_DONE_ITERS      // Repetitions: the data toggle must persist across URBs

/**
 * @brief Run the interrupt OUT loopback test
 *
 * Sends an OUT report of out_full_pkts full MPS packets plus a final short packet on the mock
 * device's interrupt OUT endpoint (interface 0), and verifies that the looped-back echo arrives
 * on its interrupt IN endpoint (see the test cases below)
 *
 * @param[in] out_full_pkts Number of full MPS packets in the OUT report payload
 * @param[in] out_short_len Short final packet length (< MPS): terminates the device-side OUT transfer
 */
static void test_hcd_intr_out_loopback(int out_full_pkts, int out_short_len)
{
    usb_speed_t port_speed = test_hcd_wait_for_conn(port_hdl);  // Trigger a connection
    vTaskDelay(pdMS_TO_TICKS(100)); // Short delay send of SOF (for FS) or EOPs (for LS)

    hcd_pipe_handle_t default_pipe = test_hcd_pipe_alloc(port_hdl, NULL, 0, port_speed); // Create a default pipe (using a NULL EP descriptor)
    uint8_t dev_addr = test_hcd_enum_device(default_pipe);

    // Mute mouse reports on the loopback interface: only echoes are sent on its IN endpoint
    const dev_hid_info_t *hid_info = dev_hid_get_info(DEV_HID_ITF_R5_MPS8);
    urb_t *ctrl_urb = test_hcd_alloc_urb(0, sizeof(usb_setup_packet_t) + hid_info->report_len);
    test_hcd_hid_set_device_mode(default_pipe, ctrl_urb, hid_info, MOCK_HID_DEVICE_MODE_LOOPBACK_ONLY);

    // Interface 0: interrupt OUT endpoint 0x01, its echo is sent on interrupt IN endpoint 0x81
    const usb_ep_desc_t *out_ep_desc = dev_hid_get_out_ep_desc(port_speed);
    const usb_ep_desc_t *in_ep_desc = dev_hid_get_in_ep_desc(port_speed, DEV_HID_ITF_R5_MPS8);
    const int mps = USB_EP_DESC_GET_MPS(out_ep_desc);
    TEST_ASSERT_EQUAL(mps, USB_EP_DESC_GET_MPS(in_ep_desc));    // The echo fits the IN endpoint MPS
    hcd_pipe_handle_t out_pipe = test_hcd_pipe_alloc(port_hdl, out_ep_desc, dev_addr, port_speed);
    hcd_pipe_handle_t in_pipe = test_hcd_pipe_alloc(port_hdl, in_ep_desc, dev_addr, port_speed);

    // OUT URB: full packets + a short final packet
    const int out_num_bytes = out_full_pkts * mps + out_short_len;
    urb_t *out_urb = test_hcd_alloc_urb(0, out_num_bytes);
    out_urb->transfer.num_bytes = out_num_bytes;
    out_urb->transfer.context = URB_CONTEXT_VAL;
    for (int i = 0; i < out_num_bytes; i++) {
        out_urb->transfer.data_buffer[i] = 0x11 * (i + 1);      // First byte is not a trigger value
    }
    // IN URB: a non-zero multiple of MPS (HCD enqueue requirement), completed early by a short packet
    urb_t *in_urb = test_hcd_alloc_urb(0, (out_full_pkts + 1) * mps);
    in_urb->transfer.num_bytes = (out_full_pkts + 1) * mps;
    in_urb->transfer.context = URB_CONTEXT_VAL;

    for (int iter_count = OUT_ECHO_MAX_ITERS; iter_count > 0; iter_count--) {
        // Enqueue the IN URB first, so a poll is pending when the device queues the echo
        TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(in_pipe, in_urb));
        TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(out_pipe, out_urb));

        // The OUT transfer completes once its last packet is ACKed
        TEST_HCD_EXPECT_PIPE_EVENT(out_pipe, HCD_PIPE_EVENT_URB_DONE);
        TEST_ASSERT_EQUAL_PTR(out_urb, hcd_urb_dequeue(out_pipe));
        TEST_HCD_EXPECT_TRANSFER_STATUS(out_urb, USB_TRANSFER_STATUS_COMPLETED);
        TEST_ASSERT_EQUAL(URB_CONTEXT_VAL, out_urb->transfer.context);
        TEST_ASSERT_EQUAL(out_num_bytes, out_urb->transfer.actual_num_bytes);

        // Mouse reports are muted: the first IN completion is the looped-back report
        TEST_HCD_EXPECT_PIPE_EVENT(in_pipe, HCD_PIPE_EVENT_URB_DONE);
        TEST_ASSERT_EQUAL_PTR(in_urb, hcd_urb_dequeue(in_pipe));
        TEST_HCD_EXPECT_TRANSFER_STATUS(in_urb, USB_TRANSFER_STATUS_COMPLETED);
        ESP_LOGI(TAG, "Echo: %d bytes", in_urb->transfer.actual_num_bytes);
        ESP_LOG_BUFFER_HEXDUMP(TAG, in_urb->transfer.data_buffer, in_urb->transfer.actual_num_bytes, ESP_LOG_INFO);
        // Verify the looped-back payload byte-for-byte
        TEST_ASSERT_EQUAL(out_num_bytes, in_urb->transfer.actual_num_bytes);
        TEST_ASSERT_EQUAL_UINT8_ARRAY(out_urb->transfer.data_buffer, in_urb->transfer.data_buffer, out_num_bytes);
    }

    // Free URBs and pipes
    test_hcd_free_urb(ctrl_urb);
    test_hcd_free_urb(in_urb);
    test_hcd_free_urb(out_urb);
    test_hcd_pipe_free(in_pipe);
    test_hcd_pipe_free(out_pipe);
    test_hcd_pipe_free(default_pipe);
    // Cleanup
    test_hcd_wait_for_disconn(port_hdl, false);
}

/*
Test HCD interrupt pipe OUT URBs (device loopback)
Purpose:
    - Test that a single-packet interrupt OUT URB completes on the bus
    - Verify the transfer end-to-end: the mock device echoes the received OUT report on its
      interrupt IN endpoint (interface 0 loopback); the host compares the payload

Procedure:
    - Setup HCD and enumerate the mock device
    - Select the LOOPBACK_ONLY device mode: the loopback interface sends no mouse reports,
      so the first IN completion after the OUT transfer is the echo (see mock_hid.h)
    - Allocate interrupt OUT (EP 0x01) and IN (EP 0x81) pipes on interface 0
    - Enqueue an IN URB (pending poll for the echo), then the OUT URB; expect it to complete
    - Expect the IN URB to complete with the echo, compare the payload byte-for-byte
    - Requeue both URBs, repeat
    - Teardown

Note: the payload is a short packet (< MPS), which terminates the device-side OUT transfer.
Its first byte must differ from the SET_REPORT trigger values (the mock cannot tell OUT
endpoint data apart from a control SET_REPORT).
*/
TEST_CASE("Test HCD interrupt pipe OUT URBs (loopback)", "[intr][tinyusb_mock]")
{
    test_hcd_intr_out_loopback(0, OUT_URB_PAYLOAD_LEN);
}

#define OUT_MULTI_PKT_SHORT_LEN         5                   // Short final packet: completes the device-side OUT transfer

// CI runner HW setup failure on esp32p4 ECO4. Disabling the test temporarily
#if !CONFIG_ESP32P4_SELECTS_REV_LESS_V3
/*
Test HCD interrupt pipe multi-packet OUT URBs (loopback)
Purpose:
    - Test that an interrupt OUT URB spanning multiple packets completes on the bus
    - Verify the transfer end-to-end: the mock device echoes the received OUT report on its
      interrupt IN endpoint (interface 0 loopback); the host compares the payload

Procedure:
    - Setup HCD and enumerate the mock device
    - Select the LOOPBACK_ONLY device mode: the loopback interface sends no mouse reports,
      so the first IN completion after the OUT transfer is the echo (see mock_hid.h)
    - Allocate interrupt OUT (EP 0x01) and IN (EP 0x81) pipes on interface 0
    - Enqueue an IN URB (pending poll for the echo), then the multi-packet OUT URB; expect it to complete
    - Expect the IN URB to complete with the echo, compare the payload byte-for-byte
    - Requeue both URBs, repeat
    - Teardown

Note: the payload ends with a short packet (< MPS), which terminates the device-side OUT transfer;
the mock's HID EP buffer (default CFG_TUD_HID_EP_BUFSIZE of 64) exceeds MOCK_HID_LOOPBACK_MAX_LEN.
Its first byte must differ from the SET_REPORT trigger values (the mock cannot tell OUT
endpoint data apart from a control SET_REPORT).
*/
TEST_CASE("Test HCD interrupt pipe multi-packet OUT URBs", "[intr][tinyusb_mock]")
{
    test_hcd_intr_out_loopback(MULTI_PKT_NUM_PKTS - 1, OUT_MULTI_PKT_SHORT_LEN);
}

#endif // CONFIG_ESP32P4_SELECTS_REV_LESS_V3

// ------------------------------------------- Data toggle resync handling -------------------------------------------

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
    set_mock_device_report_trigger(urb, MOCK_HID_SET_REPORT_STALL_TRIGGER);
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
    - Verify that valid interrupt bInterval values allocate successfully:
      - Full-Speed: 1..255
      - Low-Speed: 1..255 (USB 2.0 Specification Table 9-13 defines minimum as 10, but Linux host allows also lower than 10)
      - High-Speed: 1..16
    - Verify that USB 2.0-disallowed interrupt bInterval values are rejected:
      - High-Speed: 17..255

Procedure:
    - Setup HCD and wait for connection
    - Attempt interrupt pipe allocation for each bInterval in the interval [0x01..0xFF]
    - Expect ESP_OK only for the range valid at the selected device speed
    - Expect ESP_ERR_NOT_SUPPORTED and a NULL pipe handle for disallowed values
    - Cleanup
*Note: USB 2.0 Specification Table 9-13 defines these speed-specific bInterval ranges.
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

            const bool invalid_hs_config = (test_speed == USB_SPEED_HIGH) && test_bInterval > 16;

            if (invalid_hs_config) {
                TEST_ASSERT_EQUAL_MESSAGE(ESP_ERR_NOT_SUPPORTED, hcd_pipe_alloc(port_hdl, &pipe_config, &pipe_hdl), "Create pipe should fail");
                TEST_ASSERT_NULL_MESSAGE(pipe_hdl, "Pipe handle should be NULL");
            } else {
                TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, hcd_pipe_alloc(port_hdl, &pipe_config, &pipe_hdl), "Create pipe should not fail");
                TEST_ASSERT_NOT_NULL_MESSAGE(pipe_hdl, "Pipe handle should not be NULL");
                TEST_ASSERT_EQUAL(ESP_OK, hcd_pipe_free(pipe_hdl));
            }
        }
    }
    // Cleanup
    test_hcd_wait_for_disconn(port_hdl, false);
}
