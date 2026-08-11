/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_pm.h"
#include "tinyusb_pm.h"
#if CONFIG_TINYUSB_USB_OTG_WAKEUP
#include "tinyusb_usb_wakeup.h"
#endif // CONFIG_TINYUSB_USB_OTG_WAKEUP

static const char *TAG = "TinyUSB-PM";

/**
 * Macro used in switch-case branch, to break off the case when error is returned
 */
#define PM_BREAK_ON_ERROR(x, fail_msg) do {     \
        esp_err_t err_rc_ = (x);                \
        if (unlikely(err_rc_ != ESP_OK)) {      \
            ESP_LOGW(TAG, fail_msg);            \
            break;                              \
        }                                       \
    } while (0)

#define PM_CHECK(cond, ret_val) ({              \
            if (!(cond)) {                      \
                return (ret_val);               \
            }                                   \
})

/**
 * @brief TinyUSB power management context
 *
 * Shared across the TinyUSB task (mount/suspend/resume callbacks), application
 * tasks (`tinyusb_remote_wakeup`, notify/status APIs), and light-sleep exit
 * callbacks. Access is serialized with `s_pm_spinlock`.
 */
typedef struct {
    bool mounted;                   /*!< USB device configured by the host */
    bool suspended;                 /*!< USB Device suspended with PM lock released */
    bool lock_enabled;              /*!< PM lock enabled/disabled by user */
    bool lock_acquired;             /*!< PM lock currently acquired */
    esp_pm_lock_handle_t lock;      /*!< PM lock object */
} tinyusb_pm_ctx_t;

static tinyusb_pm_ctx_t s_pm_ctx;
static portMUX_TYPE s_pm_spinlock = portMUX_INITIALIZER_UNLOCKED;

#define PM_ENTER_CRITICAL()    portENTER_CRITICAL(&s_pm_spinlock)
#define PM_EXIT_CRITICAL()     portEXIT_CRITICAL(&s_pm_spinlock)

// ------------------------------------------------ PM lock management -------------------------------------------------

/**
 * @brief Sync tracked PM lock state with esp_pm_lock_get_stats()
 * @note esp_pm_lock_get_stats() is available only in IDF 6.0 and newer
 * @note Caller must hold `s_pm_spinlock`
 */
static void tinyusb_pm_lock_sync(void)
{
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    esp_pm_lock_instance_stats_t stats;

    if (s_pm_ctx.lock == NULL) {
        return;
    }
    if (esp_pm_lock_get_stats(s_pm_ctx.lock, &stats) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read PM lock stats for sync");
        return;
    }

    const bool stats_acquired = (stats.acquired != 0);
    if (stats_acquired != s_pm_ctx.lock_acquired) {
        ESP_LOGW(TAG, "PM lock state mismatch: tracked=%d, stats=%d",
                 s_pm_ctx.lock_acquired, stats_acquired);
    }
#endif // ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
}

/**
 * @brief Acquire the TinyUSB PM lock
 *
 * @return
 *      - ESP_OK on success or if the lock is already acquired
 *      - Other error codes from called functions
 */
static esp_err_t tinyusb_pm_lock_acquire(void)
{
    PM_ENTER_CRITICAL();
    if (s_pm_ctx.lock_acquired) {
        PM_EXIT_CRITICAL();
        return ESP_OK;
    }
    tinyusb_pm_lock_sync();

    esp_err_t err = esp_pm_lock_acquire(s_pm_ctx.lock);
    if (err != ESP_OK) {
        PM_EXIT_CRITICAL();
        ESP_LOGE(TAG, "Lock acquire error");
        return err;
    }
    s_pm_ctx.lock_acquired = true;
    PM_EXIT_CRITICAL();
    ESP_EARLY_LOGD(TAG, "PM lock acquired");
    return ESP_OK;
}

/**
 * @brief Release the TinyUSB PM lock
 *
 * @return
 *      - ESP_OK on success or if the lock is not acquired
 *      - Other error codes from called functions
 */
