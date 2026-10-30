/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t common_task_acquire(void);
esp_err_t common_task_release(void);
esp_err_t common_task_register_driver(void *driver_ctx);
esp_err_t common_task_unregister_driver(void *driver_ctx);

#ifdef __cplusplus
}
#endif
