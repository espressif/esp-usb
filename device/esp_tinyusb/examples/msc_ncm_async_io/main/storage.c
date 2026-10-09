/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

// Storage exposed over MSC: SD card or the "storage" flash partition, wrapped by a block device
// that counts the bytes read and written and adds the optional simulated medium delay.

#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "wear_levelling.h"
#include "esp_blockdev.h"
#include "example.h"
#if CONFIG_EXAMPLE_STORAGE_SDCARD
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#if CONFIG_EXAMPLE_SD_LDO_CHAN >= 0
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#endif

static const char *TAG = "storage";
#endif

static portMUX_TYPE s_io_lock = portMUX_INITIALIZER_UNLOCKED;
static io_bytes_t s_io_bytes;

io_bytes_t io_bytes_get(void)
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

esp_blockdev_handle_t storage_init(void)
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

esp_blockdev_handle_t storage_init(void)
{
    esp_blockdev_handle_t part = NULL;
    esp_blockdev_handle_t wl = NULL;
    ESP_ERROR_CHECK(esp_partition_get_blockdev(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "storage", &part));
    ESP_ERROR_CHECK(wl_get_blockdev(part, &wl));
    return slow_bdev_wrap(wl);
}
#endif // CONFIG_EXAMPLE_STORAGE_SDCARD
