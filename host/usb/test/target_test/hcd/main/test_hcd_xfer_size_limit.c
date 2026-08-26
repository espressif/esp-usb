/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"
#include "esp_log.h"
#include "mock_msc.h"
#include "dev_msc.h"
#include "hcd_common.h"

static const char *TAG = "XFER_SIZE_LIMIT";

/*
This test validates the HCD's per-transfer size-limit enforcement for control transfers. The limit is the
maximum number of bytes the controller can move in a single control transfer for the active DMA mode, reported by
hcd_pipe_get_xfer_size_limit():
    - Buffer DMA:     min(HCTSIZ.XferSize width, MPS * HCTSIZ.PktCnt width), floored to a whole number of MPS packets
                      (databook Section 5.4.41).
    - Scatter/Gather: the 17-bit non-isochronous qTD "Total bytes to transfer" field (programming guide Section 6).
The same test source runs against both DMA-mode builds (default = Scatter/Gather, buffer_dma = Buffer DMA), and it reads
the limit at runtime, so it is agnostic to mode, port speed and hardware configuration.

Control transfer is exercised with a real GET_CONFIGURATION_DESCRIPTOR whose data stage is sized to L - MPS, L and L + MPS; the
first two MUST be accepted (and complete via the device's short descriptor packet), and the last MUST be rejected.
*/

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

/**
 * @brief Assert the HCD rejects an over-limit transfer at enqueue time
 *
 * The rejection happens before the data buffer is touched or the channel is activated, so a tiny buffer with an
 * over-sized num_bytes is sufficient (and avoids a needless large allocation).
 *
 * @param pipe       Pipe to enqueue on
 * @param num_bytes  Total transfer num_bytes (for control this includes the 8-byte setup packet)
 */
static void expect_reject(hcd_pipe_handle_t pipe, int num_bytes)
{
    urb_t *urb = test_hcd_alloc_urb(0, sizeof(usb_setup_packet_t));
    urb->transfer.num_bytes = num_bytes;
    ESP_LOGI(TAG, "Expecting reject: num_bytes=%d", num_bytes);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, hcd_urb_enqueue(pipe, urb));
    test_hcd_free_urb(urb);
}

/**
 * @brief Assert the HCD accepts an at/under-limit CONTROL transfer and completes it
 *
 * We use a standard GET_CONFIGURATION_DESCRIPTOR request whose data stage reserves 'data_len' bytes (== the tested
 * limit). The device returns its (short) configuration descriptor, which terminates the IN data stage with a short
 * packet, so the transfer completes on its own. This exercises the HAL programming a limit-sized IN buffer (reserved
 * size == data_len) without relying on a manual cancel.
 *
 * @param default_pipe  Control (default) pipe
 * @param data_len      Control data-stage length in bytes; must be <= the pipe's size limit
 */
static void expect_accept_ctrl(hcd_pipe_handle_t default_pipe, int data_len)
{
    const int num_bytes = (int)sizeof(usb_setup_packet_t) + data_len;
    urb_t *urb = test_hcd_alloc_urb(0, num_bytes);
    urb->transfer.num_bytes = num_bytes;
    // wLength is a 16-bit field; it may be smaller than data_len for very large limits. That is fine: the HAL still
    // reserves the full data_len IN buffer, and the device terminates the data stage with a short packet.
    const uint16_t wLength = (uint16_t)((data_len > UINT16_MAX) ? UINT16_MAX : data_len);
    USB_SETUP_PACKET_INIT_GET_CONFIG_DESC((usb_setup_packet_t *)urb->transfer.data_buffer, 0, wLength);
    ESP_LOGI(TAG, "Expecting accept (ctrl): data_len=%d (num_bytes=%d, wLength=%u)", data_len, num_bytes, wLength);
    TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(default_pipe, urb));
    TEST_HCD_EXPECT_PIPE_EVENT(default_pipe, HCD_PIPE_EVENT_URB_DONE);
    TEST_ASSERT_EQUAL_PTR(urb, hcd_urb_dequeue(default_pipe));
    TEST_HCD_EXPECT_TRANSFER_STATUS(urb, USB_TRANSFER_STATUS_COMPLETED);
    // The device returns at least the setup + a descriptor header, and never more than we reserved
    TEST_ASSERT_GREATER_OR_EQUAL(sizeof(usb_setup_packet_t), urb->transfer.actual_num_bytes);
    TEST_ASSERT_LESS_OR_EQUAL(num_bytes, urb->transfer.actual_num_bytes);
    test_hcd_free_urb(urb);
}

// -----------------------------------------------------------------------------
// Test Cases
// -----------------------------------------------------------------------------

/*
Test HCD control transfer size limit

Purpose:
    - Verify hcd_pipe_get_xfer_size_limit() reports a sane, MPS-aligned per-transfer byte limit for the control pipe
    - Verify the HCD accepts control transfers whose data stage is at and just under the limit (and completes them)
    - Verify the HCD rejects control transfers whose data stage is over the limit with ESP_ERR_INVALID_SIZE
    - The control size check applies to the data stage only (the 8-byte setup packet is excluded)
    - Runs for both DMA modes via separate build configs

Note:
    In some configurations the control limit exceeds the largest data stage a device could ever return, but the HCD
    check operates on num_bytes only (it does not depend on the setup packet's 16-bit wLength), so an over-limit data
    stage is always constructible and always rejected.

Procedure:
    - Setup HCD, connect, enumerate the MSC device, use the control (default) pipe
    - Query the size limit L and MPS M
    - Enqueue control transfers with data-stage lengths L-M, L (accepted) and L+M (rejected)
    - Teardown
*/
TEST_CASE("Test HCD control transfer size limit", "[ctrl][full_speed][high_speed]")
{
    usb_speed_t port_speed = test_hcd_wait_for_conn(port_hdl);
    vTaskDelay(pdMS_TO_TICKS(100));

    hcd_pipe_handle_t default_pipe = test_hcd_pipe_alloc(port_hdl, NULL, 0, port_speed);
    (void)test_hcd_enum_device(default_pipe);

    const int mps = hcd_pipe_get_mps(default_pipe);
    const int limit = (int)hcd_pipe_get_xfer_size_limit(default_pipe);
    ESP_LOGI(TAG, "CTRL: mps=%d limit=%d", mps, limit);
    TEST_ASSERT_GREATER_THAN(0, limit);
    TEST_ASSERT_EQUAL_MESSAGE(0, limit % mps, "Control size limit is not an integer multiple of MPS");
    TEST_ASSERT_GREATER_OR_EQUAL(mps, limit);

    // Data stage nearly the limit and exactly the limit -> accepted (and completes via a short IN packet)
    expect_accept_ctrl(default_pipe, limit - mps);
    expect_accept_ctrl(default_pipe, limit);
    // Data stage over the limit -> rejected (num_bytes = setup + data_len, data_len = limit + mps)
    expect_reject(default_pipe, (int)sizeof(usb_setup_packet_t) + limit + mps);

    test_hcd_pipe_free(default_pipe);
    test_hcd_wait_for_disconn(port_hdl, false);
}
