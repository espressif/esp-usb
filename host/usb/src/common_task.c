/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "common_task.h"
#include "usb_private.h"
#include "usb/usb_host.h"

#define COMMON_TASK_NAME "USB Common"

typedef struct {
    TaskHandle_t task_hdl;
    SemaphoreHandle_t mutex;
    uint32_t ref_count;
    struct {
        UBaseType_t task_priority;
        configSTACK_DEPTH_TYPE stack_size;
        BaseType_t core_id;
    } config;
} common_task_ctx_t;

static common_task_ctx_t s_common_task_ctx;
static StaticSemaphore_t s_common_task_mutex_buffer;

static bool common_task_config_is_compatible(UBaseType_t task_priority,
                                             configSTACK_DEPTH_TYPE stack_size,
                                             BaseType_t core_id)
{
    if (task_priority > s_common_task_ctx.config.task_priority) {
        return false;
    }
    if (stack_size > s_common_task_ctx.config.stack_size) {
        return false;
    }
    if (core_id != tskNO_AFFINITY && core_id != s_common_task_ctx.config.core_id) {
        return false;
    }

    return true;
}

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

esp_err_t common_task_acquire(UBaseType_t task_priority, configSTACK_DEPTH_TYPE stack_size, BaseType_t core_id)
{
    if (task_priority == 0 || stack_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

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
        // Bring the shared client-event wakeup infrastructure up before any
        // common-mode client is registered to use it.
        esp_err_t ret = usb_host_common_clients_sem_create();
        if (ret != ESP_OK) {
            xSemaphoreGive(s_common_task_ctx.mutex);
            return ret;
        }

        // Start the shared background task after the common semaphore exists so
        // the loop can block on a valid wakeup object from its first iteration.
        BaseType_t task_created = xTaskCreatePinnedToCore(common_task,
                                                          COMMON_TASK_NAME,
                                                          stack_size,
                                                          NULL,
                                                          task_priority,
                                                          &s_common_task_ctx.task_hdl,
                                                          core_id);
        if (task_created != pdPASS) {
            usb_host_common_clients_sem_delete();
            xSemaphoreGive(s_common_task_ctx.mutex);
            return ESP_ERR_NO_MEM;
        }
        s_common_task_ctx.config.task_priority = task_priority;
        s_common_task_ctx.config.stack_size = stack_size;
        s_common_task_ctx.config.core_id = core_id;
    } else if (!common_task_config_is_compatible(task_priority, stack_size, core_id)) {
        xSemaphoreGive(s_common_task_ctx.mutex);
        return ESP_ERR_INVALID_STATE;
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
        s_common_task_ctx.config.task_priority = 0;
        s_common_task_ctx.config.stack_size = 0;
        s_common_task_ctx.config.core_id = tskNO_AFFINITY;
        xSemaphoreGive(mutex);

        // Stop the shared task before deleting the common semaphore it can block on.
        if (task_hdl != NULL) {
            vTaskDelete(task_hdl);
        }

        // Tear the common wakeup infrastructure down on the last release so a
        // later opt-in recreates it from scratch.
        ESP_ERROR_CHECK(usb_host_common_clients_sem_delete());
        vSemaphoreDelete(mutex);
        return ESP_OK;
    }
    xSemaphoreGive(s_common_task_ctx.mutex);

    return ESP_OK;
}
