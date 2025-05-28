/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "sdkconfig.h"
#include "wear_levelling.h"
#include "diskio_wl.h"
#include "msc_storage.h"

static const char *TAG = "storage_spiflash";

static esp_err_t storage_spiflash_mount(void *ctx, BYTE pdrv)
{
    wl_handle_t wl_handle = (wl_handle_t)ctx;
    return ff_diskio_register_wl_partition(pdrv, wl_handle);
}

static esp_err_t storage_spiflash_unmount(void *ctx)
{
    wl_handle_t wl_handle = (wl_handle_t)ctx;
    BYTE pdrv;
    pdrv = ff_diskio_get_pdrv_wl(wl_handle);
    if (pdrv == 0xff) {
        return ESP_ERR_INVALID_STATE;
    }
    ff_diskio_clear_pdrv_wl(wl_handle);

    char drv[3] = {(char)('0' + pdrv), ':', 0};
    f_mount(0, drv, 0);
    ff_diskio_unregister(pdrv);

    return ESP_OK;
}

static size_t storage_spiflash_get_sector_count(wl_handle_t wl_handle)
{
    assert(wl_handle != WL_INVALID_HANDLE);

    size_t result = 0;
    size_t size = wl_sector_size(wl_handle);
    if (size == 0) {
        result = 0;
    } else {
        result = (size_t)(wl_size(wl_handle) / size);
    }
    return result;
}

static size_t storage_spiflash_get_sector_size(wl_handle_t wl_handle)
{
    assert(wl_handle != WL_INVALID_HANDLE);
    return (size_t)wl_sector_size(wl_handle);
}

static esp_err_t storage_spiflash_sector_read(void *ctx,
        uint32_t lba,
        uint32_t offset,
        size_t size,
        void *dest)
{
    wl_handle_t wl_handle = (wl_handle_t)ctx;
    size_t temp = 0;
    size_t addr = 0; // Address of the data to be read, relative to the beginning of the partition.
    size_t sector_size = storage_spiflash_get_sector_size(wl_handle);

    ESP_RETURN_ON_FALSE(!__builtin_umul_overflow(lba, sector_size, &temp), ESP_ERR_INVALID_SIZE, TAG, "overflow lba %lu sector_size %u", lba, sector_size);
    ESP_RETURN_ON_FALSE(!__builtin_uadd_overflow(temp, offset, &addr), ESP_ERR_INVALID_SIZE, TAG, "overflow addr %u offset %lu", temp, offset);

    return wl_read(wl_handle, addr, dest, size);
}

static esp_err_t storage_spiflash_sector_write(void *ctx,
        uint32_t lba,
        uint32_t offset,
        size_t size,
        const void *src)
{
    (void) lba;         // lba is not used in this implementation
    (void) offset;      // offset is not used in this implementation

    wl_handle_t wl_handle = (wl_handle_t)ctx;
    size_t temp = 0;
    size_t addr = 0; // Address of the data to be read, relative to the beginning of the partition.
    size_t sector_size = storage_spiflash_get_sector_size(wl_handle);

    ESP_RETURN_ON_FALSE(!__builtin_umul_overflow(lba, sector_size, &temp), ESP_ERR_INVALID_SIZE, TAG, "overflow lba %lu sector_size %u", lba, sector_size);
    ESP_RETURN_ON_FALSE(!__builtin_uadd_overflow(temp, offset, &addr), ESP_ERR_INVALID_SIZE, TAG, "overflow addr %u offset %lu", temp, offset);
    ESP_RETURN_ON_ERROR(wl_erase_range(wl_handle, addr, size), TAG, "Failed to erase");

    return wl_write(wl_handle, addr, src, size);
}

static esp_err_t storage_spiflash_get_info(void *ctx, storage_info_t *info)
{
    ESP_RETURN_ON_FALSE(info, ESP_ERR_INVALID_ARG, TAG, "Storage info pointer can't be NULL");

    wl_handle_t wl_handle = (wl_handle_t)ctx;
    info->total_sectors = storage_spiflash_get_sector_count(wl_handle);
    info->sector_size = storage_spiflash_get_sector_size(wl_handle);

    return ESP_OK;
}

// Constant struct of function pointers
const storage_api_t spiflash_storage_api = {
    .mount = &storage_spiflash_mount,
    .unmount = &storage_spiflash_unmount,
    .read = &storage_spiflash_sector_read,
    .write = &storage_spiflash_sector_write,
    .get_info = &storage_spiflash_get_info,
};

esp_err_t storage_spiflash_get(void *ctx, const storage_api_t **storage_api)
{
    wl_handle_t wl_handle = (wl_handle_t)ctx;
    ESP_RETURN_ON_FALSE(storage_api != NULL, ESP_ERR_INVALID_ARG, TAG, "Storage API pointer can't be NULL");
    ESP_RETURN_ON_FALSE(wl_handle != WL_INVALID_HANDLE, ESP_ERR_INVALID_ARG, TAG, "Invalid wear-levelling handle");

    *storage_api = &spiflash_storage_api;

    return ESP_OK;
}