static esp_err_t tinyusb_pm_lock_release(void)
{
    PM_ENTER_CRITICAL();
    if (!s_pm_ctx.lock_acquired) {
        PM_EXIT_CRITICAL();
        return ESP_OK;
    }
    tinyusb_pm_lock_sync();

    esp_err_t err = esp_pm_lock_release(s_pm_ctx.lock);
    if (err != ESP_OK) {
        PM_EXIT_CRITICAL();
        ESP_LOGE(TAG, "Lock release error");
        return err;
    }
    s_pm_ctx.lock_acquired = false;
    PM_EXIT_CRITICAL();
    ESP_LOGD(TAG, "PM lock released");
    return ESP_OK;
}

/**
 * @brief Enter USB suspend PM state: release lock and mark suspended
 *
 * Lock release and `suspended` are updated under the same critical section so a
 * concurrent USB-wakeup exit callback cannot observe a released lock with
 * `suspended == false` and skip re-acquiring.
 *
 * @return
 *      - ESP_OK on success
 *      - Other error codes from lock release
 */
static esp_err_t tinyusb_pm_enter_suspend(void)
{
    PM_ENTER_CRITICAL();
    if (!s_pm_ctx.lock_enabled) {
        PM_EXIT_CRITICAL();
        return ESP_OK;
    }

    if (s_pm_ctx.lock_acquired) {
        tinyusb_pm_lock_sync();
        esp_err_t err = esp_pm_lock_release(s_pm_ctx.lock);
        if (err != ESP_OK) {
            PM_EXIT_CRITICAL();
            ESP_LOGE(TAG, "Lock release error");
            return err;
        }
        s_pm_ctx.lock_acquired = false;
    }
    s_pm_ctx.suspended = true;
    PM_EXIT_CRITICAL();
    ESP_LOGD(TAG, "PM lock released");
    return ESP_OK;
}

/**
 * @brief Leave USB suspend PM state: acquire lock and clear suspended
 *
 * @return
 *      - ESP_OK on success
 *      - Other error codes from lock acquire
 */
static esp_err_t tinyusb_pm_leave_suspend(void)
{
    PM_ENTER_CRITICAL();
    if (!s_pm_ctx.lock_enabled) {
        PM_EXIT_CRITICAL();
        return ESP_OK;
    }

    if (!s_pm_ctx.lock_acquired) {
        tinyusb_pm_lock_sync();
        esp_err_t err = esp_pm_lock_acquire(s_pm_ctx.lock);
        if (err != ESP_OK) {
            PM_EXIT_CRITICAL();
            ESP_LOGE(TAG, "Lock acquire error");
            return err;
        }
        s_pm_ctx.lock_acquired = true;
    }
    s_pm_ctx.suspended = false;
    PM_EXIT_CRITICAL();
    ESP_EARLY_LOGD(TAG, "PM lock acquired");
    return ESP_OK;
}

#if CONFIG_TINYUSB_USB_OTG_WAKEUP
static esp_err_t tinyusb_pm_on_light_sleep_usb_wakeup(void)
{
    // Re-acquire under the same critical section as the suspended check so we cannot
    // race a concurrent enter/leave suspend transition.
    PM_ENTER_CRITICAL();
    if (!s_pm_ctx.lock_enabled || !s_pm_ctx.suspended) {
        PM_EXIT_CRITICAL();
        return ESP_OK;
    }
    if (!s_pm_ctx.lock_acquired) {
        tinyusb_pm_lock_sync();
        esp_err_t err = esp_pm_lock_acquire(s_pm_ctx.lock);
        if (err != ESP_OK) {
            PM_EXIT_CRITICAL();
            ESP_LOGE(TAG, "Lock acquire error");
            return err;
        }
        s_pm_ctx.lock_acquired = true;
    }
    PM_EXIT_CRITICAL();
    ESP_EARLY_LOGD(TAG, "PM lock acquired after USB wakeup");
    return ESP_OK;
}
#endif // CONFIG_TINYUSB_USB_OTG_WAKEUP

// --------------------------------------------------- Public API ------------------------------------------------------

