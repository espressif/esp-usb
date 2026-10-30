/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "common_task.h"

typedef struct {
    TaskHandle_t task_hdl;
    SemaphoreHandle_t mutex;
    uint32_t ref_count;
    uint32_t num_registered_drivers;
} common_task_ctx_t;

static common_task_ctx_t s_common_task_ctx;

esp_err_t common_task_acquire(void)
{
    (void)s_common_task_ctx;
    // TODO: wire common-task lifecycle.
    return ESP_OK;
}

esp_err_t common_task_release(void)
{
    // TODO: wire common-task lifecycle.
    return ESP_OK;
}

esp_err_t common_task_register_driver(void *driver_ctx)
{
    (void)driver_ctx;
    // TODO: wire shared-driver registration.
    return ESP_OK;
}

esp_err_t common_task_unregister_driver(void *driver_ctx)
{
    (void)driver_ctx;
    // TODO: wire shared-driver deregistration.
    return ESP_OK;
}
