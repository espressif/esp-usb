/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <string.h>
#include "stdint.h"
#include "esp_err.h"
#include "device/usbd_pvt.h"
#include "diskio_impl.h"
#include "vfs_fat_internal.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    uint32_t total_sectors;                     /*!< Total number of sectors in the storage medium. */
    uint32_t sector_size;                       /*!< Size of a single sector in bytes. */
} storage_info_t;

typedef struct {
    esp_err_t (*mount)(void *ctx, BYTE pdrv);                                                   /*!< Storage mount function pointer. */
    esp_err_t (*unmount)(void *ctx);                                                            /*!< Storage unmount function pointer. */
    esp_err_t (*read)(void *ctx, uint32_t lba, uint32_t offset, size_t size, void *dest);       /*!< Storage read function pointer. */
    esp_err_t (*write)(void *ctx, uint32_t lba, uint32_t offset, size_t size, const void *src); /*!< Storage write function pointer. */
    esp_err_t (*get_info)(void *ctx, storage_info_t *info);           /*!< Storage get information function pointer */
} storage_api_t;

/**
 * @brief Mount the storage to the application
 *
 * This function mounts the storage to the application, allowing it to access the storage medium.
 *
 * @note This function is for the tud_mount_cb() callback and should not be called directly.
 */
void msc_storage_mount_to_app(void);

/**
 * @brief Unmount the storage from the application
 *
 * This function unmounts the storage from the application, preventing further access to the storage medium.
 *
 * @note This function is for the tud_umount_cb() callback and should not be called directly.
 */
void msc_storage_mount_to_usb(void);

#ifdef __cplusplus
}
#endif
