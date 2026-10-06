/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

// CDC + MSC composite device that shows how MSC storage IO blocks other USB classes.
//
// - MSC exposes an SD card, or the "storage" flash partition (FAT, wear levelled), as a USB drive.
// - A heartbeat task sends a line over CDC every 100 ms and measures the USB latency: the time from
//   queuing the line until TinyUSB reports it sent (tud_cdc_tx_complete_cb, TinyUSB task).
//   While the TinyUSB task is busy with storage IO, the latency grows by that time.
// - While the host accesses the storage, a progress line every second shows the read/write rate,
//   and a summary follows when the access ends.
// - Build with and without CONFIG_TINYUSB_MSC_ASYNC_IO and copy a file to the drive to compare.

#include <stdio.h>
#include <stdarg.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_partition.h"
#include "wear_levelling.h"
#include "esp_blockdev.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "tinyusb_cdc_acm.h"
#include "class/cdc/cdc_device.h"
#if CONFIG_EXAMPLE_STORAGE_SDCARD
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#if CONFIG_EXAMPLE_SD_LDO_CHAN >= 0
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif
#define STORAGE_STR             "SD card"
#else
#define STORAGE_STR             "internal flash"
#endif

static const char *TAG = "msc_async_io";

#if CONFIG_TINYUSB_MSC_ASYNC_IO
#define IO_MODE_STR             "ON"
#else
#define IO_MODE_STR             "OFF"
#endif

#define HEARTBEAT_PERIOD_MS     100
#define TX_TIMEOUT_MS           2000
#define STALL_THRESHOLD_US      10000   // Latency above this is marked as a stall
#define IDLE_AFTER_IO_US        1000000 // No storage IO for this long ends an activity period
#define PROGRESS_PERIOD_US      1000000

static SemaphoreHandle_t s_tx_done;

// Bytes moved by the storage, updated by the block device wrapper
typedef struct {
    uint64_t read;
    uint64_t written;
} io_bytes_t;

static portMUX_TYPE s_io_lock = portMUX_INITIALIZER_UNLOCKED;
static io_bytes_t s_io_bytes;

static io_bytes_t io_bytes_get(void)
{
    portENTER_CRITICAL(&s_io_lock);
    const io_bytes_t bytes = s_io_bytes;
    portEXIT_CRITICAL(&s_io_lock);
    return bytes;
}

// ------------------ Storage wrapper: counts IO and adds the simulated medium delay ------------------

#define INNER(dev)  ((esp_blockdev_handle_t)(dev)->ctx)

static void slow_bdev_access(size_t len, bool is_write)
{
    portENTER_CRITICAL(&s_io_lock);
    if (is_write) {
        s_io_bytes.written += len;
    } else {
        s_io_bytes.read += len;
    }
    portEXIT_CRITICAL(&s_io_lock);
#if CONFIG_EXAMPLE_MEDIUM_DELAY_MS > 0
    vTaskDelay(pdMS_TO_TICKS(CONFIG_EXAMPLE_MEDIUM_DELAY_MS));
#endif
}

static esp_err_t slow_bdev_read(esp_blockdev_handle_t dev, uint8_t *dst, size_t dst_size, uint64_t src_addr, size_t len)
{
    slow_bdev_access(len, false);
    return INNER(dev)->ops->read(INNER(dev), dst, dst_size, src_addr, len);
}

static esp_err_t slow_bdev_write(esp_blockdev_handle_t dev, const uint8_t *src, uint64_t dst_addr, size_t len)
{
    slow_bdev_access(len, true);
    return INNER(dev)->ops->write(INNER(dev), src, dst_addr, len);
}

