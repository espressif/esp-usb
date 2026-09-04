/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Mount USB MSC storage on VFS/FatFS.
 *
 * ESP-IDF 6.1+:
 *   MSC only supplies an esp_blockdev handle from msc_host_get_blockdev().
 *   IDF FatFS owns diskio: esp_vfs_fat_bdl_mount() -> diskio_bdl.c -> BDL ops.
 *
 *   fopen / VFS -> FatFS -> diskio_bdl.c (IDF) -> msc_bdl_read/write -> SCSI
 *
 * Pre-6.1:
 *   This component registers SCSI-backed FatFS callbacks itself
 *   (ff_diskio_register_msc in diskio_usb.c), then mounts FatFS.
 *
 *   fopen / VFS -> FatFS -> diskio_usb.c -> scsi_cmd_read10/write10
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include "msc_common.h"
#include "usb/msc_host_vfs.h"
#include "ffconf.h"
#include "ff.h"
#include "esp_idf_version.h"
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 1, 0)
#include "diskio_bdl.h"
#else
#include "diskio_impl.h"
#endif

#define DRIVE_STR_LEN 3

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 1, 0)
typedef struct msc_host_vfs {
    char *base_path;
    esp_blockdev_handle_t bdl; /* borrowed from msc_device_t; not released here */
} msc_host_vfs_t;
#else
typedef struct msc_host_vfs {
    char drive[DRIVE_STR_LEN];
    char *base_path;
    uint8_t pdrv;
} msc_host_vfs_t;
#endif

static const char *TAG = "MSC VFS";

static esp_err_t msc_format_storage(size_t block_size, size_t allocation_size, const char *drv)
{
    void *workbuf = NULL;
    const size_t workbuf_size = 4096;

    MSC_RETURN_ON_FALSE(workbuf = ff_memalloc(workbuf_size), ESP_ERR_NO_MEM);

    // Valid value of cluster size is between sector_size and 128 * sector_size.
    size_t cluster_size = MIN(MAX(allocation_size, block_size), 128 * block_size);

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    FRESULT err = f_mkfs(drv, FM_ANY | FM_SFD, cluster_size, workbuf, workbuf_size);
#else
    const MKFS_PARM opt = {(BYTE)(FM_ANY | FM_SFD), 0, 0, 0, cluster_size};
    FRESULT err = f_mkfs(drv, &opt, workbuf, workbuf_size);
#endif

    if (err) {
        ESP_LOGE(TAG, "Formatting failed with error: %d", err);
        free(workbuf);
        return ESP_ERR_MSC_FORMAT_FAILED;
    }

    free(workbuf);
    return ESP_OK;
}

esp_err_t msc_host_vfs_format(msc_host_device_handle_t device, const esp_vfs_fat_mount_config_t *mount_config, const msc_host_vfs_handle_t vfs_handle)
{
    MSC_RETURN_ON_INVALID_ARG(device);
    MSC_RETURN_ON_INVALID_ARG(mount_config);
    MSC_RETURN_ON_INVALID_ARG(vfs_handle);

    size_t block_size = ((msc_device_t *)device)->disk.block_size;
    size_t alloc_size = mount_config->allocation_unit_size;

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 1, 0)
    BYTE pdrv = ff_diskio_get_pdrv_bdl(vfs_handle->bdl);
    MSC_RETURN_ON_FALSE(pdrv != 0xff, ESP_ERR_INVALID_STATE);
    char drive[DRIVE_STR_LEN] = {(char)('0' + pdrv), ':', 0};
    return msc_format_storage(block_size, alloc_size, drive);
#else
    return msc_format_storage(block_size, alloc_size, vfs_handle->drive);
#endif
}

static void dealloc_msc_vfs(msc_host_vfs_t *vfs)
{
    free(vfs->base_path);
    free(vfs);
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 1, 0)

esp_err_t msc_host_vfs_register(msc_host_device_handle_t device,
                                const char *base_path,
                                const esp_vfs_fat_mount_config_t *mount_config,
                                msc_host_vfs_handle_t *vfs_handle)
{
    MSC_RETURN_ON_INVALID_ARG(device);
    MSC_RETURN_ON_INVALID_ARG(base_path);
    MSC_RETURN_ON_INVALID_ARG(mount_config);
    MSC_RETURN_ON_INVALID_ARG(vfs_handle);

    msc_device_t *dev = (msc_device_t *)device;
    MSC_RETURN_ON_FALSE(dev->bdl != NULL, ESP_ERR_INVALID_STATE);

    msc_host_vfs_t *vfs = calloc(1, sizeof(msc_host_vfs_t));
    MSC_RETURN_ON_FALSE(vfs != NULL, ESP_ERR_NO_MEM);

    /* IDF FatFS registers diskio_bdl and mounts; MSC only hands over the BDL handle. */
    esp_err_t err = esp_vfs_fat_bdl_mount(base_path, dev->bdl, mount_config);
    if (err != ESP_OK) {
        dealloc_msc_vfs(vfs);
        return (err == ESP_FAIL) ? ESP_ERR_MSC_MOUNT_FAILED : err;
    }

    vfs->bdl = dev->bdl;
    vfs->base_path = strdup(base_path);
    if (vfs->base_path == NULL) {
        esp_vfs_fat_bdl_unmount(base_path, dev->bdl);
        dealloc_msc_vfs(vfs);
        return ESP_ERR_NO_MEM;
    }

    *vfs_handle = vfs;
    return ESP_OK;
}

