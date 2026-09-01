/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_pm.h"
#include "tinyusb_pm.h"
#if CONFIG_TINYUSB_USB_OTG_WAKEUP
#include "tinyusb_usb_wakeup.h"
#endif // CONFIG_TINYUSB_USB_OTG_WAKEUP

static const char *TAG = "TinyUSB-PM";

static portMUX_TYPE tinyusb_pm_spinlock = portMUX_INITIALIZER_UNLOCKED;
#define TINYUSB_PM_ENTER_CRITICAL()    portENTER_CRITICAL(&tinyusb_pm_spinlock)
#define TINYUSB_PM_EXIT_CRITICAL()     portEXIT_CRITICAL(&tinyusb_pm_spinlock)

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

#define PM_CHECK_FROM_CRIT(cond, ret_val) ({    \
            if (!(cond)) {                      \
                TINYUSB_PM_EXIT_CRITICAL();     \
                return (ret_val);               \
            }                                   \
})

/**
 * @brief TinyUSB power management context
 */
typedef struct {
    struct {
        // Dynamic members require a critical section
        bool mounted;                   /*!< USB device configured by the host; required to enter suspend PM state */
        bool suspended;                 /*!< USB Device suspended with PM lock released */
        bool lock_acquired;             /*!< PM lock currently acquired */
    } dynamic;
    struct {
        // Constant members do not change after registration thus do not require a critical section
        bool lock_enabled;              /*!< PM lock enabled/disabled by user */
        esp_pm_lock_handle_t lock;      /*!< PM lock object */
    } constant;
} tinyusb_pm_ctx_t;

static tinyusb_pm_ctx_t s_pm_ctx;

// ------------------------------------------------ PM lock management -------------------------------------------------

/**
 * @brief Sync tracked PM lock state with esp_pm_lock_get_stats()
 * @note esp_pm_lock_get_stats() is available only in IDF 6.0 and newer
 *
 * @TODO: Get stats should be preferred over the flag tracking, once the IDF 5.5 is EOL
 */
static void tinyusb_pm_lock_sync(void)
{
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    esp_pm_lock_instance_stats_t stats;

    if (s_pm_ctx.constant.lock == NULL) {
        return;
    }
    if (esp_pm_lock_get_stats(s_pm_ctx.constant.lock, &stats) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read PM lock stats for sync");
        return;
    }

    // Lock acquired status from esp_pm instance
    const bool stats_acquired = (stats.acquired != 0);

    // Lock acquired status from the pm_context
    bool pm_context_lock_acquired = false;
    TINYUSB_PM_ENTER_CRITICAL();
    pm_context_lock_acquired = s_pm_ctx.dynamic.lock_acquired;
    TINYUSB_PM_EXIT_CRITICAL();

    // Compare the esp_pm instance lock status and pm_context lock status if they are synced
    if (stats_acquired != pm_context_lock_acquired) {
        ESP_LOGW(TAG, "PM lock state mismatch: tracked=%d, stats=%d", pm_context_lock_acquired, stats_acquired);
    }
#endif // ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
}

/**
 * @brief Acquire the TinyUSB PM lock
 *
 * The spinlock is held across the flag check/update and lock acquire to prevent double-acquire of the PM lock
 *
 * @return
 *      - ESP_OK on success or if the lock is already acquired
 *      - Other error codes from called functions
 */
static esp_err_t tinyusb_pm_lock_acquire(void)
{
    esp_err_t err = ESP_OK;

    TINYUSB_PM_ENTER_CRITICAL();
    PM_CHECK_FROM_CRIT(!s_pm_ctx.dynamic.lock_acquired, ESP_OK);
    err = esp_pm_lock_acquire(s_pm_ctx.constant.lock);
    if (err == ESP_OK) {
        s_pm_ctx.dynamic.lock_acquired = true;
    }
    TINYUSB_PM_EXIT_CRITICAL();
    // Using ISR version, because the function could be called from the Light sleep exit callback's critical section
    ESP_RETURN_ON_ERROR_ISR(err, TAG, "Lock acquire error");

    tinyusb_pm_lock_sync();
    ESP_EARLY_LOGD(TAG, "PM lock acquired");
    return ESP_OK;
}

/**
 * @brief Release the TinyUSB PM lock
 *
 * The spinlock is held across the flag check/update and lock release to prevent double-release of the PM lock
 *
 * @return
 *      - ESP_OK on success or if the lock is not acquired
 *      - Other error codes from called functions
 */
static esp_err_t tinyusb_pm_lock_release(void)
{
    esp_err_t err = ESP_OK;

    TINYUSB_PM_ENTER_CRITICAL();
    PM_CHECK_FROM_CRIT(s_pm_ctx.dynamic.lock_acquired, ESP_OK);
    err = esp_pm_lock_release(s_pm_ctx.constant.lock);
    if (err == ESP_OK) {
        s_pm_ctx.dynamic.lock_acquired = false;
    }
    TINYUSB_PM_EXIT_CRITICAL();
    ESP_RETURN_ON_ERROR(err, TAG, "Lock release error");

    tinyusb_pm_lock_sync();
    ESP_LOGD(TAG, "PM lock released");
    return ESP_OK;
}

