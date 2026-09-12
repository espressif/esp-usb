/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "diskio_bdl.h"
#include "storage_blockdev.h"

static const char *TAG = "storage_blockdev";

static esp_blockdev_handle_t _bdl_handle = ESP_BLOCKDEV_HANDLE_INVALID; // Borrowed, not owned

/* Sector bounds mirror FatFS's FF_MIN_SS/FF_MAX_SS (512/4096), hardcoded locally
 * to avoid pulling in ffconf.h. USB MSC block_size is uint16_t, so 4096 is safe. */
#define MSC_BLOCKDEV_MIN_SECTOR_SIZE  512
#define MSC_BLOCKDEV_MAX_SECTOR_SIZE  4096

// gcd_size/lcm2_size copied from components/fatfs/diskio/diskio_bdl.c
static inline size_t gcd_size(size_t a, size_t b)
{
    while (b != 0) {
        size_t t = b;
        b = a % b;
        a = t;
    }
    return a;
}

static inline size_t lcm2_size(size_t a, size_t b)
{
    return (a && b) ? (a / gcd_size(a, b)) * b : 0;
}

/**
 * Derive the logical sector size from BDL geometry, mirroring
 * compute_fs_sector_size() in diskio_bdl.c (LCM of read/write/erase size,
 * clamped to [512, 4096]).
 *
 * @return valid power-of-two sector size, or 0 if geometry is incompatible.
 */
