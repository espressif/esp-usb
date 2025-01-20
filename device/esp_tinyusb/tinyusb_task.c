/*
 * SPDX-FileCopyrightText: 2020-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_check.h"
#include "tinyusb.h"
#include "sdkconfig.h"

const static char *TAG = "tinyusb_task";

static portMUX_TYPE tusb_task_lock = portMUX_INITIALIZER_UNLOCKED;
#define TINYUSB_TASK_ENTER_CRITICAL()    portENTER_CRITICAL(&tusb_task_lock)
#define TINYUSB_TASK_EXIT_CRITICAL()     portEXIT_CRITICAL(&tusb_task_lock)

#define TINYUSB_TASK_CHECK(cond, ret_val) ({                \
    if (!(cond)) {                                          \
        return (ret_val);                                   \
    }                                                       \
})

#define TINYUSB_TASK_CHECK_FROM_CRIT(cond, ret_val) ({      \
    if (!(cond)) {                                          \
        TINYUSB_TASK_EXIT_CRITICAL();                       \
        return ret_val;                                     \
}                                                           \
})

// TinyUSB task context
typedef struct {
    TaskHandle_t handle;                    /*!< Task handle */
    EventGroupHandle_t flags;               /*!< Flags to synchronize TinyUSB driver initialization in task */
    bool init_in_task;                      /*!< TinyUSB Stack initialization place. */
    tinyusb_task_init_params_t init_params; /*!< TinyUSB Driver initialization parameters */
} tinyusb_task_ctx_t;

static tinyusb_task_ctx_t *p_tusb_task_ctx = NULL;

const static int INIT_OK = BIT0;
const static int INIT_FAILED = BIT1;

/**
 * @brief This function converts TinyUSB task affinity mask to FreeRTOS affinity mask
 *
 * @param task_affinity TinyUSB task affinity mask
 * @return BaseType_t FreeRTOS affinity mask
 */
static inline BaseType_t tinyusb_task_get_affinity(tinyusb_task_affinity_mask_t task_affinity)
{
    BaseType_t affinity;
    switch (task_affinity) {
    case TINYUSB_TASK_AFFINITY_NONE:
        affinity = CONFIG_FREERTOS_NO_AFFINITY;
        break;
    case TINYUSB_TASK_AFFINITY_CPU0:
        affinity = (BaseType_t)0x00;
        break;
#if !CONFIG_FREERTOS_UNICORE
    case TINYUSB_TASK_AFFINITY_CPU1:
        affinity = (BaseType_t)0x01;
        break;
#endif // !CONFIG_FREERTOS_UNICORE
    default:
#if CONFIG_FREERTOS_UNICORE
        affinity = CONFIG_FREERTOS_NO_AFFINITY;
#else
        // Hint: Pin to CPU1 by default on uniprocessor systems
        affinity = (BaseType_t)0x01;
#endif //  CONFIG_FREERTOS_UNICORE
        break;
    }
    return affinity;
}

/**
 * @brief This top level thread processes all usb events and invokes callbacks
 */
static void tinyusb_device_task(void *arg)
{
    ESP_LOGD(TAG, "TinyUSB task started");
    assert(p_tusb_task_ctx != NULL); // Sanity check

    if (p_tusb_task_ctx->init_in_task) {
        EventGroupHandle_t *flags = &p_tusb_task_ctx->flags;
        tinyusb_task_init_params_t *init_params = &p_tusb_task_ctx->init_params;
        if (!tusb_rhport_init(init_params->rhport, init_params->rhport_init)) {
            ESP_LOGE(TAG, "Init TinyUSB stack failed");
            xEventGroupSetBits(*flags, INIT_FAILED);
            vTaskDelete(NULL);
        }
        ESP_LOGD(TAG, "TinyUSB stack has been initialized in task");
        xEventGroupSetBits(*flags, INIT_OK);
    }

    while (1) { // RTOS forever loop
        tud_task();
    }
}

