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

/**
 * @brief Initialize SDMMC storage for TinyUSB MSC
 *
 * This function returns a storage API that can be used to interact with the SDMMC storage.
 *
 * @return
 *    - storage_api Pointer to the storage API structure that will be initialized.
 */
esp_err_t storage_sdmmc_init(void *ctx, const storage_api_t **storage_api);
