/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_log.h"
#include "esp_check.h"
#include "esp_bit_defs.h"
#include "esp_private/usb_phy.h"
#include "esp_private/sleep_event.h"
#include "tinyusb_usb_wakeup.h"
#include "tusb.h"

#include "esp_sleep.h"

static const char *TAG = "TinyUSB-WAKE";

#define USB_WAKE_CHECK(cond, ret_val) ({        \
            if (!(cond)) {                      \
                return (ret_val);               \
            }                                   \
})

// ------------------------------------------- USB Device light-sleep wakeup -------------------------------------------

typedef struct {
    tinyusb_port_t port;                        /*!< USB port used by the driver */
    bool port_configured;                       /*!< Port was configured during init */
    bool otg_prepared_for_light_sleep;          /*!< UTMI OTG suspend state was set on light sleep enter */
    tinyusb_usb_wakeup_pm_cbs_t pm_cbs;         /*!< Optional PM integration callbacks */
} tinyusb_usb_wakeup_ctx_t;

static tinyusb_usb_wakeup_ctx_t s_usb_wakeup_ctx;

/**
 * @brief Check whether the configured port supports USB light-sleep wakeup
 *
 * @param[in] port USB port used by the driver
 *
 * @return True when the port supports UTMI OTG wakeup preparation
 */
static bool port_supports_usb_wakeup(tinyusb_port_t port)
{
#if (SOC_USB_OTG_PERIPH_NUM > 1)
    return port == TINYUSB_PORT_HIGH_SPEED_0;
#else
    (void)port;
    return true;
#endif // (SOC_USB_OTG_PERIPH_NUM > 1)
}

/**
 * @brief Query whether the USB bus is suspended for light-sleep enter handling
 *
 * Uses the registered `pm_state` callback when available, otherwise falls back to
 * `tud_suspended()`.
 *
 * @return True when the bus is considered suspended
 */
static bool usb_wakeup_is_bus_suspended(void)
{
    if (s_usb_wakeup_ctx.pm_cbs.pm_state != NULL) {
        return s_usb_wakeup_ctx.pm_cbs.pm_state();
    }
    return tud_suspended();
}

/**
 * @brief Restore UTMI OTG state after light sleep or driver deinitialization
 */
static void usb_wakeup_restore_otg_state(void)
{
    if (!s_usb_wakeup_ctx.otg_prepared_for_light_sleep) {
        return;
    }

    s_usb_wakeup_ctx.otg_prepared_for_light_sleep = false;
    usb_phy_set_otg_suspend_state(false);
    usb_phy_clear_otg_wakeup_status();
    ESP_LOGD(TAG, "USB OTG light-sleep state restored");
}

/**
 * @brief Prepare UTMI OTG for USB wakeup before entering light sleep
 *
 * Only applies when the USB bus is suspended and the configured port supports wakeup.
 *
 * @param[in] user_arg Unused sleep event callback argument
 * @param[in] ext_arg  Unused sleep event callback argument
 *
 * @return
 *      - ESP_OK always; skips UTMI preparation when prerequisites are not met
 */
static esp_err_t usb_wakeup_enter_light_sleep(void *user_arg, void *ext_arg)
{
    (void)user_arg;
    (void)ext_arg;

    // UTMI PHY must not be already prepared for the light sleep
    USB_WAKE_CHECK(!s_usb_wakeup_ctx.otg_prepared_for_light_sleep, ESP_OK);
    // Port must support USB Wakeup (relevant only to P4 with 2 separate root ports)
    USB_WAKE_CHECK(s_usb_wakeup_ctx.port_configured && port_supports_usb_wakeup(s_usb_wakeup_ctx.port), ESP_OK);
    // Bus must be suspended (PM suspend state when registered, otherwise TinyUSB stack state)
    USB_WAKE_CHECK(usb_wakeup_is_bus_suspended(), ESP_OK);

    // Prepare PHY for light sleep
    usb_phy_set_otg_suspend_state(true);

    s_usb_wakeup_ctx.otg_prepared_for_light_sleep = true;
    ESP_EARLY_LOGD(TAG, "USB OTG prepared for light sleep");
    return ESP_OK;
}

/**
 * @brief Restore UTMI OTG state after exiting light sleep
 *
 * @param[in] user_arg Unused sleep event callback argument
 * @param[in] ext_arg  Unused sleep event callback argument
 *
 * @return
 *      - ESP_OK on success, when UTMI OTG was not prepared for light sleep, or when the wakeup
 *        was not caused by USB (the PM lock is intentionally left released in that case)
 *      - ESP_FAIL if the registered PM `acquire_pm_lock` callback fails
 */