esp_err_t tinyusb_task_start(const tinyusb_task_config_t *task_cfg, const tinyusb_task_init_params_t *init_params)
{
    TINYUSB_TASK_ENTER_CRITICAL();
    TINYUSB_TASK_CHECK_FROM_CRIT(p_tusb_task_ctx == NULL, ESP_ERR_NOT_SUPPORTED);             // Task shouldn't started
    TINYUSB_TASK_EXIT_CRITICAL();
    TINYUSB_TASK_CHECK(task_cfg != NULL, ESP_ERR_INVALID_ARG);
    if (task_cfg->init_in_task) {
        TINYUSB_TASK_CHECK(init_params != NULL, ESP_ERR_INVALID_ARG);
    }

    esp_err_t ret;
    // Allocate TinyUSB Task object
    tinyusb_task_ctx_t *task_ctx = heap_caps_calloc(1, sizeof(tinyusb_task_ctx_t), MALLOC_CAP_DEFAULT);
    if (task_ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }

    task_ctx->handle = NULL;                                     // TinyUSB task is not started

    if (task_cfg->init_in_task) {
        task_ctx->flags = xEventGroupCreate();
        if (task_ctx->flags == NULL) {
            ESP_LOGE(TAG, "Failed to allocate task sync flags");
            ret = ESP_ERR_NO_MEM;
            goto err;
        }
        task_ctx->init_params.rhport = init_params->rhport;
        task_ctx->init_params.rhport_init = init_params->rhport_init;
        task_ctx->init_in_task = true;
    }

    // Task inited successfully
    TINYUSB_TASK_ENTER_CRITICAL();
    if (p_tusb_task_ctx != NULL) {
        TINYUSB_TASK_EXIT_CRITICAL();
        ret = ESP_ERR_NOT_SUPPORTED;
        goto task_err;
    }
    p_tusb_task_ctx = task_ctx;
    TINYUSB_TASK_EXIT_CRITICAL();

    TaskHandle_t task_hdl;
    BaseType_t affinity = tinyusb_task_get_affinity(task_cfg->affinity);
    ESP_LOGD(TAG, "Creating TinyUSB main task on CPU%d", affinity);
    // Create a task for tinyusb device stack:
    xTaskCreatePinnedToCore(tinyusb_device_task,
                            "TinyUSB",
                            task_cfg->size,
                            NULL,
                            task_cfg->priority,
                            &task_hdl,
                            affinity);
    if (task_hdl == NULL) {
        ESP_LOGE(TAG, "Create TinyUSB main task failed");
        ret = ESP_ERR_NOT_FINISHED;
        goto task_err;
    }

    if (task_cfg->init_in_task) {
        // Wait until tinyusb task has started and complete initialization of the driver stack
        EventBits_t flags = xEventGroupWaitBits(p_tusb_task_ctx->flags,
                                                INIT_OK | INIT_FAILED,
                                                pdFALSE, pdFALSE,
                                                portMAX_DELAY);
        vEventGroupDelete(p_tusb_task_ctx->flags);
        if (flags & INIT_FAILED) {
            ESP_LOGE(TAG, "Init TinyUSB stack in task has failed");
            ret = ESP_FAIL;
            goto err;
        }
    }
    // Task has started
    TINYUSB_TASK_ENTER_CRITICAL();
    if (p_tusb_task_ctx->handle != NULL) {
        TINYUSB_TASK_EXIT_CRITICAL();
        ESP_LOGE(TAG, "TinyUSB task has already started");
        ret = ESP_ERR_INVALID_STATE;
        goto assign_err;
    }
    p_tusb_task_ctx->handle = task_hdl;
    TINYUSB_TASK_EXIT_CRITICAL();

    return ESP_OK;

assign_err:
    vTaskDelete(task_hdl);
task_err:
    if (task_cfg->init_in_task) {
        vEventGroupDelete(p_tusb_task_ctx->flags);
    }
err:
    heap_caps_free(task_ctx);
    return ret;
}

esp_err_t tinyusb_task_stop(void)
{
    TINYUSB_TASK_ENTER_CRITICAL();
    TINYUSB_TASK_CHECK_FROM_CRIT(p_tusb_task_ctx != NULL, ESP_ERR_NOT_SUPPORTED);
    tinyusb_task_ctx_t *task_ctx = p_tusb_task_ctx;
    p_tusb_task_ctx = NULL;
    TINYUSB_TASK_EXIT_CRITICAL();

    if (task_ctx->handle != NULL) {
        vTaskDelete(task_ctx->handle);
        task_ctx->handle = NULL;
    }

    heap_caps_free(task_ctx);
    return ESP_OK;
}
