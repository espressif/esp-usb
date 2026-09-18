/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "diskio_bdl.h"
#include "storage_blockdev.h"

static const char *TAG = "storage_blockdev";

static portMUX_TYPE _bdl_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_blockdev_handle_t _bdl_handle = ESP_BLOCKDEV_HANDLE_INVALID; // borrowed
static size_t _bdl_block_size = 0;

#define MSC_BLOCKDEV_MIN_BLOCK_SIZE  512  // SCSI block_size floor (uint16_t field)

// 0/1 = no alignment constraint.
static bool storage_blockdev_size_aligned(size_t value, size_t unit)
{
    return (unit <= 1) || ((value % unit) == 0);
}

// Prefer erase_size, else max(read, write); floor 512. Returns 0 if unusable.
static size_t storage_blockdev_compute_msc_block_size(esp_blockdev_handle_t bdl_handle)
{
    const esp_blockdev_geometry_t *g = &bdl_handle->geometry;

    size_t result = (g->erase_size > 1) ? g->erase_size
                    : ((g->read_size > g->write_size) ? g->read_size : g->write_size);

    if (result < MSC_BLOCKDEV_MIN_BLOCK_SIZE) {
        result = MSC_BLOCKDEV_MIN_BLOCK_SIZE;
    }

    bool needs_erase = bdl_handle->device_flags.erase_before_write ||
                       bdl_handle->device_flags.and_type_write;
    bool ok = (result > 0) && (result <= UINT16_MAX) &&
              storage_blockdev_size_aligned(result, g->read_size) &&
              storage_blockdev_size_aligned(result, g->write_size) &&
              (!needs_erase || storage_blockdev_size_aligned(result, g->erase_size)) &&
              ((g->disk_size % (uint64_t)result) == 0);

    if (!ok) {
        ESP_LOGE(TAG, "incompatible geometry: disk=%llu read=%u write=%u erase=%u block=%u",
                 (unsigned long long)g->disk_size,
                 (unsigned)g->read_size, (unsigned)g->write_size,
                 (unsigned)g->erase_size, (unsigned)result);
        return 0;
    }

    return result;
}

static esp_err_t storage_blockdev_mount(BYTE pdrv)
{
    assert(_bdl_handle != ESP_BLOCKDEV_HANDLE_INVALID);
    return ff_diskio_register_bdl(pdrv, _bdl_handle);
}

static esp_err_t storage_blockdev_unmount(void)
{
    assert(_bdl_handle != ESP_BLOCKDEV_HANDLE_INVALID);
    BYTE pdrv = ff_diskio_get_pdrv_bdl(_bdl_handle);
    if (pdrv == 0xff) {
        return ESP_ERR_INVALID_STATE;
    }
    ff_diskio_clear_pdrv_bdl(_bdl_handle);

    char drv[3] = {(char)('0' + pdrv), ':', 0};
    f_mount(0, drv, 0);
    ff_diskio_unregister(pdrv);

    return ESP_OK;
}

static esp_err_t storage_blockdev_get_info(storage_info_t *info)
{
    ESP_RETURN_ON_FALSE(info, ESP_ERR_INVALID_ARG, TAG, "info is NULL");
    assert(_bdl_handle != ESP_BLOCKDEV_HANDLE_INVALID);
    assert(_bdl_block_size > 0);

    uint64_t total_sectors_64 = _bdl_handle->geometry.disk_size / (uint64_t)_bdl_block_size;
    ESP_RETURN_ON_FALSE(total_sectors_64 <= UINT32_MAX, ESP_ERR_INVALID_SIZE, TAG,
                        "too many sectors: %llu at block_size %u",
                        total_sectors_64, (unsigned)_bdl_block_size);

    info->sector_size = (uint32_t)_bdl_block_size;
    info->total_sectors = (uint32_t)total_sectors_64;
    return ESP_OK;
}

static void storage_blockdev_close(void)
{
    portENTER_CRITICAL(&_bdl_lock);
    _bdl_handle = ESP_BLOCKDEV_HANDLE_INVALID;
    _bdl_block_size = 0;
    portEXIT_CRITICAL(&_bdl_lock);
}

static esp_err_t storage_blockdev_sector_read(uint32_t lba, uint32_t offset, size_t size, void *dest)
{
    assert(_bdl_handle != ESP_BLOCKDEV_HANDLE_INVALID);
    assert(_bdl_block_size > 0);
    ESP_RETURN_ON_FALSE(_bdl_handle->ops->read != NULL, ESP_ERR_NOT_SUPPORTED, TAG, "read not supported");

    uint64_t addr = (uint64_t)lba * _bdl_block_size + offset;

    ESP_RETURN_ON_FALSE(_bdl_handle->geometry.read_size > 0, ESP_ERR_NOT_SUPPORTED, TAG, "read_size is 0");
    ESP_RETURN_ON_FALSE((addr % _bdl_handle->geometry.read_size) == 0, ESP_ERR_INVALID_ARG, TAG,
                        "read addr 0x%llx not aligned to %u", addr, (unsigned)_bdl_handle->geometry.read_size);
    ESP_RETURN_ON_FALSE((size % _bdl_handle->geometry.read_size) == 0, ESP_ERR_INVALID_ARG, TAG,
                        "read len %u not aligned to %u", (unsigned)size, (unsigned)_bdl_handle->geometry.read_size);

    return _bdl_handle->ops->read(_bdl_handle, (uint8_t *)dest, size, addr, size);
}

