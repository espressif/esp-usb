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

// Bounded wait per tud_task_ext() queue receive: lets the loop re-check stop_requested
// instead of blocking indefinitely on TinyUSB's event queue.
#define TINYUSB_TASK_STOP_POLL_MS       50
// Max time tinyusb_task_stop() waits for the task to quiesce before forcing deletion.
#define TINYUSB_TASK_STOP_TIMEOUT_MS    2000

// TinyUSB task context
typedef struct {
    // TinyUSB stack configuration
    uint8_t rhport;                         /*!< USB Peripheral hardware port number. Available when hardware has several available peripherals. */
    tusb_rhport_init_t rhport_init;         /*!< USB Device RH port initialization configuration pointer */
    const tinyusb_desc_config_t *desc_cfg;  /*!< USB Device descriptors configuration pointer */
    // Task related
    TaskHandle_t handle;                    /*!< Task handle */
    volatile TaskHandle_t awaiting_handle;           /*!< Task handle, waiting to be notified after successful start of TinyUSB stack */
    volatile bool stop_requested;           /*!< Cooperative stop flag, checked between tud_task_ext() iterations */
    SemaphoreHandle_t stopped_sem;          /*!< Given by the task once it has left its loop and holds no storage locks; tinyusb_task_stop() then deletes it */
} tinyusb_task_ctx_t;

static bool _task_is_running = false;               // Locking flag for the task, access only from the critical section
static tinyusb_task_ctx_t *p_tusb_task_ctx = NULL;  // TinyUSB task context

/**
 * @brief This top level thread processes all usb events and invokes callbacks
 */
static void tinyusb_device_task(void *arg)
{
    tinyusb_task_ctx_t *task_ctx = (tinyusb_task_ctx_t *)arg;

    // Sanity check
    assert(task_ctx != NULL);
    assert(task_ctx->awaiting_handle != NULL);

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
    task_ctx->handle = xTaskGetCurrentTaskHandle(); // Save task handle
    p_tusb_task_ctx = task_ctx;                     // Save global task context pointer
    TINYUSB_TASK_EXIT_CRITICAL();

    xTaskNotifyGive(task_ctx->awaiting_handle);     // Notify parent task that TinyUSB stack was started successfully

    while (!task_ctx->stop_requested) { // RTOS forever loop, until a graceful stop is requested
        // Bounded wait so we can re-check stop_requested even with no pending USB events.
        // Loop only breaks between iterations, once tud_task_ext() released any storage
        // locks it took - so this task never gets deleted mid-transfer.
        tud_task_ext(TINYUSB_TASK_STOP_POLL_MS, false);
    }
    // Hand deletion off to tinyusb_task_stop() instead of self-deleting: if we deleted
    // ourselves right as its wait timed out, it could vTaskDelete() an already-dead handle.
    // Blocking forever keeps the handle valid until the caller deletes us.
    // _task_is_running is cleared by tinyusb_task_stop() once teardown is complete.
    xSemaphoreGive(task_ctx->stopped_sem); // Tell tinyusb_task_stop() it is now safe to delete us
    vTaskSuspend(NULL);
    // Unreachable: tinyusb_task_stop() deletes this task before it could ever be resumed.

desc_free:
    tinyusb_descriptors_free();
del:
    TINYUSB_TASK_ENTER_CRITICAL();
    _task_is_running = false;       // Task is not running anymore
    TINYUSB_TASK_EXIT_CRITICAL();
    vTaskDelete(NULL);
    // No return needed here: vTaskDelete(NULL) does not return
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
    TINYUSB_TASK_CHECK_FROM_CRIT(p_tusb_task_ctx == NULL, ESP_ERR_INVALID_STATE);     // Task shouldn't started
    TINYUSB_TASK_CHECK_FROM_CRIT(!_task_is_running, ESP_ERR_INVALID_STATE);           // Task shouldn't be running
    // Cleared by the task if stack init fails, at err: if the task was never created,
    // otherwise by tinyusb_task_stop() once teardown is complete
    _task_is_running = true;
    TINYUSB_TASK_EXIT_CRITICAL();

    esp_err_t ret;
    bool task_created = false;
    tinyusb_task_ctx_t *task_ctx = heap_caps_calloc(1, sizeof(tinyusb_task_ctx_t), MALLOC_CAP_DEFAULT);
    if (task_ctx == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto err;
    }

    task_ctx->awaiting_handle = xTaskGetCurrentTaskHandle();    // Save parent task handle
    task_ctx->handle = NULL;                                    // TinyUSB task is not started
    task_ctx->stop_requested = false;
    task_ctx->stopped_sem = xSemaphoreCreateBinary();
    if (task_ctx->stopped_sem == NULL) {
        ESP_LOGE(TAG, "Failed to create TinyUSB task stop semaphore");
        ret = ESP_ERR_NO_MEM;
        goto err;
    }
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
        ESP_LOGE(TAG, "Create TinyUSB main task failed");
        ret = ESP_ERR_NOT_FINISHED;
        goto err;
    }
    task_created = true;

    // Wait until the Task notify that port is active, 5 sec is more than enough
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000)) == 0) {
        ESP_LOGE(TAG, "Task wasn't able to start TinyUSB stack");
        ret = ESP_ERR_TIMEOUT;
        goto err;
    }

    return ESP_OK;

