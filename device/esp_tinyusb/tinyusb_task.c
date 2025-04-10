/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "tinyusb.h"
#include "tinyusb_types.h"
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
    // TinyUSB stack configuration
    uint8_t rhport;                         /*!< USB Peripheral hardware port number. Available when hardware has several available peripherals. */
    tusb_rhport_init_t rhport_init;         /*!< USB Device RH port initialization configuration pointer */
    // Task related
    TaskHandle_t handle;                    /*!< Task handle */
    SemaphoreHandle_t port_active_smp;      /*!< Semaphore to synchronize tasks when stack is started and port is active. */
} tinyusb_task_ctx_t;

static tinyusb_task_ctx_t *p_tusb_task_ctx = NULL;

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
        affinity = (BaseType_t)0x00;
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
    assert(arg != NULL); // Sanity check

    tinyusb_task_ctx_t *task_ctx = (tinyusb_task_ctx_t *)arg;
    if (!tusb_rhport_init(task_ctx->rhport, &task_ctx->rhport_init)) {
        ESP_LOGE(TAG, "Init TinyUSB stack failed");
        vTaskDelete(NULL);
    }
    ESP_LOGD(TAG, "TinyUSB stack has been initialized in task");
    // Give semaphore signalizing that the stack has been inited
    xSemaphoreGive(task_ctx->port_active_smp);

    while (1) { // RTOS forever loop
        tud_task();
    }
}

esp_err_t tinyusb_task_check_config(const tinyusb_task_config_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "Task configuration can't be NULL");
    ESP_RETURN_ON_FALSE(config->size != 0, ESP_ERR_INVALID_ARG, TAG, "Task size can't be 0");
    ESP_RETURN_ON_FALSE(config->priority != 0, ESP_ERR_INVALID_ARG, TAG, "Task priority can't be 0");
    ESP_RETURN_ON_FALSE(config->affinity < TINYUSB_TASK_AFFINITY_MAX, ESP_ERR_INVALID_ARG, TAG, "Task affinity is invalid");
    return ESP_OK;
}

esp_err_t tinyusb_task_start(tinyusb_port_t port, const tinyusb_task_config_t *config)
{
    TINYUSB_TASK_ENTER_CRITICAL();
    TINYUSB_TASK_CHECK_FROM_CRIT(p_tusb_task_ctx == NULL, ESP_ERR_INVALID_STATE);             // Task shouldn't started
    TINYUSB_TASK_EXIT_CRITICAL();

    esp_err_t ret;
    // Allocate TinyUSB Task object
    tinyusb_task_ctx_t *task_ctx = heap_caps_calloc(1, sizeof(tinyusb_task_ctx_t), MALLOC_CAP_DEFAULT);
    if (task_ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    task_ctx->port_active_smp = xSemaphoreCreateBinary();
    if (task_ctx->port_active_smp == NULL) {
        ESP_LOGE(TAG, "Failed to allocate task sync semaphore");
        ret = ESP_ERR_NO_MEM;
        goto err;
    }

    task_ctx->handle = NULL;                                    // TinyUSB task is not started
    task_ctx->rhport = port;                                    // Peripheral port number
    task_ctx->rhport_init.role = TUSB_ROLE_DEVICE;              // Role selection: esp_tinyusb is always a device
    task_ctx->rhport_init.speed = (port == TINYUSB_PORT_0) ? TUSB_SPEED_FULL : TUSB_SPEED_HIGH; // Speed selection

    TaskHandle_t task_hdl = NULL;
    BaseType_t affinity = tinyusb_task_get_affinity(config->affinity);
    ESP_LOGD(TAG, "Creating TinyUSB main task on CPU%d", affinity);
    // Create a task for tinyusb device stack:
    xTaskCreatePinnedToCore(tinyusb_device_task,
                            "TinyUSB",
                            config->size,
                            (void *) task_ctx,
                            config->priority,
                            &task_hdl,
                            affinity);
    if (task_hdl == NULL) {
        ESP_LOGE(TAG, "Create TinyUSB main task failed");
        ret = ESP_ERR_NOT_FINISHED;
        goto task_err;
    }

    // Wait until the port will be active, 5 sec is more than enough
    if (xSemaphoreTake(task_ctx->port_active_smp, pdMS_TO_TICKS(5000)) == pdFALSE) {
        ESP_LOGE(TAG, "Task didn't start in time");
        ret = ESP_ERR_TIMEOUT;
        goto task_err;
    }

    // Task has started
    TINYUSB_TASK_ENTER_CRITICAL();
    if (p_tusb_task_ctx != NULL) {
        TINYUSB_TASK_EXIT_CRITICAL();
        ESP_LOGE(TAG, "TinyUSB task has already started");
        ret = ESP_ERR_INVALID_STATE;
        goto assign_err;
    }
    // Save task handle
    task_ctx->handle = task_hdl;
    p_tusb_task_ctx = task_ctx;
    TINYUSB_TASK_EXIT_CRITICAL();

    return ESP_OK;

assign_err:
    vTaskDelete(task_hdl);
task_err:
    vSemaphoreDelete(task_ctx->port_active_smp);
err:
    heap_caps_free(task_ctx);
    return ret;
}

esp_err_t tinyusb_task_stop(void)
{
    TINYUSB_TASK_ENTER_CRITICAL();
    TINYUSB_TASK_CHECK_FROM_CRIT(p_tusb_task_ctx != NULL, ESP_ERR_INVALID_STATE);
    tinyusb_task_ctx_t *task_ctx = p_tusb_task_ctx;
    p_tusb_task_ctx = NULL;
    TINYUSB_TASK_EXIT_CRITICAL();

    if (task_ctx->handle != NULL) {
        vTaskDelete(task_ctx->handle);
        task_ctx->handle = NULL;
    }
    // Stop TinyUSB stack
    ESP_RETURN_ON_FALSE(tusb_rhport_teardown(task_ctx->rhport), ESP_ERR_NOT_FINISHED, TAG, "Unable to teardown TinyUSB stack");
    // Cleanup
    vSemaphoreDelete(task_ctx->port_active_smp);
    heap_caps_free(task_ctx);
    return ESP_OK;
}
