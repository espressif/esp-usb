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
#include "vfs_fat_internal.h"

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
