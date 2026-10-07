/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

// MSC + NCM composite device that shows how MSC storage IO affects a USB network interface.
//
// - MSC exposes an SD card, or the "storage" flash partition, as a USB drive.
// - NCM makes the device a USB Ethernet adapter with IP 192.168.4.1 and a DHCP server.
// - The host measures network latency and throughput (tools/measure.py) while copying to the drive.
// - Build with and without CONFIG_TINYUSB_MSC_ASYNC_IO to compare.

#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "example.h"

static const char *TAG = "msc_ncm";

#define PROGRESS_PERIOD_MS  1000

// Logs the storage rate on the UART console while the host accesses the drive
static void msc_progress_task(void *arg)
{
    io_bytes_t last = io_bytes_get();
    ncm_tx_stats_t last_tx = ncm_netif_tx_stats();
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(PROGRESS_PERIOD_MS));
        const io_bytes_t now = io_bytes_get();
        const ncm_tx_stats_t tx = ncm_netif_tx_stats();
        if (now.read != last.read || now.written != last.written || tx.sent != last_tx.sent) {
            ESP_LOGI(TAG, "[msc] write %5.1f MB/s  read %5.1f MB/s | [ncm tx/s] sent %" PRIu32 " busy %" PRIu32
                     " timeout %" PRIu32 " retries %" PRIu32 " | max send %" PRIu32 " us",
                     (now.written - last.written) / 1e6, (now.read - last.read) / 1e6,
                     tx.sent - last_tx.sent, tx.busy_drops - last_tx.busy_drops,
                     tx.timeout_drops - last_tx.timeout_drops, tx.busy_retries - last_tx.busy_retries,
                     tx.max_send_us);
        }
        last = now;
        last_tx = tx;
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

#if CONFIG_EXAMPLE_STORAGE_SDCARD
    // The card belongs to the host: the device never mounts or formats it
    const tinyusb_msc_driver_config_t driver_cfg = {
        .user_flags.auto_mount_off = 1,
    };
    ESP_ERROR_CHECK(tinyusb_msc_install_driver(&driver_cfg));
    const tinyusb_msc_storage_config_t storage_cfg = {
        .medium.blockdev = storage_init(),
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
        .fat_fs = {
            .base_path = "/data",
            .do_not_format = true,
        },
    };
#else
    // Mounting to the application first formats the partition with FAT if it is empty.
    // The storage is handed over to USB when the host connects.
    const tinyusb_msc_storage_config_t storage_cfg = {
        .medium.blockdev = storage_init(),
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,
        .fat_fs = {
            .base_path = "/data",
            .config.max_files = 2,
        },
    };
#endif
    ESP_ERROR_CHECK(tinyusb_msc_new_storage_blockdev(&storage_cfg, NULL));

    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    ESP_ERROR_CHECK(ncm_netif_init());
    net_servers_start();

    xTaskCreate(msc_progress_task, "msc_progress", 3072, NULL, 3, NULL);

    ESP_LOGI(TAG, "Storage: %s, async IO: %s, simulated medium delay: %d ms. Device IP: 192.168.4.1",
             STORAGE_STR, IO_MODE_STR, CONFIG_EXAMPLE_MEDIUM_DELAY_MS);
}
