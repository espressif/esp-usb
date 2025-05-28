/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "soc/soc_caps.h"
#include "sdkconfig.h"
#include "msc_storage.h"


#if SOC_SDMMC_HOST_SUPPORTED

#include "diskio_impl.h"
#include "diskio_sdmmc.h"

static const char *TAG = "storage_sdmmc";

static esp_err_t storage_sdmmc_mount(void *ctx, BYTE pdrv)
{
    sdmmc_card_t *card = (sdmmc_card_t *)ctx;
    ff_diskio_register_sdmmc(pdrv, card);
    ff_sdmmc_set_disk_status_check(pdrv, false);
    return ESP_OK;
}

static esp_err_t storage_sdmmc_unmount(void *ctx)
{
    BYTE pdrv;
    sdmmc_card_t *card = (sdmmc_card_t *)ctx;
    pdrv = ff_diskio_get_pdrv_card(card);
    if (pdrv == 0xff) {
        ESP_LOGE(TAG, "Invalid state");
        return ESP_ERR_INVALID_STATE;
    }

    char drv[3] = {(char)('0' + pdrv), ':', 0};
    f_mount(0, drv, 0);
    ff_diskio_unregister(pdrv);

    return ESP_OK;
}

static inline uint32_t storage_sdmmc_get_sector_count(sdmmc_card_t *card)
{
    assert(card);
    return (uint32_t)card->csd.capacity;
}

static inline uint32_t storage_sdmmc_get_sector_size(sdmmc_card_t *card)
{
    assert(card);
    return (uint32_t)card->csd.sector_size;
}

static esp_err_t storage_sdmmc_sector_read(void *ctx,
        uint32_t lba,
        uint32_t offset,
        size_t size,
        void *dest)
{
    sdmmc_card_t *card = (sdmmc_card_t *)ctx;
    uint32_t sector_size = storage_sdmmc_get_sector_size(card);
    return sdmmc_read_sectors(card, dest, lba, size / sector_size);
}

static esp_err_t storage_sdmmc_sector_write(void *ctx,
        uint32_t lba,
        uint32_t offset,
        size_t size,
        const void *src)
{
    sdmmc_card_t *card = (sdmmc_card_t *)ctx;
    uint32_t sector_size = storage_sdmmc_get_sector_size(card);
    return sdmmc_write_sectors(card, src, lba, size / sector_size);
}

static esp_err_t storage_sdmmc_get_info(void *ctx, storage_info_t *info)
{
    ESP_RETURN_ON_FALSE(info, ESP_ERR_INVALID_ARG, TAG, "Storage info pointer can't be NULL");

    sdmmc_card_t *card = (sdmmc_card_t *)ctx;
    info->total_sectors = storage_sdmmc_get_sector_count(card);
    info->sector_size = storage_sdmmc_get_sector_size(card);
    return ESP_OK;
}

// Constant struct of function pointers
const storage_api_t sdmmc_storage_api = {
    .mount = &storage_sdmmc_mount,
    .unmount = &storage_sdmmc_unmount,
    .read = &storage_sdmmc_sector_read,
    .write = &storage_sdmmc_sector_write,
    .get_info = &storage_sdmmc_get_info,
};

esp_err_t storage_sdmmc_init(void *ctx, const storage_api_t **storage_api)
{
    sdmmc_card_t *card = (sdmmc_card_t *)ctx;
    ESP_RETURN_ON_FALSE(storage_api != NULL, ESP_ERR_INVALID_ARG, TAG, "Storage API pointer can't be NULL");
    ESP_RETURN_ON_FALSE(card, ESP_ERR_INVALID_ARG, TAG, "SDMMC card handle can't be NULL");

    *storage_api = &sdmmc_storage_api;
    return ESP_OK;
}
#endif // SOC_SDMMC_HOST_SUPPORTED
