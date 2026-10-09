/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

// CDC + MSC composite device for measuring how MSC storage IO delays other classes.
//
// - MSC exposes a block device: a RAM disk, or an SD card (CONFIG_BENCH_STORAGE_SDCARD).
//   The block device is wrapped by bench_bdev, which adds an optional delay per access,
//   read/write counters and write failure injection.
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
#if CONFIG_BENCH_STORAGE_SDCARD
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif

static const char *TAG = "msc_bench";

#define CTRL_PREFIX             0xFF
#define CTRL_SET_DELAY          'D'     // 0xFF 'D' <delay_ms u16 LE>  -> "D <ms>\n"
#define CTRL_GET_STATS          'S'     // 0xFF 'S'                    -> "S <reads> <writes> <delay_ms>\n"
#define CTRL_RESET_STATS        'R'     // 0xFF 'R'                    -> "R\n"
#define CTRL_FAIL_WRITES        'F'     // 0xFF 'F' <count u16 LE>     -> "F <count>\n", next <count> writes fail
#define CTRL_GET_INFO           'I'     // 0xFF 'I'                    -> "I <storage> <io_mode> <sd_khz>\n"
#define CTRL_GET_STACK          'W'     // 0xFF 'W'                    -> "W <used> <size>\n", peak stack use of the async IO worker, -1 -1 if none

#define ASYNC_IO_TASK_NAME      "msc_async_io"  // Task name used by esp_tinyusb

#if CONFIG_BENCH_STORAGE_SDCARD
#define INFO_STORAGE    "sd"
#else
#define INFO_STORAGE    "ramdisk"
#endif
#if CONFIG_TINYUSB_MSC_ASYNC_IO
#define INFO_IO_MODE    "async"
#else
#define INFO_IO_MODE    "sync"
#endif

static volatile uint32_t s_delay_ms;
static volatile uint32_t s_reads;
static volatile uint32_t s_writes;
static volatile uint32_t s_fail_writes;

// ------------------------- bench_bdev: wrapper with delay, counters, failure injection -------------------------

#define INNER(dev)  ((esp_blockdev_handle_t)(dev)->ctx)

