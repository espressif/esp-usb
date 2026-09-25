/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "soc/soc_caps.h"
#include "esp_log.h"
#include "esp_check.h"
#include "tinyusb.h"
#include "sdkconfig.h"
#include "descriptors_control.h"
#include "device/usbd_pvt.h"

#if TUSB_VERSION_NUMBER < 1900 // < 0.19.0
#define tusb_deinit(x)  tusb_teardown(x)  // For compatibility with tinyusb component versions from 0.17.0~2 to 0.18.0~5
#endif

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

typedef enum {
    TINYUSB_TASK_STOPPED = 0,    // No task exists; start() is allowed
    TINYUSB_TASK_STARTING,       // Task is being created or is still initializing the stack
    TINYUSB_TASK_RUNNING,        // Task is inside the tud_task() loop; stop() is allowed
    TINYUSB_TASK_STOP_REQUESTED, // stop() was called
    TINYUSB_TASK_STOPPING,       // stop() is waiting for the task to leave tud_task()
} tinyusb_task_state_t;

// TinyUSB task context
typedef struct {
    // TinyUSB stack configuration
    uint8_t rhport;                         /*!< USB Peripheral hardware port number. Available when hardware has several available peripherals. */
    tusb_rhport_init_t rhport_init;         /*!< USB Device RH port initialization configuration pointer */
    const tinyusb_desc_config_t *desc_cfg;  /*!< USB Device descriptors configuration pointer */
    // Task related
    TaskHandle_t awaiting_handle;           /*!< Task handle, waiting to be notified after successful start of TinyUSB stack */
    TaskHandle_t task_handle;               /*!< Handle of TinyUSB task */
    SemaphoreHandle_t stopped;              /*!< Given once the task has left tud_task() and is about to delete itself */
} tinyusb_task_ctx_t;

static tinyusb_task_ctx_t *s_task_ctx;
static volatile tinyusb_task_state_t s_task_state = TINYUSB_TASK_STOPPED; /*!< State of TinyUSB task */

/**
 * @brief Wake tud_task()
 */
static void tinyusb_task_request_stop(void *param)
{
    TINYUSB_TASK_ENTER_CRITICAL();
    assert(s_task_state == TINYUSB_TASK_STOP_REQUESTED);
    s_task_state = TINYUSB_TASK_STOPPING;
    TINYUSB_TASK_EXIT_CRITICAL();
    return;
}

/**
 * @brief This top level thread processes all usb events and invokes callbacks
 */
static void tinyusb_device_task(void *arg)
{
    tinyusb_task_ctx_t *task_ctx = (tinyusb_task_ctx_t *)arg;

    // Sanity check
    assert(task_ctx != NULL);
    assert(task_ctx->awaiting_handle != NULL);
    assert(s_task_state == TINYUSB_TASK_STARTING);

    ESP_LOGD(TAG, "TinyUSB task started");

    if (tud_inited()) {
        ESP_LOGE(TAG, "TinyUSB stack is already initialized");
        goto del;
    }
    if (tinyusb_descriptors_set(task_ctx->rhport, task_ctx->desc_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB descriptors set failed");
        goto del;
    }
    if (!tusb_rhport_init(task_ctx->rhport, &task_ctx->rhport_init)) {
        ESP_LOGE(TAG, "Init TinyUSB stack failed");
        goto desc_free;
    }

    TINYUSB_TASK_ENTER_CRITICAL();
    s_task_state = TINYUSB_TASK_RUNNING;
    TINYUSB_TASK_EXIT_CRITICAL();

    task_ctx->task_handle = xTaskGetCurrentTaskHandle();
    xTaskNotifyGive(task_ctx->awaiting_handle);     // Notify parent task that TinyUSB stack was started successfully

    while (s_task_state != TINYUSB_TASK_STOPPING) {
        tud_task();
    }

    (void)tusb_deinit(task_ctx->rhport); // Always returns true

desc_free:
    tinyusb_descriptors_free();
del:
    TINYUSB_TASK_ENTER_CRITICAL();
    s_task_state = TINYUSB_TASK_STOPPED;
    TINYUSB_TASK_EXIT_CRITICAL();
    task_ctx->task_handle = NULL;
    xSemaphoreGive(task_ctx->stopped);
    vTaskDelete(NULL);
}

esp_err_t tinyusb_task_check_config(const tinyusb_task_config_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "Task configuration can't be NULL");
    ESP_RETURN_ON_FALSE(config->size != 0, ESP_ERR_INVALID_ARG, TAG, "Task size can't be 0");
    ESP_RETURN_ON_FALSE(config->priority != 0, ESP_ERR_INVALID_ARG, TAG, "Task priority can't be 0");
#if CONFIG_FREERTOS_UNICORE
    ESP_RETURN_ON_FALSE(config->xCoreID == 0, ESP_ERR_INVALID_ARG, TAG, "Task affinity must be 0 only in uniprocessor mode");
#else
    ESP_RETURN_ON_FALSE(config->xCoreID <= SOC_CPU_CORES_NUM, ESP_ERR_INVALID_ARG, TAG, "Task affinity should be less or equal to CPU amount");
#endif //
    return ESP_OK;
}

