/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "unity.h"
#include "esp_log.h"
#include "dev_hid.h"
#include "hcd_common.h"
#include "mock_hid.h"
#include "hal/usb_dwc_ll.h"     // To be able to read PID of the host channel

static const char *TAG = "INTR";

#define URB_DONE_ITERS      50      // Number of URB completions (URB_DONE events) per URB in a test case
// --------------------------------------------------- Test Cases ------------------------------------------------------

/*
Test HCD interrupt pipe URBs
Purpose:
    - Test that an interrupt pipe can be created
    - URBs can be created and enqueued to the interrupt pipe
    - Interrupt pipe returns HCD_PIPE_EVENT_URB_DONE
    - Test that URBs can be aborted when enqueued

Procedure:
    - Setup HCD and wait for connection
    - Allocate default pipe and enumerate the device
    - Setup interrupt pipe and allocate URBs
    - Enqueue URBs, expect HCD_PIPE_EVENT_URB_DONE, and requeue
    - Stop after fixed number of iterations
    - Deallocate URBs
    - Teardown

Note: Some mice will NAK until it is moved, so try moving the mouse around if this test case gets stuck.
*/

#define NUM_URBS                            3
#define NUM_URB_ITERS                       (NUM_URBS * URB_DONE_ITERS)

TEST_CASE("Test HCD interrupt pipe URBs", "[intr][tinyusb_mock]")
{
    usb_speed_t port_speed = test_hcd_wait_for_conn(port_hdl);  // Trigger a connection
    vTaskDelay(pdMS_TO_TICKS(100)); // Short delay send of SOF (for FS) or EOPs (for LS)

    hcd_pipe_handle_t default_pipe = test_hcd_pipe_alloc(port_hdl, NULL, 0, port_speed); // Create a default pipe (using a NULL EP descriptor)
    uint8_t dev_addr = test_hcd_enum_device(default_pipe);

    // Allocate interrupt pipe and URBS
    const dev_hid_info_t *hid_info = dev_hid_get_info();
    const usb_ep_desc_t *in_ep_desc = dev_hid_get_in_ep_desc(port_speed);
    const int data_buff_size = USB_EP_DESC_GET_MPS(in_ep_desc);
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
    int iter_count = NUM_URB_ITERS;
    for (iter_count = NUM_URB_ITERS; iter_count > 0; iter_count--) {
        // Wait for an URB to be done
        TEST_HCD_EXPECT_PIPE_EVENT(intr_pipe, HCD_PIPE_EVENT_URB_DONE);
        // Dequeue the URB and check results
        urb_t *urb = hcd_urb_dequeue(intr_pipe);
        TEST_HCD_EXPECT_TRANSFER_STATUS(urb, USB_TRANSFER_STATUS_COMPLETED);
        TEST_ASSERT_EQUAL(URB_CONTEXT_VAL, urb->transfer.context);
        // Each report is a single packet: full packet when report == MPS, short packet (early termination) otherwise
        const int expected_num_bytes = hid_info->report_len;
        ESP_LOGI(TAG, "Actual num bytes: %d (expected %d)", urb->transfer.actual_num_bytes, expected_num_bytes);
        TEST_ASSERT_EQUAL(expected_num_bytes, urb->transfer.actual_num_bytes);
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
    test_hcd_pipe_free(default_pipe);
    // Cleanup
    test_hcd_wait_for_disconn(port_hdl, false);
}

/*
Test HCD interrupt pipe multi-packet URBs
Purpose:
    - Test that an interrupt URB spanning multiple MPS-sized packets completes correctly

Procedure:
    - Setup HCD and wait for connection
    - Allocate default pipe and enumerate the device
    - Setup interrupt pipe and allocate a URB of MULTI_PKT_NUM_PKTS * MPS bytes
    - Enqueue URB, expect HCD_PIPE_EVENT_URB_DONE, check actual_num_bytes, and requeue
    - Stop after fixed number of iterations
    - Deallocate URB
    - Teardown

Note: Some mice will NAK until it is moved, so try moving the mouse around if this test case gets stuck.
Note: The mock device's report (5 bytes) is smaller than the EP MPS (8), so the transfer always
terminates early after the first packet. The buffer-full completion path (report == MPS) is kept
in the expectations for completeness, but is not exercised by this mock device.
*/

#define MULTI_PKT_NUM_PKTS  3
#define MULTI_PKT_ITERS     URB_DONE_ITERS

TEST_CASE("Test HCD interrupt pipe multi-packet URBs", "[intr][tinyusb_mock]")
{
    usb_speed_t port_speed = test_hcd_wait_for_conn(port_hdl);  // Trigger a connection
    vTaskDelay(pdMS_TO_TICKS(100)); // Short delay send of SOF (for FS) or EOPs (for LS)

    hcd_pipe_handle_t default_pipe = test_hcd_pipe_alloc(port_hdl, NULL, 0, port_speed); // Create a default pipe (using a NULL EP descriptor)
    uint8_t dev_addr = test_hcd_enum_device(default_pipe);

    // Allocate interrupt pipe and a multi-packet URB
    const dev_hid_info_t *hid_info = dev_hid_get_info();
    const usb_ep_desc_t *in_ep_desc = dev_hid_get_in_ep_desc(port_speed);
    const int mps = USB_EP_DESC_GET_MPS(in_ep_desc);
    const bool fills_buffer = (hid_info->report_len == mps);
    if (fills_buffer) {
        // Per-packet DMA activations only stay DWORD aligned if the MPS itself is DWORD aligned
        TEST_ASSERT_EQUAL_MESSAGE(0, mps % sizeof(uint32_t), "Multi-packet fill requires DWORD aligned MPS");
    }
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
            ESP_LOGI(TAG, "pkt %d: X mov %d, Y mov %d", pkt, report[1], report[2]);
        }
    }

    // Free URB and pipes
    test_hcd_free_urb(urb);
    test_hcd_pipe_free(intr_pipe);
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
    const dev_hid_info_t *hid_info = dev_hid_get_info();
    const usb_ep_desc_t *in_ep_desc = dev_hid_get_in_ep_desc(port_speed);
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
    const dev_hid_info_t *hid_info = dev_hid_get_info();
    const usb_ep_desc_t *in_ep_desc = dev_hid_get_in_ep_desc(port_speed);
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
