/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "esp_blockdev.h"
#include "msc_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Open a generic esp_blockdev-backed storage medium.
 *
 * The handle is borrowed, not owned: caller keeps its lifecycle valid for as
 * long as the MSC storage instance exists.
 *
 * @param[in] bdl_handle Block device handle. Must not be ESP_BLOCKDEV_HANDLE_INVALID.
 * @param[out] medium Output pointer to the storage medium vtable.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if bdl_handle is invalid or medium is NULL
 *      - ESP_ERR_NOT_SUPPORTED if no valid sector size can be derived
 */
esp_err_t storage_blockdev_open_medium(esp_blockdev_handle_t bdl_handle, const storage_medium_t **medium);

#ifdef __cplusplus
}
#endif
