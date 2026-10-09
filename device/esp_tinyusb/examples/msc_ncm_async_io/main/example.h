/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_blockdev.h"

#if CONFIG_TINYUSB_MSC_ASYNC_IO
#define IO_MODE_STR     "ON"
#else
#define IO_MODE_STR     "OFF"
#endif

#if CONFIG_EXAMPLE_STORAGE_SDCARD
#define STORAGE_STR     "SD card"
#else
#define STORAGE_STR     "internal flash"
#endif

// Bytes moved by the storage, updated by the block device wrapper
typedef struct {
    uint64_t read;
    uint64_t written;
} io_bytes_t;

io_bytes_t io_bytes_get(void);

// Opens the storage and returns it wrapped by the counting block device
esp_blockdev_handle_t storage_init(void);

// Creates the lwIP interface on top of USB NCM, with a DHCP server on 192.168.4.1.
// Call after tinyusb_driver_install().
esp_err_t ncm_netif_init(void);

// NCM transmit counters. Written only by the lwIP task, so a snapshot may be slightly torn.
typedef struct {
    uint32_t sent;          // frames accepted by USB
    uint32_t busy_drops;    // dropped: all IN NTB buffers were full (tinyusb_net_send_sync() returned ESP_FAIL)
    uint32_t timeout_drops; // dropped: the TinyUSB task did not take the frame within the timeout
    uint32_t busy_retries;  // retries after "buffers full" (CONFIG_EXAMPLE_NCM_TX_RETRY_BUSY)
    uint32_t max_send_us;   // longest netif_transmit() call, including retries
} ncm_tx_stats_t;

ncm_tx_stats_t ncm_netif_tx_stats(void);

// Frames lwIP wanted to send that USB did not take (busy + timeout)
uint32_t ncm_netif_tx_drops(void);

// Starts the UDP echo, TCP sink, TCP source and HTTP status servers
void net_servers_start(void);
