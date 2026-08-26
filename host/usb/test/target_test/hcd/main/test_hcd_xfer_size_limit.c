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
These tests validate the HCD's per-transfer size-limit enforcement for bulk and control transfers. The limit is the
maximum number of bytes the controller can move in a single bulk/control transfer for the active DMA mode, reported by
hcd_pipe_get_xfer_size_limit():
    - Buffer DMA:     min(HCTSIZ.XferSize width, MPS * HCTSIZ.PktCnt width), floored to a whole number of MPS packets
                      (databook Section 5.4.41).
    - Scatter/Gather: the 17-bit non-isochronous qTD "Total bytes to transfer" field (programming guide Section 6).
The same test source runs against both DMA-mode builds (default = Scatter/Gather, buffer_dma = Buffer DMA), and it reads
the limit at runtime, so it is agnostic to mode, port speed and hardware configuration.

Bulk is exercised with a real SCSI WRITE(10)+READ(10) transaction against the mass-storage device, with the data phase
sized to the largest whole number of sectors that fits the limit L (i.e. as close to L as sector alignment allows, and
exactly L when L is a multiple of the sector size). This moves real data in both directions at limit scale and verifies
the read-back matches, confirming the HAL programs a limit-sized transfer without tripping its range asserts or silently
truncating HCTSIZ.XferSize / the qTD size field. An over-limit transfer (L + MPS) is then enqueued and MUST be rejected
with ESP_ERR_INVALID_SIZE before any hardware/buffer access.

Control is exercised with a real GET_CONFIGURATION_DESCRIPTOR whose data stage is sized to L - MPS, L and L + MPS; the
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

// Tags for the SCSI WRITE(10)/READ(10) CBWs used by the real limit-scale transfer
#define TEST_BULK_LIMIT_WRITE_TAG   0xCA5E0001
#define TEST_BULK_LIMIT_READ_TAG    0xCA5E0002
// LBA to write the limit-scale test data to. WARNING: writing here overwrites the device's data at this offset.
#define TEST_BULK_LIMIT_LBA         8192

/**
 * @brief Send a SCSI CBW on the bulk OUT pipe and wait for it to complete
 */
static void bulk_send_cbw(hcd_pipe_handle_t out_pipe, urb_t *urb_cbw, bool is_read,
                          unsigned int lba, int sectors, int sector_size, uint32_t tag)
{
    mock_msc_scsi_init_cbw((mock_msc_bulk_cbw_t *)urb_cbw->transfer.data_buffer, is_read, lba, sectors, sector_size, tag);
    urb_cbw->transfer.num_bytes = sizeof(mock_msc_bulk_cbw_t);
    TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(out_pipe, urb_cbw));
    TEST_HCD_EXPECT_PIPE_EVENT(out_pipe, HCD_PIPE_EVENT_URB_DONE);
    TEST_ASSERT_EQUAL_PTR(urb_cbw, hcd_urb_dequeue(out_pipe));
    TEST_HCD_EXPECT_TRANSFER_STATUS(urb_cbw, USB_TRANSFER_STATUS_COMPLETED);
}

/**
 * @brief Receive and validate a SCSI CSW on the bulk IN pipe
 */
static void bulk_recv_csw(hcd_pipe_handle_t in_pipe, urb_t *urb_csw, size_t csw_size, uint32_t tag)
{
    urb_csw->transfer.num_bytes = csw_size;
    TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(in_pipe, urb_csw));
    TEST_HCD_EXPECT_PIPE_EVENT(in_pipe, HCD_PIPE_EVENT_URB_DONE);
    TEST_ASSERT_EQUAL_PTR(urb_csw, hcd_urb_dequeue(in_pipe));
    TEST_ASSERT_TRUE(mock_msc_scsi_check_csw((mock_msc_bulk_csw_t *)urb_csw->transfer.data_buffer, tag));
}

