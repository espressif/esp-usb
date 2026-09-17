/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"
#include "esp_log.h"
#include "mock_msc.h"
#include "dev_msc.h"
#include "dev_isoc.h"
#include "hcd_common.h"

static const char *TAG = "XFER_SIZE_LIMIT";

/*
These tests validate the HCD's per-transfer size-limit enforcement for bulk and control transfers. The limit is the
maximum number of bytes the controller can move in a single bulk/control transfer
    - Scatter/Gather: the 17-bit non-isochronous qTD "Total bytes to transfer" field (programming guide Section 6).

Bulk is exercised with a real SCSI WRITE(10)+READ(10) transaction against the mass-storage device, with the data phase
sized to the largest whole number of sectors that fits the limit L (i.e. as close to L as sector alignment allows, and
exactly L when L is a multiple of the sector size). This moves real data in both directions at limit scale and verifies
the read-back matches, confirming the HAL programs a limit-sized transfer without tripping its range asserts or silently
truncating the qTD size field. An over-limit transfer (L + MPS) is then enqueued and MUST be rejected with
ESP_ERR_INVALID_SIZE before any hardware/buffer access.

Control is exercised with a real GET_CONFIGURATION_DESCRIPTOR whose data stage is sized to L - MPS, L and L + MPS; the
first two MUST be accepted (and complete via the device's short descriptor packet), and the last MUST be rejected.

Periodic (interrupt/isochronous) transfers are not bounded by the qTD byte-count field: they use one qTD per packet,
so their size is bounded by the descriptor list length - XFER_LIST_LEN_INTR (== FRAME_LIST_LEN) qTDs for interrupt,
XFER_LIST_LEN_ISOC minus a scheduling timing margin for isochronous. They are exercised with endpoint descriptors
targeting a non-existent device address (isochronous OUT completes without a handshake; interrupt acceptance is fully
exercised by the synchronous descriptor list fill at enqueue), so no periodic-capable device is required.

Maximum transfer sizes in Scatter/Gather DMA mode, per transfer type. The limits are set by the qTD byte-count
fields and the HCD's descriptor list lengths, so they are identical on all targets regardless of the DWC2 core
revision (4.30a on P4 ECO5+/S31, 4.00a on P4 ECO4/S2/S3/H4):

+-------+---------------------------+---------------------------+------------------------------------------------------+
| Type  | Max bytes (HS)            | Max bytes (FS)            | Limited by                                           |
+-------+---------------------------+---------------------------+------------------------------------------------------+
| CTRL  | 131008 (MPS 64)           | 131008 (MPS 64)           | 17-bit non-iso qTD "Total bytes to transfer" field   |
| BULK  | 130560 (MPS 512)          | 131008 (MPS 64)           | (131071 B), floored to a whole number of MPS         |
| INTR  | 32768 (32 x MPS 1024)     | 2048 (32 x MPS 64)        | XFER_LIST_LEN_INTR (32) qTDs, one packet per qTD     |
| ISOC  | 62464 (61 x MPS 1024)     | 62403 (61 x MPS 1023)     | XFER_LIST_LEN_ISOC (64) - XFER_LIST_ISOC_MARGIN (3)  |
|       |                           |                           | descriptors; (61 / interval) packets of <= MPS each  |
+-------+---------------------------+---------------------------+------------------------------------------------------+

All limits are enforced at enqueue time with ESP_ERR_INVALID_SIZE. The ISOC row shows interval = 1; the packet count
scales down with the pipe's interval (61 / interval). The CTRL limit applies to the data stage only (the 8-byte setup
packet is excluded).
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
    // Only one limit-scale data buffer is held at a time: the write buffer is freed before the read buffer is
    // allocated, halving the test's peak memory.
    urb_t *urb_cbw = test_hcd_alloc_urb(0, sizeof(mock_msc_bulk_cbw_t));
    const size_t csw_size = sizeof(mock_msc_bulk_csw_t) + (in_mps - (sizeof(mock_msc_bulk_csw_t) % in_mps));
    urb_t *urb_csw = test_hcd_alloc_urb(0, csw_size);

    // WRITE(10): CBW (OUT) -> data (OUT, one limit-scale URB) -> CSW (IN)
    urb_t *urb_write = test_hcd_alloc_urb(0, data_size);
    bulk_send_cbw(bulk_out_pipe, urb_cbw, false, TEST_BULK_LIMIT_LBA, sectors, sector_size, TEST_BULK_LIMIT_WRITE_TAG);
    for (int i = 0; i < data_size; i++) {
        urb_write->transfer.data_buffer[i] = i % 255;
    }
    urb_write->transfer.num_bytes = data_size;
    TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(bulk_out_pipe, urb_write));
    TEST_HCD_EXPECT_PIPE_EVENT(bulk_out_pipe, HCD_PIPE_EVENT_URB_DONE);
    TEST_ASSERT_EQUAL_PTR(urb_write, hcd_urb_dequeue(bulk_out_pipe));
    TEST_HCD_EXPECT_TRANSFER_STATUS(urb_write, USB_TRANSFER_STATUS_COMPLETED);
    TEST_ASSERT_EQUAL(data_size, urb_write->transfer.actual_num_bytes);
    bulk_recv_csw(bulk_in_pipe, urb_csw, csw_size, TEST_BULK_LIMIT_WRITE_TAG);
    // Free the write buffer before allocating the read buffer
    test_hcd_free_urb(urb_write);

    // READ(10): CBW (OUT) -> data (IN, one limit-scale URB) -> CSW (IN)
    urb_t *urb_read = test_hcd_alloc_urb(0, data_size);
    bulk_send_cbw(bulk_out_pipe, urb_cbw, true, TEST_BULK_LIMIT_LBA, sectors, sector_size, TEST_BULK_LIMIT_READ_TAG);
    memset(urb_read->transfer.data_buffer, 0x00, data_size);
    urb_read->transfer.num_bytes = data_size;
    TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(bulk_in_pipe, urb_read));
    TEST_HCD_EXPECT_PIPE_EVENT(bulk_in_pipe, HCD_PIPE_EVENT_URB_DONE);
    TEST_ASSERT_EQUAL_PTR(urb_read, hcd_urb_dequeue(bulk_in_pipe));
    TEST_HCD_EXPECT_TRANSFER_STATUS(urb_read, USB_TRANSFER_STATUS_COMPLETED);
    TEST_ASSERT_EQUAL(data_size, urb_read->transfer.actual_num_bytes);
    bulk_recv_csw(bulk_in_pipe, urb_csw, csw_size, TEST_BULK_LIMIT_READ_TAG);

    // The read-back data must match the known written pattern (i % 255). urb_write is already freed, so the expected
    // pattern is regenerated here; report the first mismatching byte.
    for (int i = 0; i < data_size; i++) {
        const uint8_t expected = (uint8_t)(i % 255);
        const uint8_t actual = urb_read->transfer.data_buffer[i];
        if (actual != expected) {
            char msg[120];
            snprintf(msg, sizeof(msg), "Read-back data does not match written pattern at byte %d (size limit)", i);
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(expected, actual, msg);
        }
    }
    ESP_LOGI(TAG, "Verified %d B (%d sectors) bulk WRITE+READ at the transfer-size limit", data_size, sectors);

    test_hcd_free_urb(urb_cbw);
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

// -----------------------------------------------------------------------------
// Periodic transfer size limits
// -----------------------------------------------------------------------------

// Mirror the HCD's periodic descriptor list lengths (hcd_dwc.c): periodic transfers use one qTD per packet, so their
// size is bounded by the descriptor list length - not by hcd_pipe_get_xfer_size_limit(), which applies to bulk/control
#define TEST_INTR_XFER_LIST_LEN     32  // XFER_LIST_LEN_INTR == FRAME_LIST_LEN (USB_HAL_FRAME_LIST_LEN_32)
#define TEST_ISOC_XFER_LIST_LEN     64  // XFER_LIST_LEN_ISOC
#define TEST_ISOC_XFER_LIST_MARGIN  3   // XFER_LIST_ISOC_MARGIN: list slots reserved for scheduling timing margin
#define TEST_INTR_MPS               64  // Fabricated interrupt endpoint MPS, legal at both FS and HS

/**
 * @brief Assert the HCD rejects an over-limit interrupt transfer at enqueue time
 *
 * The rejection happens before the data buffer is touched or the channel is activated, so a tiny buffer with an
 * over-sized num_bytes is sufficient.
 *
 * @param pipe       Interrupt pipe to enqueue on
 * @param num_bytes  Total transfer num_bytes
 * @param flags      Transfer flags (e.g. USB_TRANSFER_FLAG_ZERO_PACK)
 */