static esp_err_t slow_bdev_erase(esp_blockdev_handle_t dev, uint64_t start_addr, size_t len)
{
    return INNER(dev)->ops->erase ? INNER(dev)->ops->erase(INNER(dev), start_addr, len) : ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t slow_bdev_sync(esp_blockdev_handle_t dev)
{
    return INNER(dev)->ops->sync ? INNER(dev)->ops->sync(INNER(dev)) : ESP_OK;
}

static esp_err_t slow_bdev_ioctl(esp_blockdev_handle_t dev, const uint8_t cmd, void *args)
{
    return INNER(dev)->ops->ioctl ? INNER(dev)->ops->ioctl(INNER(dev), cmd, args) : ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t slow_bdev_release(esp_blockdev_handle_t dev)
{
    return INNER(dev)->ops->release ? INNER(dev)->ops->release(INNER(dev)) : ESP_OK;
}

static const esp_blockdev_ops_t s_slow_bdev_ops = {
    .read = slow_bdev_read,
    .write = slow_bdev_write,
    .erase = slow_bdev_erase,
    .sync = slow_bdev_sync,
    .ioctl = slow_bdev_ioctl,
    .release = slow_bdev_release,
};

static esp_blockdev_t s_slow_bdev;

static esp_blockdev_handle_t slow_bdev_wrap(esp_blockdev_handle_t inner)
{
    s_slow_bdev.ctx = inner;
    s_slow_bdev.device_flags = inner->device_flags;
    s_slow_bdev.geometry = inner->geometry;
    s_slow_bdev.ops = &s_slow_bdev_ops;
    return &s_slow_bdev;
}

#if CONFIG_EXAMPLE_STORAGE_SDCARD
// ---------------------------------- SD card ----------------------------------

static sdmmc_card_t s_card;

static esp_blockdev_handle_t storage_init(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

#if CONFIG_EXAMPLE_SD_LDO_CHAN >= 0
    // The SD card IO is powered by an on-chip LDO (ESP32-P4-Function-EV-Board)
    const sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = CONFIG_EXAMPLE_SD_LDO_CHAN,
    };
    sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;
    ESP_ERROR_CHECK(sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle));
    host.pwr_ctrl_handle = pwr_ctrl_handle;
#endif

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;
    slot_config.clk = CONFIG_EXAMPLE_SD_PIN_CLK;
    slot_config.cmd = CONFIG_EXAMPLE_SD_PIN_CMD;
    slot_config.d0 = CONFIG_EXAMPLE_SD_PIN_D0;
    slot_config.d1 = CONFIG_EXAMPLE_SD_PIN_D1;
    slot_config.d2 = CONFIG_EXAMPLE_SD_PIN_D2;
    slot_config.d3 = CONFIG_EXAMPLE_SD_PIN_D3;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_ERROR_CHECK(sdmmc_host_init());
    ESP_ERROR_CHECK(sdmmc_host_init_slot(host.slot, &slot_config));
    esp_err_t err = sdmmc_card_init(&host, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD card init failed (%s). Is a card inserted?", esp_err_to_name(err));
        abort();
    }
    sdmmc_card_print_info(stdout, &s_card);

    esp_blockdev_handle_t card_bdev = NULL;
    ESP_ERROR_CHECK(sdmmc_get_blockdev(&s_card, &card_bdev));
    return slow_bdev_wrap(card_bdev);
}

#else
// ---------------------------------- Internal flash ----------------------------------

static esp_blockdev_handle_t storage_init(void)
{
    esp_blockdev_handle_t part = NULL;
    esp_blockdev_handle_t wl = NULL;
    ESP_ERROR_CHECK(esp_partition_get_blockdev(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "storage", &part));
    ESP_ERROR_CHECK(wl_get_blockdev(part, &wl));
    return slow_bdev_wrap(wl);
}
#endif // CONFIG_EXAMPLE_STORAGE_SDCARD

// ---------------------------------- CDC heartbeat ----------------------------------

// Called by TinyUSB (in the TinyUSB task) when a CDC IN transfer completes.
// esp_tinyusb does not use this callback, so the application can implement it.
void tud_cdc_tx_complete_cb(uint8_t itf)
{
    if (itf == TINYUSB_CDC_ACM_0) {
        xSemaphoreGive(s_tx_done);
    }
}

// Sends one line and returns the time until TinyUSB reported it sent, or -1 on timeout
static int64_t cdc_send_timed(const char *line, size_t len)
{
    xSemaphoreTake(s_tx_done, 0);   // Drop completions of earlier writes
    const int64_t start = esp_timer_get_time();
    tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (const uint8_t *)line, len);
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
    if (xSemaphoreTake(s_tx_done, pdMS_TO_TICKS(TX_TIMEOUT_MS)) != pdTRUE) {
        return -1;
    }
    return esp_timer_get_time() - start;
}

static void cdc_printf(const char *fmt, ...)
{
    char line[160];
    va_list args;
    va_start(args, fmt);
    const int len = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    cdc_send_timed(line, MIN((size_t)len, sizeof(line) - 1));
}

// USB latency statistics
typedef struct {
    uint32_t samples;
    uint32_t stalls;
    int64_t max_us;
    int64_t sum_us;
} latency_stats_t;

static void latency_stats_add(latency_stats_t *stats, int64_t latency_us)
{
    stats->samples++;
    stats->sum_us += latency_us;
    stats->max_us = MAX(stats->max_us, latency_us);
    stats->stalls += latency_us > STALL_THRESHOLD_US;
}

static double mb(uint64_t bytes)
{
    return bytes / (1024.0 * 1024.0);
}

static double mb_per_s(uint64_t bytes, int64_t us)
{
    return us > 0 ? mb(bytes) * 1000000.0 / us : 0.0;
}

// One period of storage access by the host, from the first access until IDLE_AFTER_IO_US without any
typedef struct {
    bool active;
    int64_t start_us;           // First storage access seen
    int64_t last_io_us;         // Last time the byte counters changed
    io_bytes_t start_bytes;     // Counters before the period
    io_bytes_t last_bytes;      // Counters at the last check
    int64_t progress_us;        // Last progress line
    io_bytes_t progress_bytes;  // Counters at the last progress line
    latency_stats_t latency;            // Whole period
    latency_stats_t progress_latency;   // Since the last progress line
} activity_t;