esp_err_t msc_host_vfs_unregister(msc_host_vfs_handle_t vfs_handle)
{
    MSC_RETURN_ON_INVALID_ARG(vfs_handle);
    msc_host_vfs_t *vfs = (msc_host_vfs_t *)vfs_handle;

    esp_vfs_fat_bdl_unmount(vfs->base_path, vfs->bdl);
    dealloc_msc_vfs(vfs);
    return ESP_OK;
}

#else /* ESP-IDF < 6.1: SCSI diskio callbacks live in this component */

esp_err_t msc_host_vfs_register(msc_host_device_handle_t device,
                                const char *base_path,
                                const esp_vfs_fat_mount_config_t *mount_config,
                                msc_host_vfs_handle_t *vfs_handle)
{
    MSC_RETURN_ON_INVALID_ARG(device);
    MSC_RETURN_ON_INVALID_ARG(base_path);
    MSC_RETURN_ON_INVALID_ARG(mount_config);
    MSC_RETURN_ON_INVALID_ARG(vfs_handle);

    FATFS *fs = NULL;
    BYTE pdrv;
    bool diskio_registered = false;
    esp_err_t ret = ESP_ERR_MSC_MOUNT_FAILED;
    msc_device_t *dev = (msc_device_t *)device;
    size_t block_size = dev->disk.block_size;
    size_t alloc_size = mount_config->allocation_unit_size;
    char drive[DRIVE_STR_LEN] = {0};

    msc_host_vfs_t *vfs = calloc(1, sizeof(msc_host_vfs_t));
    MSC_RETURN_ON_FALSE(vfs != NULL, ESP_ERR_NO_MEM);

    MSC_GOTO_ON_ERROR(ff_diskio_get_drive(&pdrv));

    ff_diskio_register_msc(pdrv, &dev->disk);
    drive[0] = (char)('0' + pdrv);
    drive[1] = ':';
    drive[2] = 0;
    diskio_registered = true;

    strncpy(vfs->drive, drive, DRIVE_STR_LEN);
    MSC_GOTO_ON_FALSE(vfs->base_path = strdup(base_path), ESP_ERR_NO_MEM);
    vfs->pdrv = pdrv;

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    esp_vfs_fat_conf_t conf = {
        .base_path = base_path,
        .fat_drive = drive,
        .max_files = mount_config->max_files,
    };
    MSC_GOTO_ON_ERROR(esp_vfs_fat_register_cfg(&conf, &fs));
#else
    MSC_GOTO_ON_ERROR(esp_vfs_fat_register(base_path, drive, mount_config->max_files, &fs));
#endif

    FRESULT fresult = f_mount(fs, drive, 1);

    if (fresult != FR_OK) {
        if (mount_config->format_if_mount_failed &&
                (fresult == FR_NO_FILESYSTEM || fresult == FR_INT_ERR)) {
            MSC_GOTO_ON_ERROR(msc_format_storage(block_size, alloc_size, drive));
            MSC_GOTO_ON_FALSE(f_mount(fs, drive, 0) == FR_OK, ESP_ERR_MSC_MOUNT_FAILED);
        } else {
            goto fail;
        }
    }

    *vfs_handle = vfs;
    return ESP_OK;

fail:
    if (diskio_registered) {
        ff_diskio_unregister(pdrv);
    }
    if (fs) {
        f_mount(NULL, drive, 0);
    }
    esp_vfs_fat_unregister_path(base_path);
    dealloc_msc_vfs(vfs);
    return ret;
}

esp_err_t msc_host_vfs_unregister(msc_host_vfs_handle_t vfs_handle)
{
    MSC_RETURN_ON_INVALID_ARG(vfs_handle);
    msc_host_vfs_t *vfs = (msc_host_vfs_t *)vfs_handle;

    f_mount(NULL, vfs->drive, 0);
    ff_diskio_unregister(vfs->pdrv);
    esp_vfs_fat_unregister_path(vfs->base_path);
    dealloc_msc_vfs(vfs);
    return ESP_OK;
}

#endif /* ESP_IDF_VERSION >= 6.1.0 */