static esp_err_t usb_wakeup_exit_light_sleep(void *user_arg, void *ext_arg)
{
    (void)user_arg;
    (void)ext_arg;

    // Determine the wakeup cause before touching the PHY. This exit callback runs on *every*
    // light-sleep wakeup (timer, tickless idle, UART, USB, ...), so the actual cause decides
    // whether the USB bus is being resumed by the host.
    const bool woken_by_usb = (esp_sleep_get_wakeup_causes() & BIT(ESP_SLEEP_WAKEUP_USB)) != 0;

    // UTMI PHY must be already prepared for the light sleep
    USB_WAKE_CHECK(s_usb_wakeup_ctx.otg_prepared_for_light_sleep, ESP_OK);

    // Only USB activity resumes the bus and must re-acquire the PM lock. For any other wakeup cause
    // (timer, tickless idle, UART, ...) the USB bus stays suspended and no TINYUSB_EVENT_RESUMED
    // will follow to release the lock. In that case leave the UTMI OTG suspend state armed and the
    // PM lock released so the SoC transparently re-enters automatic light sleep.

    // Crucially, do NOT touch the PHY here on non-USB wakeups. On the esp32p4 the light-sleep exit
    // sequence toggles the UTMI PCLK; un-preparing and re-preparing the OTG suspend state on every
    // tickless wakeup drives the PHY into an undefined state and forces a bus disconnect. Leaving
    // the PHY armed and untouched also keeps USB wakeup continuously enabled, so a real host resume
    // is reliably reported as ESP_SLEEP_WAKEUP_USB above.
    USB_WAKE_CHECK(woken_by_usb, ESP_OK);

    // USB resumed the bus: un-prepare the PHY and clear the wakeup latch
    usb_phy_set_otg_suspend_state(false);
    usb_phy_clear_otg_wakeup_status();

    // Clear before PM acquire so a failed acquire cannot leave enter_light_sleep permanently skipped
    s_usb_wakeup_ctx.otg_prepared_for_light_sleep = false;

    // Acquire tinyusb PM lock, if PM is used
    // It was observed, that the esp32p4 automatically re-enters light sleep after exiting it in case the PM lock
    // is not acquired immediately after exiting light sleep. The esp32p4 light sleep exit sequence takes longer than
    // on other targets and it also toggles PCLK of the UTMI PHY which forces the PHY into an undefined state
    // and forces disconnection event. Acquiring PM lock immediately after light sleep exit removes this limitation.
    if (s_usb_wakeup_ctx.pm_cbs.acquire_pm_lock != NULL) {
        USB_WAKE_CHECK(s_usb_wakeup_ctx.pm_cbs.acquire_pm_lock() == ESP_OK, ESP_FAIL);
    }
    // Using _EARLY_ variant, because we are in a light sleep critical section
    ESP_EARLY_LOGD(TAG, "USB OTG light-sleep state restored");
    return ESP_OK;
}

/**
 * @brief Callback configuration for enter light sleep event
 */
static const esp_sleep_event_cb_config_t enter_light_sleep_cb = {
    .cb = usb_wakeup_enter_light_sleep,
    .user_arg = NULL,
    .prior = 2,
    .next = NULL,
};

/**
 * @brief Callback configuration for exit light sleep event
 */
static const esp_sleep_event_cb_config_t exit_light_sleep_cb = {
    .cb = usb_wakeup_exit_light_sleep,
    .user_arg = NULL,
    .prior = 2,
    .next = NULL,
};

void tinyusb_usb_wakeup_notify_bus_resumed(void)
{
    // The bus was resumed by a device-initiated remote wakeup (the USB-wakeup exit path already
    // restores the OTG state on ESP_SLEEP_WAKEUP_USB). Clear the armed OTG suspend state and the
    // stale wakeup latch so the next light-sleep enter re-arms the PHY from a clean state.
    usb_wakeup_restore_otg_state();
}

void tinyusb_usb_wakeup_register_pm_cbs(const tinyusb_usb_wakeup_pm_cbs_t *cbs)
{
    if (cbs == NULL) {
        s_usb_wakeup_ctx.pm_cbs.pm_state = NULL;
        s_usb_wakeup_ctx.pm_cbs.acquire_pm_lock = NULL;
    } else {
        s_usb_wakeup_ctx.pm_cbs = *cbs;
    }
}

esp_err_t tinyusb_usb_wakeup_init(tinyusb_port_t port)
{
    esp_err_t ret;
    ESP_RETURN_ON_ERROR(esp_sleep_register_event_callback(SLEEP_EVENT_SW_GOTO_SLEEP, &enter_light_sleep_cb), TAG, "Failed to register goto light sleep callback");
    ESP_GOTO_ON_ERROR(esp_sleep_register_event_callback(SLEEP_EVENT_SW_EXIT_SLEEP, &exit_light_sleep_cb), err, TAG, "Failed to register exit light sleep callback");
    // Enable USB Wakeup should never fail, as long as the SOC_PM_SUPPORT_USB_WAKEUP is supported for a target
    // Since this whole file is guarded by the same SOC cap, we just defensively check for error
    ESP_ERROR_CHECK(esp_sleep_enable_usb_wakeup());

    s_usb_wakeup_ctx.port = port;
    s_usb_wakeup_ctx.port_configured = true;
    s_usb_wakeup_ctx.otg_prepared_for_light_sleep = false;

    ret = ESP_OK;
    return ret;
err:
    // This should not fail, as long as light callback function pointer is not NULL, or sleep event ID is out of range
    ESP_ERROR_CHECK(esp_sleep_unregister_event_callback(SLEEP_EVENT_SW_GOTO_SLEEP, usb_wakeup_enter_light_sleep));
    return ret;
}

esp_err_t tinyusb_usb_wakeup_deinit(void)
{
    // This should not fail, as long as light callback function pointer is not NULL, or sleep event ID is out of range
    ESP_ERROR_CHECK(esp_sleep_unregister_event_callback(SLEEP_EVENT_SW_GOTO_SLEEP, usb_wakeup_enter_light_sleep));
    ESP_ERROR_CHECK(esp_sleep_unregister_event_callback(SLEEP_EVENT_SW_EXIT_SLEEP, usb_wakeup_exit_light_sleep));
    // Disable USB Wakeup should never fail, as long as the SOC_PM_SUPPORT_USB_WAKEUP is supported for a target
    // Since this whole file is guarded by the same SOC cap, we just defensively check for error
    ESP_ERROR_CHECK(esp_sleep_disable_usb_wakeup());

    // Deregister callbacks to PM module
    tinyusb_usb_wakeup_register_pm_cbs(NULL);
    usb_wakeup_restore_otg_state();
    s_usb_wakeup_ctx.port_configured = false;
    return ESP_OK;
}
