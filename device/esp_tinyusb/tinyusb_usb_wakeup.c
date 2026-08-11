/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_log.h"
#include "esp_check.h"
#include "esp_private/usb_phy.h"
#include "esp_private/sleep_event.h"
#include "tusb.h"
#include "tinyusb_usb_wakeup.h"

#include "esp_sleep.h"

static const char *TAG = "TinyUSB-WAKE";

#define USB_WAKE_CHECK(cond, ret_val) ({        \
            if (!(cond)) {                      \
                return (ret_val);               \
            }                                   \
})

// ------------------------------------------- USB Device light-sleep wakeup -------------------------------------------

typedef struct {
    tinyusb_port_t port;                /*!< USB port used by the driver */
    bool port_configured;               /*!< Port was configured during init */
    bool otg_prepared_for_light_sleep;  /*!< UTMI OTG suspend state was set on light sleep enter */
    tinyusb_usb_wakeup_resume_cb_t resume_cb; /*!< Optional callback invoked on USB wakeup exit */
} tinyusb_usb_wakeup_ctx_t;

static tinyusb_usb_wakeup_ctx_t s_usb_wakeup_ctx;

static bool port_supports_usb_wakeup(tinyusb_port_t port)
{
#if (SOC_USB_OTG_PERIPH_NUM > 1)
    return port == TINYUSB_PORT_HIGH_SPEED_0;
#else
    (void)port;
    return true;
#endif // (SOC_USB_OTG_PERIPH_NUM > 1)
}

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
 */
static esp_err_t usb_wakeup_enter_light_sleep(void *user_arg, void *ext_arg)
{
    (void)user_arg;
    (void)ext_arg;

    USB_WAKE_CHECK(!s_usb_wakeup_ctx.otg_prepared_for_light_sleep, ESP_OK);
    USB_WAKE_CHECK(s_usb_wakeup_ctx.port_configured && port_supports_usb_wakeup(s_usb_wakeup_ctx.port), ESP_OK);
    // Do not force UTMI OTG suspend while the bus is still active (manual light sleep,
    // sleep-reject retries, or races with resume).
    USB_WAKE_CHECK(tud_suspended(), ESP_OK);

    usb_phy_set_otg_suspend_state(true);

    s_usb_wakeup_ctx.otg_prepared_for_light_sleep = true;
    ESP_EARLY_LOGD(TAG, "USB OTG prepared for light sleep");
    return ESP_OK;
}

/**
 * @brief Restore UTMI OTG state after exiting light sleep
 *
 * Always restores OTG state when it was prepared on enter. Sleep can be rejected after
 * `SLEEP_EVENT_SW_GOTO_SLEEP`, or woken by a non-USB source (timer/GPIO remote wakeup).
 * Leaving the PHY in software OTG suspend breaks subsequent remote wakeup and bus activity.
 *
 * The optional resume callback (PM lock re-acquire) runs only for USB wakeup causes.
 */
static esp_err_t usb_wakeup_exit_light_sleep(void *user_arg, void *ext_arg)
{
    (void)user_arg;
    (void)ext_arg;

    USB_WAKE_CHECK(s_usb_wakeup_ctx.otg_prepared_for_light_sleep, ESP_OK);

    s_usb_wakeup_ctx.otg_prepared_for_light_sleep = false;
    usb_phy_set_otg_suspend_state(false);
    usb_phy_clear_otg_wakeup_status();

    const uint32_t causes = esp_sleep_get_wakeup_causes();
    if ((causes & BIT(ESP_SLEEP_WAKEUP_USB)) && s_usb_wakeup_ctx.resume_cb != NULL) {
        USB_WAKE_CHECK(s_usb_wakeup_ctx.resume_cb() == ESP_OK, ESP_FAIL);
    }

    ESP_EARLY_LOGD(TAG, "USB OTG light-sleep state restored");
    return ESP_OK;
}

static const esp_sleep_event_cb_config_t enter_light_sleep_cb = {
    .cb = usb_wakeup_enter_light_sleep,
    .user_arg = NULL,
    .prior = 2,
    .next = NULL,
};

static const esp_sleep_event_cb_config_t exit_light_sleep_cb = {
    .cb = usb_wakeup_exit_light_sleep,
    .user_arg = NULL,
    .prior = 2,
    .next = NULL,
};

esp_err_t tinyusb_usb_wakeup_register_resume_cb(tinyusb_usb_wakeup_resume_cb_t cb)
{
    s_usb_wakeup_ctx.resume_cb = cb;
    return ESP_OK;
}

esp_err_t tinyusb_usb_wakeup_init(tinyusb_port_t port)
{
    ESP_RETURN_ON_ERROR(esp_sleep_register_event_callback(SLEEP_EVENT_SW_GOTO_SLEEP, &enter_light_sleep_cb), TAG, "Failed to register goto light sleep callback");
    ESP_RETURN_ON_ERROR(esp_sleep_register_event_callback(SLEEP_EVENT_SW_EXIT_SLEEP, &exit_light_sleep_cb), TAG, "Failed to register exit light sleep callback");

    s_usb_wakeup_ctx.port = port;
    s_usb_wakeup_ctx.port_configured = true;
    s_usb_wakeup_ctx.otg_prepared_for_light_sleep = false;
    return ESP_OK;
}

esp_err_t tinyusb_usb_wakeup_deinit(void)
{
    ESP_RETURN_ON_ERROR(esp_sleep_unregister_event_callback(SLEEP_EVENT_SW_GOTO_SLEEP, usb_wakeup_enter_light_sleep), TAG, "Failed to unregister goto light sleep callback");
    ESP_RETURN_ON_ERROR(esp_sleep_unregister_event_callback(SLEEP_EVENT_SW_EXIT_SLEEP, usb_wakeup_exit_light_sleep), TAG, "Failed to unregister exit light sleep callback");

    s_usb_wakeup_ctx.resume_cb = NULL;
    usb_wakeup_restore_otg_state();
    s_usb_wakeup_ctx.port_configured = false;
    return ESP_OK;
}