static size_t storage_blockdev_get_sector_size(void)
{
    assert(_bdl_handle != ESP_BLOCKDEV_HANDLE_INVALID);
    const esp_blockdev_geometry_t *g = &_bdl_handle->geometry;

    size_t result = (size_t)MSC_BLOCKDEV_MIN_SECTOR_SIZE;

    if (g->read_size > 1) {
        result = lcm2_size(result, g->read_size);
    }
    if (g->write_size > 1) {
        result = lcm2_size(result, g->write_size);
    }

    if (g->erase_size > 1) {
        size_t with_erase = lcm2_size(result, g->erase_size);
        if (with_erase && with_erase <= MSC_BLOCKDEV_MAX_SECTOR_SIZE) {
            result = with_erase;
        }
    }

    if (result < MSC_BLOCKDEV_MIN_SECTOR_SIZE || result > MSC_BLOCKDEV_MAX_SECTOR_SIZE
            || (result & (result - 1)) != 0) {
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
    ESP_RETURN_ON_FALSE(info, ESP_ERR_INVALID_ARG, TAG, "Storage info pointer can't be NULL");
    assert(_bdl_handle != ESP_BLOCKDEV_HANDLE_INVALID);

    size_t sector_size = storage_blockdev_get_sector_size();
    ESP_RETURN_ON_FALSE(sector_size > 0, ESP_ERR_NOT_SUPPORTED, TAG, "Block device reports zero sector size");

    // sector_size is clamped to <=4096 so it narrows safely; the total_sectors
    // quotient can still exceed UINT32_MAX for very large blockdevs.
    uint64_t total_sectors_64 = _bdl_handle->geometry.disk_size / (uint64_t)sector_size;
    ESP_RETURN_ON_FALSE(total_sectors_64 <= UINT32_MAX, ESP_ERR_INVALID_SIZE, TAG,
                        "Block device too large: %llu sectors exceeds UINT32_MAX at sector_size %u",
                        total_sectors_64, (unsigned)sector_size);

    info->sector_size = (uint32_t)sector_size;
    info->total_sectors = (uint32_t)total_sectors_64;
    return ESP_OK;
}

static void storage_blockdev_close(void)
{
    // Borrowed handle: do NOT call ops->release() here. Lifecycle belongs to
    // whoever created the handle and passed it to storage_blockdev_open_medium().
    _bdl_handle = ESP_BLOCKDEV_HANDLE_INVALID;
}

static esp_err_t storage_blockdev_sector_read(uint32_t lba, uint32_t offset, size_t size, void *dest)
{
    assert(_bdl_handle != ESP_BLOCKDEV_HANDLE_INVALID);
    ESP_RETURN_ON_FALSE(_bdl_handle->ops->read != NULL, ESP_ERR_NOT_SUPPORTED, TAG, "Block device does not support read");

    size_t sector_size = storage_blockdev_get_sector_size();
    ESP_RETURN_ON_FALSE(sector_size > 0, ESP_ERR_NOT_SUPPORTED, TAG, "Block device geometry does not yield a valid sector size");
    uint64_t addr = (uint64_t)lba * sector_size + offset;

    ESP_RETURN_ON_FALSE(_bdl_handle->geometry.read_size > 0, ESP_ERR_NOT_SUPPORTED, TAG, "Block device has zero read_size");
    ESP_RETURN_ON_FALSE((addr % _bdl_handle->geometry.read_size) == 0, ESP_ERR_INVALID_ARG, TAG,
                        "Read address 0x%llx not aligned to read_size %u", addr, (unsigned)_bdl_handle->geometry.read_size);
    ESP_RETURN_ON_FALSE((size % _bdl_handle->geometry.read_size) == 0, ESP_ERR_INVALID_ARG, TAG,
                        "Read size %u not aligned to read_size %u", (unsigned)size, (unsigned)_bdl_handle->geometry.read_size);

    return _bdl_handle->ops->read(_bdl_handle, (uint8_t *)dest, size, addr, size);
}

static esp_err_t storage_blockdev_sector_write(uint32_t lba, uint32_t offset, size_t size, const void *src)
{
    assert(_bdl_handle != ESP_BLOCKDEV_HANDLE_INVALID);
    ESP_RETURN_ON_FALSE(_bdl_handle->ops->write != NULL, ESP_ERR_NOT_SUPPORTED, TAG, "Block device is read-only");

    size_t sector_size = storage_blockdev_get_sector_size();
    ESP_RETURN_ON_FALSE(sector_size > 0, ESP_ERR_NOT_SUPPORTED, TAG, "Block device geometry does not yield a valid sector size");
    uint64_t addr = (uint64_t)lba * sector_size + offset;

    ESP_RETURN_ON_FALSE(_bdl_handle->geometry.write_size > 0, ESP_ERR_NOT_SUPPORTED, TAG, "Block device is read-only (write_size == 0)");
    ESP_RETURN_ON_FALSE((addr % _bdl_handle->geometry.write_size) == 0, ESP_ERR_INVALID_ARG, TAG,
                        "Write address 0x%llx not aligned to write_size %u", addr, (unsigned)_bdl_handle->geometry.write_size);
    ESP_RETURN_ON_FALSE((size % _bdl_handle->geometry.write_size) == 0, ESP_ERR_INVALID_ARG, TAG,
                        "Write size %u not aligned to write_size %u", (unsigned)size, (unsigned)_bdl_handle->geometry.write_size);

    // Mirrors diskio_bdl.c's ff_bdl_write(): blockdevs do not erase-on-write
    // implicitly. Skip silently if not erase-aligned (NAND/FTL handles it).
    if (_bdl_handle->device_flags.erase_before_write || _bdl_handle->device_flags.and_type_write) {
        size_t erase_sz = _bdl_handle->geometry.erase_size;
        if (_bdl_handle->ops->erase != NULL && erase_sz > 0
                && (addr % erase_sz) == 0 && (size % erase_sz) == 0) {
            esp_err_t err = _bdl_handle->ops->erase(_bdl_handle, addr, size);
            ESP_RETURN_ON_ERROR(err, TAG, "Block device erase failed at 0x%llx len %u", addr, (unsigned)size);
        }
    }

    return _bdl_handle->ops->write(_bdl_handle, (const uint8_t *)src, addr, size);
}

// Constant struct of function pointers
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
    ESP_RETURN_ON_FALSE(bdl_handle != ESP_BLOCKDEV_HANDLE_INVALID, ESP_ERR_INVALID_ARG, TAG, "Invalid block device handle");
    ESP_RETURN_ON_FALSE(medium != NULL, ESP_ERR_INVALID_ARG, TAG, "Storage API pointer can't be NULL");
    ESP_RETURN_ON_FALSE(bdl_handle->ops != NULL && bdl_handle->ops->read != NULL && bdl_handle->ops->write != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "Block device must support read and write ops");
    // Singleton guard: only one blockdev can be open at a time, else two LUNs
    // would alias the same handle (see file-scope comment on _bdl_handle).
    ESP_RETURN_ON_FALSE(_bdl_handle == ESP_BLOCKDEV_HANDLE_INVALID, ESP_ERR_INVALID_STATE, TAG,
                        "A block-device-backed MSC storage instance is already open; only one supported at a time");

    _bdl_handle = bdl_handle;
    *medium = &blockdev_storage_medium;

    return ESP_OK;
}