static void expect_reject_intr(hcd_pipe_handle_t pipe, int num_bytes, uint32_t flags)
{
    urb_t *urb = test_hcd_alloc_urb(0, TEST_INTR_MPS);
    urb->transfer.num_bytes = num_bytes;
    urb->transfer.flags = flags;
    ESP_LOGI(TAG, "Expecting reject (intr): num_bytes=%d flags=0x%"PRIx32, num_bytes, flags);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, hcd_urb_enqueue(pipe, urb));
    test_hcd_free_urb(urb);
}

/**
 * @brief Assert the HCD rejects an over-limit isochronous transfer at enqueue time
 *
 * The rejection happens before the data buffer is touched or the channel is activated, so a tiny buffer is
 * sufficient.
 *
 * @param pipe              Isochronous pipe to enqueue on
 * @param num_isoc_packets  Number of isochronous packets in the transfer
 * @param mps               Packet size used for each packet descriptor
 */
static void expect_reject_isoc(hcd_pipe_handle_t pipe, int num_isoc_packets, int mps)
{
    urb_t *urb = test_hcd_alloc_urb(num_isoc_packets, mps);
    urb->transfer.num_bytes = num_isoc_packets * mps;
    for (int i = 0; i < num_isoc_packets; i++) {
        urb->transfer.isoc_packet_desc[i].num_bytes = mps;
    }
    ESP_LOGI(TAG, "Expecting reject (isoc): num_isoc_packets=%d", num_isoc_packets);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, hcd_urb_enqueue(pipe, urb));
    test_hcd_free_urb(urb);
}

