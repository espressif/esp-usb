/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <string.h>
#include "stdint.h"
#include "esp_err.h"
#include "msc_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get the storage API for SDMMC
 *
 * This function returns a storage API that can be used to interact with the SDMMC storage.
 *
 * @param[in] ctx Pointer to the storage context: For SD/MMC: Pointer to `sdmmc_card_t` structure.
 * @param[out] storage_api Pointer to the storage API.
 *
 * @return
 *    - ESP_OK: Storage API returned successfully.
 *    - ESP_ERR_INVALID_ARG: Invalid argument, ctx or storage_api is NULL.
 */
esp_err_t storage_sdmmc_get(void *ctx, const storage_api_t **storage_api);

#ifdef __cplusplus
}
#endif