/**
 * @brief Enter USB suspend PM state and release the PM lock
 *
 * Sets `suspended` before releasing the lock so the USB wakeup enter path can
 * observe suspend state as soon as automatic light sleep becomes allowed.
 *
 * @return
 *      - ESP_OK on success or when the PM lock is not enabled
 *      - ESP_ERR_NOT_ALLOWED if the USB device is not mounted
 *      - Other error codes from lock release
 */
static esp_err_t tinyusb_pm_enter_suspend(void)
{
    // Check if the PM lock is used
    PM_CHECK(s_pm_ctx.constant.lock_enabled, ESP_OK);

    TINYUSB_PM_ENTER_CRITICAL();
    PM_CHECK_FROM_CRIT(s_pm_ctx.dynamic.mounted, ESP_ERR_NOT_ALLOWED);

    // Mark as suspended first, before releasing the lock for safety
    // Another core might query the suspended flag right after tinyusb_pm_lock_release() with the suspended flag still false
    s_pm_ctx.dynamic.suspended = true;
    TINYUSB_PM_EXIT_CRITICAL();

    // Release PM lock to allow automatic light sleep when USB peripheral is in suspended state
    const esp_err_t err = tinyusb_pm_lock_release();
    if (err != ESP_OK) {
        // Roll back to false
        TINYUSB_PM_ENTER_CRITICAL();
        s_pm_ctx.dynamic.suspended = false;
        TINYUSB_PM_EXIT_CRITICAL();
    }
    return err;
}

/**
 * @brief Leave USB suspend PM state and acquire the PM lock
 *
 * @return
 *      - ESP_OK on success or when the PM lock is not enabled
 *      - ESP_ERR_NOT_ALLOWED if the USB device is not mounted while in USB suspend PM state
 *      - Other error codes from lock acquire
 */
static esp_err_t tinyusb_pm_leave_suspend(void)
{
    // Check if the PM lock is used
    PM_CHECK(s_pm_ctx.constant.lock_enabled, ESP_OK);

    TINYUSB_PM_ENTER_CRITICAL();
    const bool in_suspend_pm = s_pm_ctx.dynamic.suspended;
    if (in_suspend_pm) {
        PM_CHECK_FROM_CRIT(s_pm_ctx.dynamic.mounted, ESP_ERR_NOT_ALLOWED);
    }
    TINYUSB_PM_EXIT_CRITICAL();

    // Acquire PM lock to restrict automatic light sleep when USB peripheral is in resumed state
    const esp_err_t err = tinyusb_pm_lock_acquire();
    if (err == ESP_OK) {
        TINYUSB_PM_ENTER_CRITICAL();
        s_pm_ctx.dynamic.suspended = false;
        TINYUSB_PM_EXIT_CRITICAL();
    }
    return err;
}

#if CONFIG_TINYUSB_USB_OTG_WAKEUP
/**
 * @brief Read the synchronized USB suspend PM state
 *
 * @return True when the device is in USB suspend PM state
 */
static bool tinyusb_pm_get_pm_state(void)
{
    TINYUSB_PM_ENTER_CRITICAL();
    const bool suspended = s_pm_ctx.dynamic.suspended;
    TINYUSB_PM_EXIT_CRITICAL();
    return suspended;
}

/**
 * @brief Re-acquire the PM lock immediately after waking from light sleep on USB activity
 *
 * @note The PM lock would have normally been acquired by the resume callback some time later,
 *       but it's necessary to acquire it immediately after waking from light sleep, to prevent light sleep re-entry
 *
 * Registered as a callback from `tinyusb_usb_wakeup.c`. Only acquires the lock
 * while the device remains in USB suspend PM state.
 *
 * @return
 *      - ESP_OK on success, when the PM lock is not enabled, or when the device is not in USB suspend PM state
 *      - Other error codes from lock acquire
 */
static esp_err_t tinyusb_pm_on_light_sleep_usb_wakeup(void)
{
    PM_CHECK(s_pm_ctx.constant.lock_enabled, ESP_OK);
    if (!tinyusb_pm_get_pm_state()) {
        return ESP_OK;
    }
    return tinyusb_pm_lock_acquire();
}
#endif // CONFIG_TINYUSB_USB_OTG_WAKEUP

// --------------------------------------------------- Public API ------------------------------------------------------

