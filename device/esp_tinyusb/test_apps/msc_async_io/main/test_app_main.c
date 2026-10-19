/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

// CDC + MSC composite device for testing MSC async IO, driven by check_msc_async_io.py on the host.
//
// - MSC exposes a RAM disk through esp_blockdev. The RAM disk adds an optional delay per
//   read and write (to emulate slow media), counts accesses and can fail writes on request.
// - CDC echoes every packet straight from the RX callback, which runs in the TinyUSB task.
//   Any time the TinyUSB task spends blocked in MSC IO shows up as CDC echo latency.
// - Control packets (first byte 0xFF) change the delay, query counters or inject failures,
//   see README.md.

#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_blockdev.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "tinyusb_cdc_acm.h"

static const char *TAG = "msc_async_test";

#define CTRL_PREFIX             0xFF
#define CTRL_SET_DELAY          'D'     // 0xFF 'D' <delay_ms u16 LE>  -> "D <ms>\n"
#define CTRL_GET_STATS          'S'     // 0xFF 'S'                    -> "S <reads> <writes> <delay_ms>\n"
#define CTRL_RESET_STATS        'R'     // 0xFF 'R'                    -> "R\n"
#define CTRL_FAIL_WRITES        'F'     // 0xFF 'F' <count u16 LE>     -> "F <count>\n", next <count> writes fail
#define CTRL_GET_INFO           'I'     // 0xFF 'I'                    -> "I <io_mode>\n"
#define CTRL_GET_STACK          'W'     // 0xFF 'W'                    -> "W <used> <size>\n", peak stack use of the async IO worker, -1 -1 if none

#define ASYNC_IO_TASK_NAME      "msc_async_io"  // Task name used by esp_tinyusb

#if CONFIG_TINYUSB_MSC_ASYNC_IO
#define INFO_IO_MODE    "async"
#else
#define INFO_IO_MODE    "sync"
#endif

static volatile uint32_t s_delay_ms;
static volatile uint32_t s_reads;
static volatile uint32_t s_writes;
static volatile uint32_t s_fail_writes;

// ---------------------------------- RAM disk with delay, counters and failure injection ----------------------------------

#define RAMDISK_SECTOR_SIZE     512
#define RAMDISK_SIZE            ((size_t)CONFIG_TEST_RAMDISK_SIZE_KB * 1024)

static uint8_t *s_ramdisk;