/**
 * @brief Assert the HCD accepts an at/under-limit isochronous transfer and completes it
 *
 * The pipe targets a non-existent device address: isochronous transfers have no handshake, so the URB completes
 * on its own once scheduled.
 *
 * @param pipe              Isochronous pipe to enqueue on
 * @param num_isoc_packets  Number of isochronous packets in the transfer
 * @param mps               Packet size used for each packet descriptor
 */
static void expect_accept_isoc(hcd_pipe_handle_t pipe, int num_isoc_packets, int mps)
{
    urb_t *urb = test_hcd_alloc_urb(num_isoc_packets, num_isoc_packets * mps);
    urb->transfer.num_bytes = num_isoc_packets * mps;
    for (int i = 0; i < num_isoc_packets; i++) {
        urb->transfer.isoc_packet_desc[i].num_bytes = mps;
    }
    ESP_LOGI(TAG, "Expecting accept (isoc): num_isoc_packets=%d (%d bytes)", num_isoc_packets, num_isoc_packets * mps);
    TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(pipe, urb));
    TEST_HCD_EXPECT_PIPE_EVENT(pipe, HCD_PIPE_EVENT_URB_DONE);
    TEST_ASSERT_EQUAL_PTR(urb, hcd_urb_dequeue(pipe));
    TEST_HCD_EXPECT_TRANSFER_STATUS(urb, USB_TRANSFER_STATUS_COMPLETED);
    TEST_ASSERT_EQUAL(num_isoc_packets * mps, urb->transfer.actual_num_bytes);
    for (int i = 0; i < num_isoc_packets; i++) {
        TEST_ASSERT_EQUAL_MESSAGE(USB_TRANSFER_STATUS_COMPLETED, urb->transfer.isoc_packet_desc[i].status, "Packet NOT completed");
    }
    test_hcd_free_urb(urb);
}