static void activity_update(activity_t *act, int64_t now)
{
    const io_bytes_t bytes = io_bytes_get();
    const bool changed = bytes.read != act->last_bytes.read || bytes.written != act->last_bytes.written;

    if (changed && !act->active) {
        *act = (activity_t) {
            .active = true,
            .start_us = now,
            .start_bytes = act->last_bytes,
            .progress_us = now,
            .progress_bytes = act->last_bytes,
        };
        cdc_printf("--- MSC activity started (async IO %s) ---\r\n", IO_MODE_STR);
    }
    if (changed) {
        act->last_io_us = now;
    }
    act->last_bytes = bytes;
    if (!act->active) {
        return;
    }

    if (now - act->progress_us >= PROGRESS_PERIOD_US) {
        const int64_t dt = now - act->progress_us;
        cdc_printf("[msc] write %5.1f MB/s  read %5.1f MB/s | total W %7.1f MB  R %7.1f MB | usb latency max %5.1f ms\r\n",
                   mb_per_s(bytes.written - act->progress_bytes.written, dt),
                   mb_per_s(bytes.read - act->progress_bytes.read, dt),
                   mb(bytes.written - act->start_bytes.written), mb(bytes.read - act->start_bytes.read),
                   act->progress_latency.max_us / 1000.0);
        act->progress_us = now;
        act->progress_bytes = bytes;
        act->progress_latency = (latency_stats_t) {
            0
        };
    }

    if (now - act->last_io_us > IDLE_AFTER_IO_US) {
        // Rates over the time storage was actually accessed, without the idle tail
        const int64_t dt = act->last_io_us - act->start_us;
        const uint64_t written = bytes.written - act->start_bytes.written;
        const uint64_t read = bytes.read - act->start_bytes.read;
        const latency_stats_t *lat = &act->latency;
        cdc_printf("--- MSC activity done (async IO %s): %.1f s, wrote %.1f MB (%.1f MB/s), read %.1f MB (%.1f MB/s) ---\r\n",
                   IO_MODE_STR, dt / 1000000.0, mb(written), mb_per_s(written, dt), mb(read), mb_per_s(read, dt));
        cdc_printf("--- usb latency max %.1f ms, avg %.1f ms, stalls >%d ms: %"PRIu32"/%"PRIu32" ---\r\n",
                   lat->max_us / 1000.0, lat->samples ? lat->sum_us / 1000.0 / lat->samples : 0.0,
                   STALL_THRESHOLD_US / 1000, lat->stalls, lat->samples);
        act->active = false;
    }
}

static void heartbeat_task(void *arg)
{
#if !CONFIG_EXAMPLE_PRINT_STALLS_ONLY
    int64_t last_latency_us = 0;
#endif
    activity_t act = { .last_bytes = io_bytes_get() };

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_PERIOD_MS));
        if (!tud_cdc_n_connected(TINYUSB_CDC_ACM_0)) {
            continue;   // No terminal open
        }

        activity_update(&act, esp_timer_get_time());

#if CONFIG_EXAMPLE_PRINT_STALLS_ONLY
        // A bare carriage return is enough to measure the latency and does not show in the terminal
        const int64_t latency_us = cdc_send_timed("\r", 1);
        if (latency_us > STALL_THRESHOLD_US) {
            cdc_printf("[async IO: %s] usb latency %6.1f ms   <-- STALL\r\n", IO_MODE_STR, latency_us / 1000.0);
        }
#else
        // The line carries the latency measured when sending the previous one
        char line[64];
        const int len = snprintf(line, sizeof(line), "[async IO: %s] usb latency %6.1f ms%s\r\n",
                                 IO_MODE_STR, last_latency_us / 1000.0,
                                 last_latency_us > STALL_THRESHOLD_US ? "   <-- STALL" : "");
        const int64_t latency_us = cdc_send_timed(line, len);
#endif
        if (latency_us < 0) {
            continue;   // Host stopped reading, e.g. terminal closed
        }
#if !CONFIG_EXAMPLE_PRINT_STALLS_ONLY
        last_latency_us = latency_us;
#endif
        if (act.active) {
            latency_stats_add(&act.latency, latency_us);
            latency_stats_add(&act.progress_latency, latency_us);
        }
    }
}

// ---------------------------------- main ----------------------------------

void app_main(void)
{
    s_tx_done = xSemaphoreCreateBinary();
    assert(s_tx_done);

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

    const tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
    };
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&acm_cfg));

    xTaskCreate(heartbeat_task, "heartbeat", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Storage: %s, async IO: %s, simulated medium delay: %d ms. "
             "Open the USB serial port and copy a file to the USB drive.",
             STORAGE_STR, IO_MODE_STR, CONFIG_EXAMPLE_MEDIUM_DELAY_MS);
}