// Emulates slow media
static void ramdisk_delay(void)
{
    const uint32_t delay_ms = s_delay_ms;
    if (delay_ms) {
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

static esp_err_t ramdisk_read(esp_blockdev_handle_t dev, uint8_t *dst, size_t dst_size, uint64_t src_addr, size_t len)
{
    ESP_RETURN_ON_FALSE(len <= dst_size && src_addr + len <= RAMDISK_SIZE, ESP_ERR_INVALID_SIZE, TAG, "read out of range");
    ramdisk_delay();
    memcpy(dst, s_ramdisk + src_addr, len);
    s_reads++;
    return ESP_OK;
}

static esp_err_t ramdisk_write(esp_blockdev_handle_t dev, const uint8_t *src, uint64_t dst_addr, size_t len)
{
    ESP_RETURN_ON_FALSE(dst_addr + len <= RAMDISK_SIZE, ESP_ERR_INVALID_SIZE, TAG, "write out of range");
    ramdisk_delay();
    if (s_fail_writes) {
        s_fail_writes--;
        ESP_LOGW(TAG, "Injected write failure at 0x%llx", (unsigned long long)dst_addr);
        return ESP_FAIL;
    }
    memcpy(s_ramdisk + dst_addr, src, len);
    s_writes++;
    return ESP_OK;
}

static esp_err_t ramdisk_erase(esp_blockdev_handle_t dev, uint64_t start_addr, size_t len)
{
    ESP_RETURN_ON_FALSE(start_addr + len <= RAMDISK_SIZE, ESP_ERR_INVALID_SIZE, TAG, "erase out of range");
    memset(s_ramdisk + start_addr, 0xFF, len);
    return ESP_OK;
}

static const esp_blockdev_ops_t s_ramdisk_ops = {
    .read = ramdisk_read,
    .write = ramdisk_write,
    .erase = ramdisk_erase,
};

static esp_blockdev_t s_ramdisk_dev = {
    .device_flags = { .val = 0 },   // RAM: no erase before write, any bit pattern writable
    .geometry = {
        .disk_size = RAMDISK_SIZE,
        .read_size = RAMDISK_SECTOR_SIZE,
        .write_size = RAMDISK_SECTOR_SIZE,
        .erase_size = RAMDISK_SECTOR_SIZE,
    },
    .ops = &s_ramdisk_ops,
};

static esp_err_t ramdisk_init(void)
{
    s_ramdisk = heap_caps_malloc(RAMDISK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_ramdisk == NULL) {
        s_ramdisk = heap_caps_malloc(RAMDISK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    ESP_RETURN_ON_FALSE(s_ramdisk != NULL, ESP_ERR_NO_MEM, TAG, "Cannot allocate %u B RAM disk", (unsigned)RAMDISK_SIZE);
    memset(s_ramdisk, 0, RAMDISK_SIZE);
    ESP_LOGI(TAG, "RAM disk: %u KiB in %s", (unsigned)(RAMDISK_SIZE / 1024),
             esp_ptr_external_ram(s_ramdisk) ? "PSRAM" : "internal RAM");
    return ESP_OK;
}

// ---------------------------------- CDC echo + control ----------------------------------

static void cdc_reply(int itf, const char *str)
{
    tinyusb_cdcacm_write_queue(itf, (const uint8_t *)str, strlen(str));
    tinyusb_cdcacm_write_flush(itf, 0);
}

static void cdc_handle_ctrl(int itf, const uint8_t *buf, size_t len)
{
    char reply[64];

    if (len >= 4 && buf[1] == CTRL_SET_DELAY) {
        s_delay_ms = (uint32_t)buf[2] | ((uint32_t)buf[3] << 8);
        snprintf(reply, sizeof(reply), "D %"PRIu32"\n", s_delay_ms);
    } else if (len >= 2 && buf[1] == CTRL_GET_STATS) {
        snprintf(reply, sizeof(reply), "S %"PRIu32" %"PRIu32" %"PRIu32"\n", s_reads, s_writes, s_delay_ms);
    } else if (len >= 2 && buf[1] == CTRL_RESET_STATS) {
        s_reads = 0;
        s_writes = 0;
        snprintf(reply, sizeof(reply), "R\n");
    } else if (len >= 4 && buf[1] == CTRL_FAIL_WRITES) {
        s_fail_writes = (uint32_t)buf[2] | ((uint32_t)buf[3] << 8);
        snprintf(reply, sizeof(reply), "F %"PRIu32"\n", s_fail_writes);
    } else if (len >= 2 && buf[1] == CTRL_GET_INFO) {
        snprintf(reply, sizeof(reply), "I %s\n", INFO_IO_MODE);
    } else if (len >= 2 && buf[1] == CTRL_GET_STACK) {
#if CONFIG_TINYUSB_MSC_ASYNC_IO
        TaskHandle_t worker = xTaskGetHandle(ASYNC_IO_TASK_NAME);
        const long size = TINYUSB_MSC_ASYNC_IO_TASK_DEFAULT_SIZE;
        const long used = worker ? size - (long)uxTaskGetStackHighWaterMark(worker) : -1;
        snprintf(reply, sizeof(reply), "W %ld %ld\n", used, worker ? size : -1L);
#else
        snprintf(reply, sizeof(reply), "W -1 -1\n");
#endif
    } else {
        snprintf(reply, sizeof(reply), "E\n");
    }
    cdc_reply(itf, reply);
}

// Runs in the TinyUSB task: echo latency directly reflects how long that task was busy
static void cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    static uint8_t buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE];
    size_t rx_size = 0;

    if (tinyusb_cdcacm_read(itf, buf, sizeof(buf), &rx_size) != ESP_OK || rx_size == 0) {
        return;
    }
    if (buf[0] == CTRL_PREFIX) {
        cdc_handle_ctrl(itf, buf, rx_size);
        return;
    }
    tinyusb_cdcacm_write_queue(itf, buf, rx_size);
    tinyusb_cdcacm_write_flush(itf, 0);
}

// ---------------------------------- main ----------------------------------

void app_main(void)
{
    ESP_ERROR_CHECK(ramdisk_init());

    // Mounting to APP first formats the empty RAM disk with FAT.
    // The storage is handed to USB automatically when the host connects.
    const tinyusb_msc_storage_config_t storage_cfg = {
        .medium.blockdev = &s_ramdisk_dev,
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,
        .fat_fs = {
            .base_path = "/ramdisk",
            .config.max_files = 2,
        },
    };
    ESP_ERROR_CHECK(tinyusb_msc_new_storage_blockdev(&storage_cfg, NULL));

    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    const tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = cdc_rx_callback,
    };
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&acm_cfg));

    s_delay_ms = CONFIG_TEST_IO_DELAY_MS;
    ESP_LOGI(TAG, "Ready. MSC IO mode: %s, added storage delay: %"PRIu32" ms",
             INFO_IO_MODE, s_delay_ms);
}
