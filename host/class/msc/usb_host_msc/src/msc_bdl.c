/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * MSC Block Device Layer adapter (ESP-IDF 6.1+).
 *
 * IDF FatFS talks to storage through diskio_bdl.c, which calls these ops.
 * msc_host_get_blockdev() is the factory that allocates the handle.
 * Ops translate byte-addressed BDL I/O into SCSI.
 *
 *   fopen / VFS -> FatFS -> diskio_bdl.c (IDF)
 *       -> msc_bdl_read/write -> scsi_cmd_read10/write10 -> BOT/USB
 *
 * Pre-6.1 uses diskio_usb.c instead (SCSI callbacks registered with FatFS).
 */

#include <stdlib.h>
#include "esp_err.h"
#include "esp_blockdev.h"
#include "msc_common.h"
#include "usb/msc_host.h"
#include "msc_scsi_bot.h"

static esp_err_t msc_bdl_read(esp_blockdev_handle_t h, uint8_t *dst, size_t dst_size,
                              uint64_t src_addr, size_t len)
{
    (void)dst_size;
    msc_device_t *dev = (msc_device_t *)h->ctx;
    uint32_t block_size = h->geometry.read_size;
    /* FatFS diskio_bdl issues sector-aligned lengths matching read_size. */
    return scsi_cmd_read10(dev, dst, src_addr / block_size, len / block_size, block_size);
}

static esp_err_t msc_bdl_write(esp_blockdev_handle_t h, const uint8_t *src,
                               uint64_t dst_addr, size_t len)
{
    msc_device_t *dev = (msc_device_t *)h->ctx;
    uint32_t block_size = h->geometry.write_size;
    return scsi_cmd_write10(dev, src, dst_addr / block_size, len / block_size, block_size);
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

static esp_err_t msc_bdl_release(esp_blockdev_handle_t h)
{
    if (h == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    free(h); /* does not free msc_device_t; that is owned separately */
    return ESP_OK;
}

static const esp_blockdev_ops_t s_msc_bdl_ops = {
    .read = msc_bdl_read,
    .write = msc_bdl_write,
    .erase = NULL, /* USB MSC is not NOR/NAND; no erase-before-write */
    .sync = msc_bdl_sync,
    .ioctl = msc_bdl_ioctl,
    .release = msc_bdl_release,
};

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
    /* device_flags stay 0: writable, no erase-before-write */
    *out_handle = h;
    return ESP_OK;
}
