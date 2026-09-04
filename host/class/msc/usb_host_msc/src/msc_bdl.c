/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * MSC Block Device Layer adapter (ESP-IDF 6.0+).
 *
 * IDF FatFS talks to storage through diskio_bdl.c, which calls these ops.
 * msc_host_get_blockdev() is the factory that allocates the handle.
 *
 *   fopen / VFS -> FatFS -> diskio_bdl.c (IDF)
 *       -> msc_bdl_read/write -> scsi_cmd_read10/write10 -> BOT/USB
 *
 * Pre-6.0 uses diskio_usb.c instead (SCSI callbacks registered with FatFS).
 */

#include <stdbool.h>
#include <stdlib.h>
#include "esp_err.h"
#include "esp_blockdev.h"
#include "msc_common.h"
#include "usb/msc_host.h"
#include "msc_scsi_bot.h"

static esp_err_t msc_bdl_read(esp_blockdev_handle_t h, uint8_t *dst, size_t dst_size,
                              uint64_t src_addr, size_t len)
{
    if (h == NULL || dst == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return ESP_OK;
    }

    const uint32_t block_size = h->geometry.read_size;
    if (block_size == 0 || (src_addr % block_size) != 0 || (len % block_size) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > dst_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint64_t lba = src_addr / block_size;
    const uint64_t num_blocks = len / block_size;
    if (lba > UINT32_MAX || num_blocks > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    msc_device_t *dev = (msc_device_t *)h->ctx;
    return scsi_cmd_read10(dev, dst, (uint32_t)lba, (uint32_t)num_blocks, block_size);
}

/* No src_size parameter: unlike msc_bdl_read(), there is no caller-owned
 * destination buffer to bound-check here - src only needs to hold len
 * bytes for the SCSI WRITE10 payload, which is the caller's responsibility. */
static esp_err_t msc_bdl_write(esp_blockdev_handle_t h, const uint8_t *src,
                               uint64_t dst_addr, size_t len)
{
    if (h == NULL || src == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return ESP_OK;
    }

    const uint32_t block_size = h->geometry.write_size;
    if (block_size == 0 || (dst_addr % block_size) != 0 || (len % block_size) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint64_t lba = dst_addr / block_size;
    const uint64_t num_blocks = len / block_size;
    if (lba > UINT32_MAX || num_blocks > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    msc_device_t *dev = (msc_device_t *)h->ctx;
    return scsi_cmd_write10(dev, src, (uint32_t)lba, (uint32_t)num_blocks, block_size);
}

static esp_err_t msc_bdl_sync(esp_blockdev_handle_t h)
{
    (void)h;
    return ESP_OK; /* BOT/SCSI has no host-visible write cache to flush */
}

static esp_err_t msc_bdl_ioctl(esp_blockdev_handle_t h, const uint8_t cmd, void *args)
{
    (void)h;
    (void)cmd;
    (void)args;
    /* FatFS TRIM treats ESP_ERR_NOT_SUPPORTED as success. */
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t msc_bdl_release(esp_blockdev_handle_t h);

static const esp_blockdev_ops_t s_msc_bdl_ops = {
    .read = msc_bdl_read,
    .write = msc_bdl_write,
    .erase = NULL, /* USB MSC is not NOR/NAND; no erase-before-write */
    .sync = msc_bdl_sync,
    .ioctl = msc_bdl_ioctl,
    .release = msc_bdl_release,
};

/* Identity check: only handles whose ops table is s_msc_bdl_ops were
 * allocated by msc_host_get_blockdev(). Guards against a caller passing a
 * handle from a different BDL producer (or a mismatched handle1/handle2
 * pair) into an MSC-specific entry point. */
static bool msc_bdl_owns_handle(esp_blockdev_handle_t h)
{
    return h != NULL && h->ops == &s_msc_bdl_ops;
}

static esp_err_t msc_bdl_release(esp_blockdev_handle_t h)
{
    if (!msc_bdl_owns_handle(h)) {
        return ESP_ERR_INVALID_ARG;
    }
    free(h); /* does not free msc_device_t; that is owned separately */
    return ESP_OK;
}

esp_err_t msc_host_get_blockdev(msc_host_device_handle_t device, esp_blockdev_handle_t *out_handle)
{
    if (device == NULL || out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    msc_device_t *dev = (msc_device_t *)device;
    *out_handle = ESP_BLOCKDEV_HANDLE_INVALID;
    esp_blockdev_t *h = calloc(1, sizeof(esp_blockdev_t));
    if (h == NULL) {
        return ESP_ERR_NO_MEM;
    }
    h->ctx = dev;
    h->ops = &s_msc_bdl_ops;
    h->geometry = (esp_blockdev_geometry_t) {
        .disk_size = (uint64_t)dev->disk.block_count * dev->disk.block_size,
        .read_size = dev->disk.block_size,
        .write_size = dev->disk.block_size,
        .erase_size = 0,
    };
    /* Intentional zeros (calloc already did this). Do not use
     * ESP_BLOCKDEV_FLAGS_CONFIG_DEFAULT() — it sets erase_before_write. */
    h->device_flags = (esp_blockdev_flags_t) {
        .read_only = 0,               /* USB MSC is read/write */
        .encrypted = 0,               /* no BDL-level encryption */
        .erase_before_write = 0,      /* not flash; overwrite in place */
        .and_type_write = 0,          /* not NAND/NOR AND-write */
        .default_val_after_erase = 0, /* unused: erase_size is 0 */
    };
    *out_handle = h;
    return ESP_OK;
}

esp_err_t msc_host_release_blockdev(esp_blockdev_handle_t handle)
{
    if (!msc_bdl_owns_handle(handle)) {
        return ESP_ERR_INVALID_ARG;
    }
    return handle->ops->release(handle);
}