/*
Test HCD bulk transfer size limit

Purpose:
    - Verify hcd_pipe_get_xfer_size_limit() reports a sane, MPS-aligned per-transfer byte limit for a bulk pipe
    - Verify the HCD moves a real, limit-scale bulk transfer to/from the mass-storage device (data integrity checked)
    - Verify the HCD rejects a bulk transfer over the limit with ESP_ERR_INVALID_SIZE before any HW/buffer access
    - Runs for both DMA modes (default = Scatter/Gather, buffer_dma = Buffer DMA) via separate build configs

Procedure:
    - Setup HCD, connect, enumerate the MSC device (no mass-storage reset, mirroring the bulk WRITE test)
    - Open BULK IN and BULK OUT pipes and query the shared size limit L, MPS M and the device's sector size
    - Compute the largest whole number of sectors whose byte size fits L (== L when L is sector-aligned)
    - SCSI WRITE(10) that many sectors of a known pattern, then READ(10) them back and verify the data matches
    - Enqueue an over-limit transfer (L + M) on both directions and expect ESP_ERR_INVALID_SIZE
    - Teardown (port power-off restores device state for the next test)
*/
TEST_CASE("Test HCD bulk transfer size limit", "[bulk][full_speed][high_speed]")
{
    usb_speed_t port_speed = test_hcd_wait_for_conn(port_hdl);
    vTaskDelay(pdMS_TO_TICKS(100));

    // NOTE: intentionally NOT issuing a Bulk-Only Mass Storage Reset (some devices then reject WRITE, see test_hcd_bulk)
    hcd_pipe_handle_t default_pipe = test_hcd_pipe_alloc(port_hdl, NULL, 0, port_speed);
    uint8_t dev_addr = test_hcd_enum_device(default_pipe);
    const dev_msc_info_t *dev_info = dev_msc_get_info();
    ESP_LOGI(TAG, "Device enumerated");

    const usb_ep_desc_t *out_ep_desc = dev_msc_get_out_ep_desc(port_speed);
    const usb_ep_desc_t *in_ep_desc = dev_msc_get_in_ep_desc(port_speed);
    const int in_mps = USB_EP_DESC_GET_MPS(in_ep_desc);
    hcd_pipe_handle_t bulk_out_pipe = test_hcd_pipe_alloc(port_hdl, out_ep_desc, dev_addr, port_speed);
    hcd_pipe_handle_t bulk_in_pipe = test_hcd_pipe_alloc(port_hdl, in_ep_desc, dev_addr, port_speed);

    // The per-transfer limit shared by both directions (use the smaller of the two to be safe)
    const int in_limit = (int)hcd_pipe_get_xfer_size_limit(bulk_in_pipe);
    const int out_limit = (int)hcd_pipe_get_xfer_size_limit(bulk_out_pipe);
    const int limit = (in_limit < out_limit) ? in_limit : out_limit;
    const int mps = hcd_pipe_get_mps(bulk_out_pipe);
    const int sector_size = dev_info->scsi_sector_size;
    TEST_ASSERT_GREATER_THAN(0, limit);
    TEST_ASSERT_EQUAL_MESSAGE(0, limit % mps, "Bulk size limit is not an integer multiple of MPS");

    // Largest whole number of sectors whose byte size fits within the limit -> the real transfer size at/near the limit
    const int sectors = limit / sector_size;
    TEST_ASSERT_GREATER_THAN(0, sectors);
    const int data_size = sectors * sector_size;
    ESP_LOGI(TAG, "BULK limit=%d mps=%d sector=%d -> real transfer %d B (%d sectors)%s",
             limit, mps, sector_size, data_size, sectors, (data_size == limit) ? " == limit" : " (< limit)");

    // ---- Real, limit-scale data transfer with the flash disk: WRITE(10) then READ(10)-back and verify ----
    urb_t *urb_cbw = test_hcd_alloc_urb(0, sizeof(mock_msc_bulk_cbw_t));
    urb_t *urb_write = test_hcd_alloc_urb(0, data_size);
    urb_t *urb_read = test_hcd_alloc_urb(0, data_size);
    const size_t csw_size = sizeof(mock_msc_bulk_csw_t) + (in_mps - (sizeof(mock_msc_bulk_csw_t) % in_mps));
    urb_t *urb_csw = test_hcd_alloc_urb(0, csw_size);

    // WRITE(10): CBW (OUT) -> data (OUT, one limit-scale URB) -> CSW (IN)
    bulk_send_cbw(bulk_out_pipe, urb_cbw, false, TEST_BULK_LIMIT_LBA, sectors, sector_size, TEST_BULK_LIMIT_WRITE_TAG);
    memset(urb_write->transfer.data_buffer, 0xA7, data_size);
    urb_write->transfer.num_bytes = data_size;
    TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(bulk_out_pipe, urb_write));
    TEST_HCD_EXPECT_PIPE_EVENT(bulk_out_pipe, HCD_PIPE_EVENT_URB_DONE);
    TEST_ASSERT_EQUAL_PTR(urb_write, hcd_urb_dequeue(bulk_out_pipe));
    TEST_HCD_EXPECT_TRANSFER_STATUS(urb_write, USB_TRANSFER_STATUS_COMPLETED);
    TEST_ASSERT_EQUAL(data_size, urb_write->transfer.actual_num_bytes);
    bulk_recv_csw(bulk_in_pipe, urb_csw, csw_size, TEST_BULK_LIMIT_WRITE_TAG);

    // READ(10): CBW (OUT) -> data (IN, one limit-scale URB) -> CSW (IN)
    bulk_send_cbw(bulk_out_pipe, urb_cbw, true, TEST_BULK_LIMIT_LBA, sectors, sector_size, TEST_BULK_LIMIT_READ_TAG);
    memset(urb_read->transfer.data_buffer, 0x00, data_size);
    urb_read->transfer.num_bytes = data_size;
    TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(bulk_in_pipe, urb_read));
    TEST_HCD_EXPECT_PIPE_EVENT(bulk_in_pipe, HCD_PIPE_EVENT_URB_DONE);
    TEST_ASSERT_EQUAL_PTR(urb_read, hcd_urb_dequeue(bulk_in_pipe));
    TEST_HCD_EXPECT_TRANSFER_STATUS(urb_read, USB_TRANSFER_STATUS_COMPLETED);
    TEST_ASSERT_EQUAL(data_size, urb_read->transfer.actual_num_bytes);
    bulk_recv_csw(bulk_in_pipe, urb_csw, csw_size, TEST_BULK_LIMIT_READ_TAG);

    // The read-back data at the size limit must match what was written
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(urb_write->transfer.data_buffer, urb_read->transfer.data_buffer, data_size,
                                     "Read-back data does not match written data at the size limit");
    ESP_LOGI(TAG, "Verified %d B (%d sectors) bulk WRITE+READ at the transfer-size limit", data_size, sectors);

    test_hcd_free_urb(urb_cbw);
    test_hcd_free_urb(urb_write);
    test_hcd_free_urb(urb_read);
    test_hcd_free_urb(urb_csw);

    // Over the limit -> rejected at enqueue, before any hardware/buffer access, in both directions
    expect_reject(bulk_out_pipe, limit + mps);
    expect_reject(bulk_in_pipe, limit + mps);

    test_hcd_pipe_free(bulk_out_pipe);
    test_hcd_pipe_free(bulk_in_pipe);
    test_hcd_pipe_free(default_pipe);
    test_hcd_wait_for_disconn(port_hdl, false);
}