err:
    if (task_ctx != NULL) {
        if (task_ctx->stopped_sem) {
            vSemaphoreDelete(task_ctx->stopped_sem);
        }
        heap_caps_free(task_ctx);
    }
    if (!task_created) {
        // No task exists to clear the flag, so release it here
        TINYUSB_TASK_ENTER_CRITICAL();
        _task_is_running = false;
        TINYUSB_TASK_EXIT_CRITICAL();
    }
    return ret;
}

esp_err_t tinyusb_task_stop(void)
{
    TINYUSB_TASK_ENTER_CRITICAL();
    TINYUSB_TASK_CHECK_FROM_CRIT(p_tusb_task_ctx != NULL, ESP_ERR_INVALID_STATE);
    // Must not be called from the TinyUSB task itself (e.g. from a device or MSC event
    // callback): it would wait on stopped_sem that only it can give, then delete itself
    TINYUSB_TASK_CHECK_FROM_CRIT(p_tusb_task_ctx->handle != xTaskGetCurrentTaskHandle(), ESP_ERR_INVALID_STATE);
    tinyusb_task_ctx_t *task_ctx = p_tusb_task_ctx;
    p_tusb_task_ctx = NULL;
    task_ctx->stop_requested = true; // Ask the task to exit its loop on its own, between iterations
    TINYUSB_TASK_EXIT_CRITICAL();

    if (task_ctx->handle != NULL) {
        // Detach from the host so it stops sending traffic. TinyUSB < 0.21 has no
        // CFG_TUD_TASK_EVENTS_PER_RUN limit: tud_task_ext() only returns once the event
        // queue has been empty for the whole poll timeout, so under continuous traffic
        // the task would never re-check stop_requested.
        tud_disconnect();
        // Wait for the task to reach its safe suspend point (no storage mux_lock held).
        if (xSemaphoreTake(task_ctx->stopped_sem, pdMS_TO_TICKS(TINYUSB_TASK_STOP_TIMEOUT_MS)) == pdTRUE) {
            // Sole owner of this task's deletion; it never self-deletes past its loop, so the
            // handle can't go stale underneath us.
            vTaskDelete(task_ctx->handle);
            vSemaphoreDelete(task_ctx->stopped_sem);
        } else {
            // Didn't quiesce in time (e.g. wedged hardware). Delete it anyway so callers are
            // never blocked forever, but loudly: any storage mux_lock it held is now orphaned.
            ESP_LOGE(TAG, "TinyUSB task did not stop gracefully within %d ms, forcing deletion "
                     "(storage locks may be left orphaned)", TINYUSB_TASK_STOP_TIMEOUT_MS);
            vTaskDelete(task_ctx->handle);
            // Do NOT delete stopped_sem here: the task may still be racing to give it right as
            // our wait timed out (vTaskDelete() on another core does not guarantee the victim
            // has stopped executing before returning). Leaking one binary semaphore in this
            // already-exceptional path is preferable to a use-after-free on the semaphore.
        }
        task_ctx->handle = NULL;
    }

    // Free descriptors
    tinyusb_descriptors_free();
    // Stop TinyUSB stack
    esp_err_t ret = tusb_deinit(task_ctx->rhport) ? ESP_OK : ESP_ERR_NOT_FINISHED;
    // Cleanup
    heap_caps_free(task_ctx);

    // Allow a new tinyusb_task_start() only after teardown has finished
    TINYUSB_TASK_ENTER_CRITICAL();
    _task_is_running = false;
    TINYUSB_TASK_EXIT_CRITICAL();

    ESP_RETURN_ON_ERROR(ret, TAG, "Unable to teardown TinyUSB stack");
    return ESP_OK;
}