esp_err_t tinyusb_pm_init(const tinyusb_pm_config_t *config)
{
    esp_err_t ret = ESP_FAIL;
    PM_CHECK(config != NULL, ESP_ERR_INVALID_ARG);

    // Check if the user enabled the lock
    if (!config->lock_enable) {
        PM_ENTER_CRITICAL();
        s_pm_ctx.lock_enabled = false;
        PM_EXIT_CRITICAL();
        ESP_LOGD(TAG, "PM lock disabled in configuration");
        return ESP_OK;
    }

    // Check if PM has been initialized and automatic light sleep has been enabled by user
    esp_pm_config_t pm_config;
    if (esp_pm_get_configuration(&pm_config) == ESP_OK && !pm_config.light_sleep_enable) {
        ESP_LOGW(TAG, "pm_lock_enable is set but automatic light sleep is disabled. "
                 "Configure esp_pm_configure() with light_sleep_enable=true to allow automatic light sleep "
                 "when the USB PM lock is released");
    }

    // Initialize and acquire the lock
    esp_pm_lock_handle_t pm_lock;
    PM_ENTER_CRITICAL();
    const bool already_init = (s_pm_ctx.lock != NULL);
    PM_EXIT_CRITICAL();
    ESP_RETURN_ON_FALSE(!already_init, ESP_ERR_INVALID_STATE, TAG, "PM module already initialized");
    ESP_RETURN_ON_ERROR(esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "usb_device", &pm_lock), TAG, "Failed to create PM lock");

    PM_ENTER_CRITICAL();
    s_pm_ctx.mounted = false;
    s_pm_ctx.suspended = false;
    s_pm_ctx.lock_acquired = false;
    s_pm_ctx.lock_enabled = true;
    s_pm_ctx.lock = pm_lock;
    PM_EXIT_CRITICAL();
    ESP_GOTO_ON_ERROR(tinyusb_pm_lock_acquire(), acquire_err, TAG, "Failed to acquire PM lock on init");
#if CONFIG_TINYUSB_USB_OTG_WAKEUP
    ESP_GOTO_ON_ERROR(tinyusb_usb_wakeup_register_resume_cb(tinyusb_pm_on_light_sleep_usb_wakeup), acquire_err, TAG,
                      "Failed to register USB wakeup resume callback");
#endif // CONFIG_TINYUSB_USB_OTG_WAKEUP
    ESP_LOGI(TAG, "PM lock initialized and acquired");
    ret = ESP_OK;
    return ret;

acquire_err:
    ESP_ERROR_CHECK(esp_pm_lock_delete(pm_lock));
    PM_ENTER_CRITICAL();
    s_pm_ctx.lock = NULL;
    s_pm_ctx.lock_acquired = false;
    s_pm_ctx.lock_enabled = false;
    PM_EXIT_CRITICAL();
    return ret;
}

esp_err_t tinyusb_pm_deinit(void)
{
#if CONFIG_TINYUSB_USB_OTG_WAKEUP
    tinyusb_usb_wakeup_register_resume_cb(NULL);
#endif // CONFIG_TINYUSB_USB_OTG_WAKEUP

    PM_ENTER_CRITICAL();
    esp_pm_lock_handle_t lock = s_pm_ctx.lock;
    const bool lock_enabled = s_pm_ctx.lock_enabled;
    PM_EXIT_CRITICAL();

    if (lock != NULL) {
        if (lock_enabled) {
            ESP_RETURN_ON_ERROR(tinyusb_pm_lock_release(), TAG, "Failed to release PM lock");
        }
        ESP_RETURN_ON_ERROR(esp_pm_lock_delete(lock), TAG, "Failed to delete PM lock");
        PM_ENTER_CRITICAL();
        s_pm_ctx.lock = NULL;
        PM_EXIT_CRITICAL();
    }

    PM_ENTER_CRITICAL();
    s_pm_ctx.mounted = false;
    s_pm_ctx.suspended = false;
    s_pm_ctx.lock_acquired = false;
    s_pm_ctx.lock_enabled = false;
    PM_EXIT_CRITICAL();
    return ESP_OK;
}