static void bench_bdev_delay(void)
{
    const uint32_t delay_ms = s_delay_ms;
    if (delay_ms) {
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

// The SD driver copies through a bounce buffer when the buffer is not cache-line aligned,
// which would slow down the variant that uses that buffer. Report it once per direction.
static void bench_bdev_check_alignment(const void *buf, size_t len, bool is_write)
{
#if CONFIG_CACHE_L1_CACHE_LINE_SIZE
    static bool reported[2];
    if (!reported[is_write] && (((uintptr_t)buf | len) % CONFIG_CACHE_L1_CACHE_LINE_SIZE)) {
        reported[is_write] = true;
        ESP_LOGW(TAG, "%s buffer %p (len %u) is not %d-byte aligned", is_write ? "Write" : "Read",
                 buf, (unsigned)len, CONFIG_CACHE_L1_CACHE_LINE_SIZE);
    }
#endif
}

static esp_err_t bench_bdev_read(esp_blockdev_handle_t dev, uint8_t *dst, size_t dst_size, uint64_t src_addr, size_t len)
{
    bench_bdev_delay();
    bench_bdev_check_alignment(dst, len, false);
    esp_err_t err = INNER(dev)->ops->read(INNER(dev), dst, dst_size, src_addr, len);
    s_reads++;
    return err;
}

static esp_err_t bench_bdev_write(esp_blockdev_handle_t dev, const uint8_t *src, uint64_t dst_addr, size_t len)
{
    bench_bdev_delay();
    bench_bdev_check_alignment(src, len, true);
    if (s_fail_writes) {
        s_fail_writes--;
        ESP_LOGW(TAG, "Injected write failure at 0x%llx", (unsigned long long)dst_addr);
        return ESP_FAIL;
    }
    esp_err_t err = INNER(dev)->ops->write(INNER(dev), src, dst_addr, len);
    s_writes++;
    return err;
}

static esp_err_t bench_bdev_erase(esp_blockdev_handle_t dev, uint64_t start_addr, size_t len)
{
    return INNER(dev)->ops->erase ? INNER(dev)->ops->erase(INNER(dev), start_addr, len) : ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t bench_bdev_sync(esp_blockdev_handle_t dev)
{
    return INNER(dev)->ops->sync ? INNER(dev)->ops->sync(INNER(dev)) : ESP_OK;
}

static esp_err_t bench_bdev_ioctl(esp_blockdev_handle_t dev, const uint8_t cmd, void *args)
{
    return INNER(dev)->ops->ioctl ? INNER(dev)->ops->ioctl(INNER(dev), cmd, args) : ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t bench_bdev_release(esp_blockdev_handle_t dev)
{
    return INNER(dev)->ops->release ? INNER(dev)->ops->release(INNER(dev)) : ESP_OK;
}

static const esp_blockdev_ops_t s_bench_bdev_ops = {
    .read = bench_bdev_read,
    .write = bench_bdev_write,
    .erase = bench_bdev_erase,
    .sync = bench_bdev_sync,
    .ioctl = bench_bdev_ioctl,
    .release = bench_bdev_release,
};

static esp_blockdev_t s_bench_bdev;

static esp_blockdev_handle_t bench_bdev_wrap(esp_blockdev_handle_t inner)
{
    s_bench_bdev.ctx = inner;
    s_bench_bdev.device_flags = inner->device_flags;
    s_bench_bdev.geometry = inner->geometry;
    s_bench_bdev.ops = &s_bench_bdev_ops;
    return &s_bench_bdev;
}

// ---------------------------------- RAM block device ----------------------------------

#if CONFIG_BENCH_STORAGE_RAMDISK
#define RAMDISK_SECTOR_SIZE     512
#define RAMDISK_SIZE            ((size_t)CONFIG_BENCH_RAMDISK_SIZE_KB * 1024)

static uint8_t *s_ramdisk;

static esp_err_t ramdisk_read(esp_blockdev_handle_t dev, uint8_t *dst, size_t dst_size, uint64_t src_addr, size_t len)
{
    ESP_RETURN_ON_FALSE(len <= dst_size && src_addr + len <= RAMDISK_SIZE, ESP_ERR_INVALID_SIZE, TAG, "read out of range");
    memcpy(dst, s_ramdisk + src_addr, len);
    return ESP_OK;
}

static esp_err_t ramdisk_write(esp_blockdev_handle_t dev, const uint8_t *src, uint64_t dst_addr, size_t len)
{
    ESP_RETURN_ON_FALSE(dst_addr + len <= RAMDISK_SIZE, ESP_ERR_INVALID_SIZE, TAG, "write out of range");
    memcpy(s_ramdisk + dst_addr, src, len);
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

static esp_err_t storage_init(esp_blockdev_handle_t *out)
{
    s_ramdisk = heap_caps_malloc(RAMDISK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_ramdisk == NULL) {
        s_ramdisk = heap_caps_malloc(RAMDISK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    ESP_RETURN_ON_FALSE(s_ramdisk != NULL, ESP_ERR_NO_MEM, TAG, "Cannot allocate %u B RAM disk", (unsigned)RAMDISK_SIZE);
    memset(s_ramdisk, 0, RAMDISK_SIZE);
    ESP_LOGI(TAG, "RAM disk: %u KiB in %s", (unsigned)(RAMDISK_SIZE / 1024),
             esp_ptr_external_ram(s_ramdisk) ? "PSRAM" : "internal RAM");
    *out = &s_ramdisk_dev;
    return ESP_OK;
}
#endif // CONFIG_BENCH_STORAGE_RAMDISK

// ---------------------------------- SD card block device ----------------------------------

#if CONFIG_BENCH_STORAGE_SDCARD
// ESP32-P4-Function-EV-Board microSD slot, same setup as ESP-IDF examples/storage/sd_card/sdmmc
#define SD_PIN_CLK  43
#define SD_PIN_CMD  44
#define SD_PIN_D0   39
#define SD_PIN_D1   40
#define SD_PIN_D2   41
#define SD_PIN_D3   42

static sdmmc_card_t s_card;

static esp_err_t storage_init(esp_blockdev_handle_t *out)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
#if CONFIG_BENCH_SD_HIGHSPEED
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
#endif
    host.unaligned_multi_block_rw_max_chunk_size = 8;

    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = CONFIG_BENCH_SD_LDO_CHAN,
    };
    sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;
    ESP_RETURN_ON_ERROR(sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle), TAG, "SD LDO power control");
    host.pwr_ctrl_handle = pwr_ctrl_handle;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;
    slot_config.clk = SD_PIN_CLK;
    slot_config.cmd = SD_PIN_CMD;
    slot_config.d0 = SD_PIN_D0;
    slot_config.d1 = SD_PIN_D1;
    slot_config.d2 = SD_PIN_D2;
    slot_config.d3 = SD_PIN_D3;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_RETURN_ON_ERROR(sdmmc_host_init(), TAG, "SDMMC host init");
    ESP_RETURN_ON_ERROR(sdmmc_host_init_slot(host.slot, &slot_config), TAG, "SDMMC slot init");
    ESP_RETURN_ON_ERROR(sdmmc_card_init(&host, &s_card), TAG, "SD card init (card inserted?)");
    sdmmc_card_print_info(stdout, &s_card);

    return sdmmc_get_blockdev(&s_card, out);
}
#endif // CONFIG_BENCH_STORAGE_SDCARD

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
#if CONFIG_BENCH_STORAGE_SDCARD
        const int sd_khz = s_card.real_freq_khz;
#else
        const int sd_khz = 0;
#endif
        snprintf(reply, sizeof(reply), "I %s %s %d\n", INFO_STORAGE, INFO_IO_MODE, sd_khz);
    } else if (len >= 2 && buf[1] == CTRL_GET_STACK) {
#if CONFIG_TINYUSB_MSC_ASYNC_IO
        TaskHandle_t worker = xTaskGetHandle(ASYNC_IO_TASK_NAME);
        const long size = CONFIG_TINYUSB_MSC_ASYNC_IO_TASK_STACK_SIZE;
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
    esp_blockdev_handle_t storage_bdev = NULL;
    ESP_ERROR_CHECK(storage_init(&storage_bdev));

#if CONFIG_BENCH_STORAGE_RAMDISK
    // Mounting to APP first formats the empty RAM disk with FAT.
    // The storage is handed to USB automatically when the host connects.
    const tinyusb_msc_storage_config_t storage_cfg = {
        .medium.blockdev = bench_bdev_wrap(storage_bdev),
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,
        .fat_fs = {
            .base_path = "/bench",
            .config.max_files = 2,
        },
    };
#else
    // The SD card belongs to the host: never mount or format it on the device
    const tinyusb_msc_driver_config_t driver_cfg = {
        .user_flags.auto_mount_off = 1,
    };
    ESP_ERROR_CHECK(tinyusb_msc_install_driver(&driver_cfg));
    const tinyusb_msc_storage_config_t storage_cfg = {
        .medium.blockdev = bench_bdev_wrap(storage_bdev),
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
        .fat_fs = {
            .base_path = "/bench",
            .do_not_format = true,
        },
    };
#endif
    ESP_ERROR_CHECK(tinyusb_msc_new_storage_blockdev(&storage_cfg, NULL));

    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    const tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = cdc_rx_callback,
    };
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&acm_cfg));

    s_delay_ms = CONFIG_BENCH_IO_DELAY_MS;
    ESP_LOGI(TAG, "Ready. Storage: %s, MSC IO mode: %s, added storage delay: %"PRIu32" ms",
             INFO_STORAGE, INFO_IO_MODE, s_delay_ms);
}