static esp_err_t storage_blockdev_sector_write(uint32_t lba, uint32_t offset, size_t size, const void *src)
{
    assert(_bdl_handle != ESP_BLOCKDEV_HANDLE_INVALID);
    assert(_bdl_block_size > 0);
    ESP_RETURN_ON_FALSE(_bdl_handle->ops->write != NULL, ESP_ERR_NOT_SUPPORTED, TAG, "write not supported");

    uint64_t addr = (uint64_t)lba * _bdl_block_size + offset;

    ESP_RETURN_ON_FALSE(_bdl_handle->geometry.write_size > 0, ESP_ERR_NOT_SUPPORTED, TAG, "write_size is 0");
    ESP_RETURN_ON_FALSE((addr % _bdl_handle->geometry.write_size) == 0, ESP_ERR_INVALID_ARG, TAG,
                        "write addr 0x%llx not aligned to %u", addr, (unsigned)_bdl_handle->geometry.write_size);
    ESP_RETURN_ON_FALSE((size % _bdl_handle->geometry.write_size) == 0, ESP_ERR_INVALID_ARG, TAG,
                        "write len %u not aligned to %u", (unsigned)size, (unsigned)_bdl_handle->geometry.write_size);

    if (_bdl_handle->device_flags.erase_before_write || _bdl_handle->device_flags.and_type_write) {
        size_t erase_sz = _bdl_handle->geometry.erase_size;
        ESP_RETURN_ON_FALSE((addr % erase_sz) == 0 && (size % erase_sz) == 0, ESP_ERR_INVALID_STATE, TAG,
                            "write 0x%llx len %u not erase-aligned to %u",
                            addr, (unsigned)size, (unsigned)erase_sz);
        esp_err_t err = _bdl_handle->ops->erase(_bdl_handle, addr, size);
        ESP_RETURN_ON_ERROR(err, TAG, "erase failed at 0x%llx len %u", addr, (unsigned)size);
    }

    return _bdl_handle->ops->write(_bdl_handle, (const uint8_t *)src, addr, size);
}

const storage_medium_t blockdev_storage_medium = {
    .type = STORAGE_MEDIUM_TYPE_BLOCKDEV,
    .mount = &storage_blockdev_mount,
    .unmount = &storage_blockdev_unmount,
    .read = &storage_blockdev_sector_read,
    .write = &storage_blockdev_sector_write,
    .get_info = &storage_blockdev_get_info,
    .close = &storage_blockdev_close,
};

esp_err_t storage_blockdev_open_medium(esp_blockdev_handle_t bdl_handle, const storage_medium_t **medium)
{
    ESP_RETURN_ON_FALSE(bdl_handle != ESP_BLOCKDEV_HANDLE_INVALID, ESP_ERR_INVALID_ARG, TAG, "invalid handle");
    ESP_RETURN_ON_FALSE(medium != NULL, ESP_ERR_INVALID_ARG, TAG, "medium is NULL");
    ESP_RETURN_ON_FALSE(bdl_handle->ops != NULL && bdl_handle->ops->read != NULL && bdl_handle->ops->write != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "read/write ops required");
    // TODO: plumb is_writable; without it a RO handle would look writable.
    ESP_RETURN_ON_FALSE(!bdl_handle->device_flags.read_only, ESP_ERR_NOT_SUPPORTED, TAG,
                        "read-only block device not supported");
    ESP_RETURN_ON_FALSE(bdl_handle->geometry.disk_size > 0 && bdl_handle->geometry.read_size > 0,
                        ESP_ERR_INVALID_ARG, TAG, "zero disk_size/read_size");

    portENTER_CRITICAL(&_bdl_lock);
    if (_bdl_handle != ESP_BLOCKDEV_HANDLE_INVALID) {
        portEXIT_CRITICAL(&_bdl_lock);
        ESP_LOGE(TAG, "blockdev MSC storage already open");
        return ESP_ERR_INVALID_STATE;
    }
    _bdl_handle = bdl_handle;
    portEXIT_CRITICAL(&_bdl_lock);

    bool needs_erase = bdl_handle->device_flags.erase_before_write || bdl_handle->device_flags.and_type_write;
    if (needs_erase && (bdl_handle->ops->erase == NULL || bdl_handle->geometry.erase_size <= 1)) {
        ESP_LOGE(TAG, "erase-before-write set but erase_size=%u erase_op=%s",
                 (unsigned)bdl_handle->geometry.erase_size,
                 bdl_handle->ops->erase ? "ok" : "NULL");
        storage_blockdev_close();
        return ESP_ERR_NOT_SUPPORTED;
    }

    size_t block_size = storage_blockdev_compute_msc_block_size(bdl_handle);
    if (block_size == 0) {
        storage_blockdev_close();
        return ESP_ERR_NOT_SUPPORTED;
    }

    _bdl_block_size = block_size;
    *medium = &blockdev_storage_medium;

    return ESP_OK;
}