void tinyusb_pm_on_event(tinyusb_event_t *event)
{
    if (event == NULL) {
        return;
    }

    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        if (!s_pm_ctx.lock_enabled) {
            break;
        }
        PM_BREAK_ON_ERROR(tinyusb_pm_lock_acquire(), "Failed to acquire PM lock on attach");
        PM_ENTER_CRITICAL();
        s_pm_ctx.mounted = true;
        PM_EXIT_CRITICAL();
        break;
    case TINYUSB_EVENT_DETACHED:
        if (!s_pm_ctx.lock_enabled) {
            break;
        }
        PM_ENTER_CRITICAL();
        s_pm_ctx.mounted = false;
        s_pm_ctx.suspended = false;
        PM_EXIT_CRITICAL();
        PM_BREAK_ON_ERROR(tinyusb_pm_lock_acquire(), "Failed to acquire PM lock on detach");
        break;
#ifdef CONFIG_TINYUSB_SUSPEND_CALLBACK
    case TINYUSB_EVENT_SUSPENDED:
        PM_BREAK_ON_ERROR(tinyusb_pm_enter_suspend(), "Failed to enter USB suspend PM state");
        break;
#endif // CONFIG_TINYUSB_SUSPEND_CALLBACK
#ifdef CONFIG_TINYUSB_RESUME_CALLBACK
    case TINYUSB_EVENT_RESUMED:
        PM_BREAK_ON_ERROR(tinyusb_pm_leave_suspend(), "Failed to leave USB suspend PM state");
        break;
#endif // CONFIG_TINYUSB_RESUME_CALLBACK
    default:
        ESP_LOGW(TAG, "Unhandled event %d", event->id);
        return;
    }
}

esp_err_t tinyusb_pm_remote_wake(void)
{
    // No-op when the lock was not enabled at install time so remote wakeup can
    // still be used with CONFIG_TINYUSB_PM compiled in.
    if (!s_pm_ctx.lock_enabled) {
        return ESP_OK;
    }
    return tinyusb_pm_lock_acquire();
}

esp_err_t tinyusb_pm_lock_get(bool *lock_taken)
{
    PM_CHECK(lock_taken != NULL, ESP_ERR_INVALID_ARG);
    ESP_RETURN_ON_FALSE(s_pm_ctx.lock_enabled && s_pm_ctx.lock != NULL, ESP_ERR_INVALID_STATE, TAG, "PM lock is not enabled");

    PM_ENTER_CRITICAL();
    tinyusb_pm_lock_sync();
    *lock_taken = s_pm_ctx.lock_acquired;
    PM_EXIT_CRITICAL();
    ESP_LOGD(TAG, "PM lock taken: %d", *lock_taken);
    return ESP_OK;
}

#if !CONFIG_TINYUSB_SUSPEND_CALLBACK
esp_err_t tinyusb_pm_on_suspend(tinyusb_port_t port)
{
    (void)port;
    ESP_RETURN_ON_FALSE(s_pm_ctx.lock_enabled, ESP_ERR_NOT_ALLOWED, TAG, "PM lock is not enabled");
    PM_ENTER_CRITICAL();
    const bool suspended = s_pm_ctx.suspended;
    const bool lock_acquired = s_pm_ctx.lock_acquired;
    const bool mounted = s_pm_ctx.mounted;
    PM_EXIT_CRITICAL();
    ESP_LOGD(TAG, "User suspend notify (mounted=%d, suspended=%d)", mounted, suspended);

    // Already suspended with the lock released: idempotent success.
    if (suspended && !lock_acquired) {
        return ESP_OK;
    }

    return tinyusb_pm_enter_suspend();
}
#endif // !CONFIG_TINYUSB_SUSPEND_CALLBACK

#if !CONFIG_TINYUSB_RESUME_CALLBACK
esp_err_t tinyusb_pm_on_resume(tinyusb_port_t port)
{
    (void)port;
    ESP_RETURN_ON_FALSE(s_pm_ctx.lock_enabled, ESP_ERR_NOT_ALLOWED, TAG, "PM lock is not enabled");
    PM_ENTER_CRITICAL();
    const bool suspended = s_pm_ctx.suspended;
    const bool mounted = s_pm_ctx.mounted;
    PM_EXIT_CRITICAL();
    ESP_LOGD(TAG, "User resume notify (mounted=%d, suspended=%d)", mounted, suspended);

    PM_CHECK(suspended, ESP_OK);

    return tinyusb_pm_leave_suspend();
}
#endif // !CONFIG_TINYUSB_RESUME_CALLBACK