esp_err_t tinyusb_task_start(tinyusb_port_t port, const tinyusb_task_config_t *config, const tinyusb_desc_config_t *desc_cfg)
{
    ESP_RETURN_ON_ERROR(tinyusb_descriptors_check(port, desc_cfg), TAG, "TinyUSB descriptors check failed");

    TINYUSB_TASK_ENTER_CRITICAL();
    TINYUSB_TASK_CHECK_FROM_CRIT(s_task_state == TINYUSB_TASK_STOPPED, ESP_ERR_INVALID_STATE);
    s_task_state = TINYUSB_TASK_STARTING; // Prevents race conditions on tinyusb_task_start() calls
    TINYUSB_TASK_EXIT_CRITICAL();

    esp_err_t ret;

    // Allocate resources
    tinyusb_task_ctx_t *task_ctx = heap_caps_calloc(1, sizeof(tinyusb_task_ctx_t), MALLOC_CAP_DEFAULT);
    SemaphoreHandle_t stopped_sem = xSemaphoreCreateBinary();
    if (task_ctx == NULL || stopped_sem == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto clean_up;
    }

    s_task_ctx = task_ctx;
    task_ctx->stopped = stopped_sem;
    task_ctx->awaiting_handle = xTaskGetCurrentTaskHandle();    // Save parent task handle
    task_ctx->rhport = port;                                    // Peripheral port number
    task_ctx->rhport_init.role = TUSB_ROLE_DEVICE;              // Role selection: esp_tinyusb is always a device
    // Speed selection: ESP32-S31 is HS-only single-port chip
#if CONFIG_IDF_TARGET_ESP32S31
    task_ctx->rhport_init.speed = TUSB_SPEED_HIGH;
#else
    task_ctx->rhport_init.speed = (port == TINYUSB_PORT_FULL_SPEED_0) ? TUSB_SPEED_FULL : TUSB_SPEED_HIGH;
#endif
    task_ctx->desc_cfg = desc_cfg;

    TaskHandle_t task_hdl = NULL;
    ESP_LOGD(TAG, "Creating TinyUSB main task on CPU%d", config->xCoreID);
    // Create a task for tinyusb device stack
    xTaskCreatePinnedToCore(tinyusb_device_task,
                            "TinyUSB",
                            config->size,
                            (void *) task_ctx,
                            config->priority,
                            &task_hdl,
                            config->xCoreID);
    if (task_hdl == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto clean_up;
    }

    // Wait until the Task notify that port is active, 5 sec is more than enough
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000)) == 0) {
        ESP_LOGE(TAG, "Task wasn't able to start TinyUSB stack");
        // There is nothing blocking in TinyUSB start.
        // If it failed the task must be stopped
        assert(s_task_state == TINYUSB_TASK_STOPPED);
        ret = ESP_ERR_TIMEOUT;
        goto clean_up;
    }

    assert(s_task_state == TINYUSB_TASK_RUNNING);
    return ESP_OK;

clean_up:
    if (task_ctx) {
        heap_caps_free(task_ctx);
    }
    if (stopped_sem) {
        vSemaphoreDelete(stopped_sem);
    }

    s_task_state = TINYUSB_TASK_STOPPED;
    s_task_ctx = NULL;
    return ret;
}

esp_err_t tinyusb_task_stop(void)
{
    TINYUSB_TASK_ENTER_CRITICAL();
    TINYUSB_TASK_CHECK_FROM_CRIT(s_task_state == TINYUSB_TASK_RUNNING, ESP_ERR_INVALID_STATE);
    TINYUSB_TASK_CHECK_FROM_CRIT(s_task_ctx->task_handle != xTaskGetCurrentTaskHandle(), ESP_ERR_INVALID_STATE);
    s_task_state = TINYUSB_TASK_STOP_REQUESTED;
    TINYUSB_TASK_EXIT_CRITICAL();

    // This will unblock the TinyUSB task and signal it to exit
    usbd_defer_func(tinyusb_task_request_stop, NULL, false);
    if (xSemaphoreTake(s_task_ctx->stopped, pdMS_TO_TICKS(5000)) == 0) {
        return ESP_ERR_TIMEOUT;
    }

    // TinyUSB task exited and deleted itself -> clean-up
    assert(s_task_state == TINYUSB_TASK_STOPPED);
    assert(s_task_ctx->task_handle == NULL);
    vSemaphoreDelete(s_task_ctx->stopped);
    heap_caps_free(s_task_ctx);
    s_task_ctx = NULL;
    return ESP_OK;
}