/*
Test HCD interrupt transfer size limit

Purpose:
    - Interrupt transfers use one qTD per packet (plus one extra qTD for the optional zero-length packet), so the
      transfer is bounded by the interrupt descriptor list length (XFER_LIST_LEN_INTR == FRAME_LIST_LEN == 32 qTDs)
    - Verify the HCD rejects interrupt transfers needing more qTDs than the list holds with ESP_ERR_INVALID_SIZE,
      including the zero-length-packet corner case (an MPS-aligned OUT transfer with USB_TRANSFER_FLAG_ZERO_PACK
      needs one extra qTD)
    - Verify the HCD accepts interrupt transfers that fill the descriptor list exactly (the list is filled
      synchronously at enqueue, so an accepted transfer must not trip the fill-time assert)

Procedure:
    - Setup HCD and wait for connection (no enumeration: the pipes target a non-existent device address, the
      acceptance path is fully exercised by the synchronous descriptor list fill at enqueue)
    - Allocate interrupt IN and OUT pipes with a fabricated endpoint descriptor (MPS=64, bInterval=1)
    - Enqueue 33*MPS IN, 32*MPS+1 OUT and 32*MPS+ZLP OUT (all need 33 qTDs) -> rejected with ESP_ERR_INVALID_SIZE
    - Enqueue 32*MPS IN and 31*MPS+ZLP OUT (both exactly 32 qTDs) -> accepted; halt+flush to reclaim the URBs
    - Teardown
*/
TEST_CASE("Test HCD interrupt transfer size limit", "[intr][full_speed][high_speed]")
{
    usb_speed_t port_speed = test_hcd_wait_for_conn(port_hdl);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Fabricated endpoint descriptors; the device address is intentionally left non-existent (no enumeration)
    usb_ep_desc_t intr_in_ep_desc = {
        .bLength = USB_EP_DESC_SIZE,
        .bDescriptorType = USB_B_DESCRIPTOR_TYPE_ENDPOINT,
        .bEndpointAddress = 0x81, // IN endpoint
        .bmAttributes = USB_BM_ATTRIBUTES_XFER_INT,
        .wMaxPacketSize = TEST_INTR_MPS,
        .bInterval = 1,
    };
    usb_ep_desc_t intr_out_ep_desc = intr_in_ep_desc;
    intr_out_ep_desc.bEndpointAddress = 0x02; // OUT endpoint
    hcd_pipe_handle_t intr_in_pipe = test_hcd_pipe_alloc(port_hdl, &intr_in_ep_desc, 1, port_speed);
    hcd_pipe_handle_t intr_out_pipe = test_hcd_pipe_alloc(port_hdl, &intr_out_ep_desc, 1, port_speed);

    // --- Rejected: need more qTDs than the descriptor list holds (33 > 32) ---
    expect_reject_intr(intr_in_pipe, (TEST_INTR_XFER_LIST_LEN + 1) * TEST_INTR_MPS, 0);          // IN: 33 packets
    expect_reject_intr(intr_out_pipe, TEST_INTR_XFER_LIST_LEN * TEST_INTR_MPS + 1, 0);            // OUT: 32 packets + short packet
    expect_reject_intr(intr_out_pipe, TEST_INTR_XFER_LIST_LEN * TEST_INTR_MPS, USB_TRANSFER_FLAG_ZERO_PACK); // OUT: 32 packets + ZLP

    // --- Accepted: exactly fills the descriptor list (32 qTDs) ---
    // IN: 32 packets
    urb_t *urb_in = test_hcd_alloc_urb(0, TEST_INTR_XFER_LIST_LEN * TEST_INTR_MPS);
    urb_in->transfer.num_bytes = TEST_INTR_XFER_LIST_LEN * TEST_INTR_MPS;
    TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(intr_in_pipe, urb_in));
    // OUT: 31 packets + zero-length packet
    urb_t *urb_out = test_hcd_alloc_urb(0, (TEST_INTR_XFER_LIST_LEN - 1) * TEST_INTR_MPS);
    urb_out->transfer.num_bytes = (TEST_INTR_XFER_LIST_LEN - 1) * TEST_INTR_MPS;
    urb_out->transfer.flags = USB_TRANSFER_FLAG_ZERO_PACK;
    TEST_ASSERT_EQUAL(ESP_OK, hcd_urb_enqueue(intr_out_pipe, urb_out));

    // Reclaim the URBs: the non-existent endpoint can never complete them
    TEST_ASSERT_EQUAL(ESP_OK, hcd_pipe_command(intr_in_pipe, HCD_PIPE_CMD_HALT));
    TEST_ASSERT_EQUAL(ESP_OK, hcd_pipe_command(intr_out_pipe, HCD_PIPE_CMD_HALT));
    TEST_ASSERT_EQUAL(ESP_OK, hcd_pipe_command(intr_in_pipe, HCD_PIPE_CMD_FLUSH));
    TEST_ASSERT_EQUAL(ESP_OK, hcd_pipe_command(intr_out_pipe, HCD_PIPE_CMD_FLUSH));
    TEST_HCD_EXPECT_PIPE_EVENT(intr_in_pipe, HCD_PIPE_EVENT_URB_DONE);
    TEST_HCD_EXPECT_PIPE_EVENT(intr_out_pipe, HCD_PIPE_EVENT_URB_DONE);
    TEST_ASSERT_EQUAL_PTR(urb_in, hcd_urb_dequeue(intr_in_pipe));
    TEST_ASSERT_EQUAL_PTR(urb_out, hcd_urb_dequeue(intr_out_pipe));
    test_hcd_free_urb(urb_in);
    test_hcd_free_urb(urb_out);

    test_hcd_pipe_free(intr_in_pipe);
    test_hcd_pipe_free(intr_out_pipe);
    test_hcd_wait_for_disconn(port_hdl, false);
}