esp_err_t tinyusb_pm_init(const tinyusb_pm_config_t *config)
{
    esp_err_t ret = ESP_FAIL;
    PM_CHECK(config != NULL, ESP_ERR_INVALID_ARG);

    // Check if the user enabled the lock
    if (!config->lock_enable) {
        s_pm_ctx.constant.lock_enabled = false;
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
    ESP_RETURN_ON_FALSE(s_pm_ctx.constant.lock == NULL, ESP_ERR_INVALID_STATE, TAG, "PM module already initialized");
    ESP_RETURN_ON_ERROR(esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "usb_device", &pm_lock), TAG, "Failed to create PM lock");

    s_pm_ctx.dynamic.mounted = false;
    s_pm_ctx.dynamic.suspended = false;
    s_pm_ctx.dynamic.lock_acquired = false;
    s_pm_ctx.constant.lock_enabled = true;
    s_pm_ctx.constant.lock = pm_lock;
    ESP_GOTO_ON_ERROR(tinyusb_pm_lock_acquire(), acquire_err, TAG, "Failed to acquire PM lock on init");

    // Register callbacks to tinyusb_usb_wakeup.c
#if CONFIG_TINYUSB_USB_OTG_WAKEUP
    const tinyusb_usb_wakeup_pm_cbs_t pm_cbs = {
        .pm_state = tinyusb_pm_get_pm_state,
        .acquire_pm_lock = tinyusb_pm_on_light_sleep_usb_wakeup,
    };
    tinyusb_usb_wakeup_register_pm_cbs(&pm_cbs);
#endif // CONFIG_TINYUSB_USB_OTG_WAKEUP
    ESP_LOGI(TAG, "PM lock initialized and acquired");
    ret = ESP_OK;
    return ret;

acquire_err:
    ESP_ERROR_CHECK(esp_pm_lock_delete(pm_lock));
    s_pm_ctx.dynamic.lock_acquired = false;
    s_pm_ctx.constant.lock = NULL;
    s_pm_ctx.constant.lock_enabled = false;
    return ret;
}

esp_err_t tinyusb_pm_deinit(void)
{
    // Unregister usb wakeup callbacks
#if CONFIG_TINYUSB_USB_OTG_WAKEUP
    tinyusb_usb_wakeup_register_pm_cbs(NULL);
#endif // CONFIG_TINYUSB_USB_OTG_WAKEUP

    // Release and delete PM lock
    if (s_pm_ctx.constant.lock != NULL) {
        if (s_pm_ctx.constant.lock_enabled) {
            ESP_RETURN_ON_ERROR(tinyusb_pm_lock_release(), TAG, "Failed to release PM lock");
        }
        ESP_RETURN_ON_ERROR(esp_pm_lock_delete(s_pm_ctx.constant.lock), TAG, "Failed to delete PM lock");
        s_pm_ctx.constant.lock = NULL;
    }

    s_pm_ctx.dynamic.mounted = false;
    s_pm_ctx.dynamic.suspended = false;
    s_pm_ctx.dynamic.lock_acquired = false;
    s_pm_ctx.constant.lock_enabled = false;
    return ESP_OK;
}

void tinyusb_pm_on_event(tinyusb_event_t *event)
{
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        if (!s_pm_ctx.constant.lock_enabled) {
            break;
        }
        PM_BREAK_ON_ERROR(tinyusb_pm_lock_acquire(), "Failed to acquire PM lock on attach");
        TINYUSB_PM_ENTER_CRITICAL();
        s_pm_ctx.dynamic.mounted = true;
        TINYUSB_PM_EXIT_CRITICAL();
        break;
    case TINYUSB_EVENT_DETACHED:
        if (!s_pm_ctx.constant.lock_enabled) {
            break;
        }
        PM_BREAK_ON_ERROR(tinyusb_pm_lock_acquire(), "Failed to acquire PM lock on detach");
        TINYUSB_PM_ENTER_CRITICAL();
        s_pm_ctx.dynamic.mounted = false;
        s_pm_ctx.dynamic.suspended = false;
        TINYUSB_PM_EXIT_CRITICAL();
        break;
    case TINYUSB_EVENT_SUSPENDED:
        PM_BREAK_ON_ERROR(tinyusb_pm_enter_suspend(), "Failed to enter USB suspend PM state");
        break;
    case TINYUSB_EVENT_RESUMED:
        PM_BREAK_ON_ERROR(tinyusb_pm_leave_suspend(), "Failed to leave USB suspend PM state");
        break;
    default:
        ESP_LOGW(TAG, "Unhandled event %d", event->id);
        return;
    }
}

esp_err_t tinyusb_pm_remote_wake(void)
{
    PM_CHECK(s_pm_ctx.constant.lock_enabled, ESP_OK);
    ESP_RETURN_ON_ERROR(tinyusb_pm_lock_acquire(), TAG, "Failed to acquire PM lock on remote wake");
    return ESP_OK;
}

esp_err_t tinyusb_pm_lock_get(bool *lock_taken)
{
    PM_CHECK(lock_taken != NULL, ESP_ERR_INVALID_ARG);
    ESP_RETURN_ON_FALSE(s_pm_ctx.constant.lock_enabled && s_pm_ctx.constant.lock != NULL, ESP_ERR_INVALID_STATE, TAG, "PM lock is not enabled");

    tinyusb_pm_lock_sync();
    TINYUSB_PM_ENTER_CRITICAL();
    *lock_taken = s_pm_ctx.dynamic.lock_acquired;
    TINYUSB_PM_EXIT_CRITICAL();
    ESP_LOGD(TAG, "PM lock taken: %d", *lock_taken);
    return ESP_OK;
}
