/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "common_task.h"
#include "usb/usb_host.h"

#define COMMON_TASK_NAME       "USB Common"
#define COMMON_TASK_STACK_SIZE 4096
#define COMMON_TASK_PRIORITY   5
#define COMMON_TASK_CORE_ID    tskNO_AFFINITY

typedef struct {
    TaskHandle_t task_hdl;
    SemaphoreHandle_t mutex;
    uint32_t ref_count;
} common_task_ctx_t;

static common_task_ctx_t s_common_task_ctx;
static StaticSemaphore_t s_common_task_mutex_buffer;

static void common_task(void *arg)
{
    (void)arg;

    // Keep the common task structured like existing driver background tasks.
    while (usb_host_common_clients_handle_events(portMAX_DELAY) == ESP_OK) {
        // TODO: Wire the first class driver path to this shared loop and then
        // widen the integration to the remaining common-task drivers.
    }

    vTaskDelete(NULL);
}

esp_err_t common_task_acquire(void)
{
    // Lazily create the common-task mutex the first time the shared manager is used.
    if (s_common_task_ctx.mutex == NULL) {
        s_common_task_ctx.mutex = xSemaphoreCreateMutexStatic(&s_common_task_mutex_buffer);
        if (s_common_task_ctx.mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    // Serialize common-task ownership changes, create the task on first use, and
    // keep a reference count for the drivers sharing it.
    xSemaphoreTake(s_common_task_ctx.mutex, portMAX_DELAY);
    if (s_common_task_ctx.task_hdl == NULL) {
        BaseType_t task_created = xTaskCreatePinnedToCore(common_task,
                                                          COMMON_TASK_NAME,
                                                          COMMON_TASK_STACK_SIZE,
                                                          NULL,
                                                          COMMON_TASK_PRIORITY,
                                                          &s_common_task_ctx.task_hdl,
                                                          COMMON_TASK_CORE_ID);
        if (task_created != pdPASS) {
            xSemaphoreGive(s_common_task_ctx.mutex);
            return ESP_ERR_NO_MEM;
        }
    }
    s_common_task_ctx.ref_count++;
    xSemaphoreGive(s_common_task_ctx.mutex);

    return ESP_OK;
}

esp_err_t common_task_release(void)
{
    if (s_common_task_ctx.mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // Drop one shared owner and tear the task down once the last owner leaves.
    xSemaphoreTake(s_common_task_ctx.mutex, portMAX_DELAY);
    if (s_common_task_ctx.ref_count == 0) {
        xSemaphoreGive(s_common_task_ctx.mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_common_task_ctx.ref_count--;
    if (s_common_task_ctx.ref_count == 0) {
        // Tear the common task and its mutex down completely so a later opt-in
        // recreates the shared manager from a clean state.
        TaskHandle_t task_hdl = s_common_task_ctx.task_hdl;
        SemaphoreHandle_t mutex = s_common_task_ctx.mutex;
        s_common_task_ctx.task_hdl = NULL;
        s_common_task_ctx.mutex = NULL;
        xSemaphoreGive(mutex);
        if (task_hdl != NULL) {
            vTaskDelete(task_hdl);
        }
        vSemaphoreDelete(mutex);
        return ESP_OK;
    }
    xSemaphoreGive(s_common_task_ctx.mutex);

    return ESP_OK;
}