/*
Test HCD isochronous transfer size limit

Purpose:
    - Isochronous transfers use one qTD per packet, spaced by the pipe's interval in a circular descriptor list of
      XFER_LIST_LEN_ISOC (64) entries, with XFER_LIST_ISOC_MARGIN (3) slots reserved for scheduling timing margin.
      The transfer is thus bounded to (64 - 3) / interval packets
    - Verify the HCD rejects isochronous transfers whose packets do not fit into the descriptor list with
      ESP_ERR_INVALID_SIZE
    - Verify the HCD accepts isochronous transfers that exactly fit the descriptor list and completes them
    - Verify the bound scales with the pipe's interval

Procedure:
    - Setup HCD, connect, enumerate the device
    - Allocate an ISOC OUT pipe to a non-existent device address (isochronous transfers have no handshake, so the
      URBs complete without a device), interval = 1
    - Enqueue 62 packets (62 descriptors > 61) -> rejected; enqueue 61 packets (== 61) -> accepted and completes
    - Reallocate the pipe with bInterval = 2 (interval = 2 for both FS and HS isochronous)
    - Enqueue 31 packets (62 descriptors > 61) -> rejected; enqueue 30 packets (60 <= 61) -> accepted and completes
    - Teardown
*/
TEST_CASE("Test HCD isochronous transfer size limit", "[isoc][full_speed][high_speed]")
{
    usb_speed_t port_speed = test_hcd_wait_for_conn(port_hdl);
    vTaskDelay(pdMS_TO_TICKS(100));

    hcd_pipe_handle_t default_pipe = test_hcd_pipe_alloc(port_hdl, NULL, 0, port_speed);
    uint8_t dev_addr = test_hcd_enum_device(default_pipe);

    // ISOC OUT pipe to a non-existent endpoint, interval = 1
    usb_ep_desc_t isoc_ep_desc;
    memcpy(&isoc_ep_desc, dev_isoc_get_out_ep_desc(port_speed), sizeof(usb_ep_desc_t));
    const int mps = USB_EP_DESC_GET_MPS(&isoc_ep_desc);
    hcd_pipe_handle_t isoc_pipe = test_hcd_pipe_alloc(port_hdl, &isoc_ep_desc, dev_addr + 1, port_speed);

    // Effective limit in packets at interval = 1: list length minus the scheduling margin
    const int max_packets = TEST_ISOC_XFER_LIST_LEN - TEST_ISOC_XFER_LIST_MARGIN;

    // Over the limit (62 descriptors > 61) -> rejected at enqueue, before the descriptor list is touched
    expect_reject_isoc(isoc_pipe, max_packets + 1, mps);
    // Exactly at the limit (61 descriptors) -> accepted and completes
    expect_accept_isoc(isoc_pipe, max_packets, mps);

    // Interval scaling: interval = 2^(bInterval-1) = 2 (micro)frames for both FS and HS isochronous
    isoc_ep_desc.bInterval = 2;
    hcd_pipe_handle_t isoc_pipe_i2 = test_hcd_pipe_alloc(port_hdl, &isoc_ep_desc, dev_addr + 1, port_speed);
    // 31 packets * interval 2 = 62 descriptors > 61 -> rejected
    expect_reject_isoc(isoc_pipe_i2, max_packets / 2 + 1, mps);
    // 30 packets * interval 2 = 60 descriptors <= 61 -> accepted and completes
    expect_accept_isoc(isoc_pipe_i2, max_packets / 2, mps);

    test_hcd_pipe_free(isoc_pipe_i2);
    test_hcd_pipe_free(isoc_pipe);
    test_hcd_pipe_free(default_pipe);
    test_hcd_wait_for_disconn(port_hdl, false);
}
