/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "driver/gpio.h"
#include "tinyusb_vbus_monitor.h"

const static char *TAG = "VBUS mon";

// This value is used for checking Timer Task Stack Size when MSC is enabled
// When value is changed, also update the README.md documentation
#define VBUS_MON_TIMER_TASK_STACK_MIN    2304

#if (CONFIG_IDF_TARGET_ESP32P4)
// On ESP32-P4 USB OTG 2.0 signals are not wired to GPIO matrix
// So we need to override the Bvalid signal from PHY
#include "soc/usb_dwc_struct.h"         // For GOTGCTL (when supported) and DCTL registers access
#define USB_DWC_REG                     USB_DWC_HS

#if (CONFIG_ESP32P4_REV_MIN_300)
// On ESP32-P4 ECO5, BVALID override is not supported
#define BVALID_OVERRIDE_SUPPORT_ENABLE  0
#else
#define BVALID_OVERRIDE_SUPPORT_ENABLE  1
#endif // CONFIG_ESP32P4_REV_MIN_300
#else
#error "Software-based VBUS monitoring functionality is intended exclusively for ESP32-P4, USB OTG 2.0"
#endif // CONFIG_IDF_TARGET_ESP32P4

/**
 * @brief VBUS monitoring context
 *
 * Note: This is a single-threaded implementation, so only one instance of VBUS monitoring is supported
 */
typedef struct {
    gpio_num_t gpio_num;            /*!< GPIO number used for VBUS monitoring */
    bool prev_state;                /*!< Previous VBUS IO state: true - HIGH, false - LOW */
    TimerHandle_t debounce_timer;   /*!< Debounce timer handle */
    vbus_mon_state_changed_t state_changed_cb;  /*!< Callback function called when VBUS state changes */
} vbus_monitor_context_t;


static vbus_monitor_context_t _vbus_ctx = {
    .gpio_num = GPIO_NUM_NC,
    .prev_state = false,
    .debounce_timer = NULL,
    .state_changed_cb = NULL,
};

//
// Additional low-level USB DWC functions, which are not present in the IDF USB DWC HAL
//

#if (BVALID_OVERRIDE_SUPPORT_ENABLE)
// --------------- GOTGCTL register ------------------

static void usb_dwc_ll_gotgctl_set_bvalid_override_value(usb_dwc_dev_t *hw, uint8_t value)
{
    hw->gotgctl_reg.bvalidovval = value;
}

static void usb_dwc_ll_gotgctl_enable_bvalid_override(usb_dwc_dev_t *hw, bool enable)
{
    hw->gotgctl_reg.bvalidoven = enable ? 1 : 0;
}
#endif // BVALID_OVERRIDE_SUPPORT_ENABLE

// ------------------ DCTL register --------------------

static void usb_dwc_ll_dctl_set_soft_disconnect(usb_dwc_dev_t *hw, bool enable)
{
    hw->dctl_reg.sftdiscon = enable ? 1 : 0;
}

// -------------- VBUS Internal Logic ------------------

/**
 * @brief Handle VBUS appeared event
 */
static void vbus_appeared(void)
{
    ESP_LOGD(TAG, "Appeared");
#if (BVALID_OVERRIDE_SUPPORT_ENABLE)
    usb_dwc_ll_gotgctl_set_bvalid_override_value(&USB_DWC_REG, 1);
#else
    // Workaround for ESP32-P4 ECO5, USB-OTG peripheral v4.30a
    // tud_mount_cb() will be called after SetConfiguration()
    // So we do not need to notify the upper logic about the connection event manually
#endif // BVALID_OVERRIDE_SUPPORT_ENABLE
    usb_dwc_ll_dctl_set_soft_disconnect(&USB_DWC_REG, false);
}

/**
 * @brief Handle VBUS disappeared event
 */
static void vbus_disappeared(void)
{
    ESP_LOGD(TAG, "Disappeared");
    usb_dwc_ll_dctl_set_soft_disconnect(&USB_DWC_REG, true);
#if (BVALID_OVERRIDE_SUPPORT_ENABLE)
    usb_dwc_ll_gotgctl_set_bvalid_override_value(&USB_DWC_REG, 0);
#else
    // Workaround for ESP32-P4 ECO5, USB-OTG peripheral v4.30a
    // We are not able to detect the disconnection event from the PHY after VBUS goes low
    // So we need to notify the upper logic about the disconnection event manually
    assert(_vbus_ctx.state_changed_cb != NULL); // Sanity check
    _vbus_ctx.state_changed_cb(false);
#endif // BVALID_OVERRIDE_SUPPORT_ENABLE
}

/**
 * @brief GPIO interrupt handler for VBUS monitoring io
 *
 * @param arg GPIO number
 */
static void vbus_io_cb(void *arg)
{
    (void) arg;
    // disable interrupts for a while to debounce
    gpio_intr_disable(_vbus_ctx.gpio_num);

    bool vbus_curr_state = gpio_get_level(_vbus_ctx.gpio_num);

    if (_vbus_ctx.prev_state != vbus_curr_state) {
        _vbus_ctx.prev_state = vbus_curr_state;
        // VBUS pin state has changed, start the debounce timer
        BaseType_t yield = pdFALSE;
        if (xTimerStartFromISR(_vbus_ctx.debounce_timer, &yield) != pdPASS) {
            ESP_EARLY_LOGE(TAG, "Failed to start VBUS debounce timer");
        }
        if (yield == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    } else {
        // VBUS gpio glitch, ignore and re-enable interrupt
        ESP_ERROR_CHECK(gpio_intr_enable(_vbus_ctx.gpio_num));
    }
}

/**
 * @brief VBUS debounce timer callback
 *
 * @param xTimer Timer handle
 */
static void vbus_debounce_timer_cb(TimerHandle_t xTimer)
{
    bool vbus_prev_state = _vbus_ctx.prev_state;
    bool vbus_curr_state = gpio_get_level(_vbus_ctx.gpio_num);

    if (vbus_curr_state && vbus_prev_state) {
        vbus_appeared();
    } else if (!vbus_curr_state && !vbus_prev_state) {
        vbus_disappeared();
    } else {
        // State changed again during debounce period, ignore
    }
    // Update the state
    _vbus_ctx.prev_state = vbus_curr_state;
    // Re-enable GPIO interrupt
    ESP_ERROR_CHECK(gpio_intr_enable(_vbus_ctx.gpio_num));
}

// -------------- Public API ------------------

esp_err_t tinyusb_vbus_monitor_init(tinyusb_vbus_monitor_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid argument: config is NULL");
    ESP_RETURN_ON_FALSE(config->state_changed_cb != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid argument: state_changed_cb is NULL");

    esp_err_t ret;
    // There could be only one instance of VBUS monitoring
    if (_vbus_ctx.debounce_timer != NULL) {
        ESP_LOGE(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }

#if (CONFIG_TINYUSB_MSC_ENABLED)
    // When MSC is enabled, timer task stack size must be sufficient to handle FS operations during detach events
#if (configTIMER_TASK_STACK_DEPTH < VBUS_MON_TIMER_TASK_STACK_MIN)
    ESP_LOGE(TAG, "When MSC is enabled, configTIMER_TASK_STACK_DEPTH must be at least %d bytes to handle FS operations during attach/detach events", VBUS_MON_TIMER_TASK_STACK_MIN);
    return ESP_ERR_NOT_SUPPORTED;
#endif // (configTIMER_TASK_STACK_DEPTH)
#endif // (CONFIG_TINYUSB_MSC_ENABLED)

    _vbus_ctx.gpio_num = config->gpio_num;
    _vbus_ctx.prev_state = false;
    _vbus_ctx.state_changed_cb = config->state_changed_cb;

    // VBUS Debounce timer
    _vbus_ctx.debounce_timer = xTimerCreate("vbus_debounce_timer",
                                            pdMS_TO_TICKS(config->debounce_delay_ms),
                                            pdFALSE,
                                            NULL,
                                            vbus_debounce_timer_cb);

    if (_vbus_ctx.debounce_timer == NULL) {
        ESP_LOGE(TAG, "Create VBUS debounce timer failed");
        return ESP_ERR_NO_MEM;
    }

    // Init gpio IRQ for VBUS monitoring
    const gpio_config_t vbus_io_cfg = {
        .pin_bit_mask = BIT64(_vbus_ctx.gpio_num),
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };

    ret = gpio_config(&vbus_io_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure VBUS GPIO%d: %s", _vbus_ctx.gpio_num, esp_err_to_name(ret));
        goto gpio_fail;
    }

    ret = gpio_isr_handler_add(_vbus_ctx.gpio_num, vbus_io_cb, (void *) NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add ISR handler for GPIO%d: %s", _vbus_ctx.gpio_num, esp_err_to_name(ret));
        goto isr_err;
    }
    // Disable GPIO interrupt
    gpio_intr_disable(_vbus_ctx.gpio_num);

#if (BVALID_OVERRIDE_SUPPORT_ENABLE)
    // Set initial Bvalid override value and enable override
    usb_dwc_ll_gotgctl_set_bvalid_override_value(&USB_DWC_REG, 0);
    // Wait 1 microsecond (sufficient for >5 PHY clocks)
    esp_rom_delay_us(1);
    // Enable to override the signal from PHY
    usb_dwc_ll_gotgctl_enable_bvalid_override(&USB_DWC_REG, true);
#endif // BVALID_OVERRIDE_SUPPORT_ENABLE

    // Device could be already connected, check the status and start the timer if needed
    if (gpio_get_level(_vbus_ctx.gpio_num)) {
        _vbus_ctx.prev_state = true;
        // Start debounce timer
        if (xTimerStart(_vbus_ctx.debounce_timer, 0) != pdPASS) {
            ESP_LOGE(TAG, "Failed to start VBUS debounce timer");
            goto timer_err;
        }
    } else {
        // Enable GPIO interrupt
        ESP_ERROR_CHECK(gpio_intr_enable(_vbus_ctx.gpio_num));
    }

    ESP_LOGD(TAG, "Init GPIO%d, debounce delay: %"PRIu32" ms", config->gpio_num, config->debounce_delay_ms);
    return ESP_OK;

timer_err:
    gpio_isr_handler_remove(_vbus_ctx.gpio_num);
#if (BVALID_OVERRIDE_SUPPORT_ENABLE)
    usb_dwc_ll_gotgctl_enable_bvalid_override(&USB_DWC_REG, false);
#endif // BVALID_OVERRIDE_SUPPORT_ENABLE
isr_err:
    gpio_reset_pin(_vbus_ctx.gpio_num);
    _vbus_ctx.gpio_num = GPIO_NUM_NC;
gpio_fail:
    if (_vbus_ctx.debounce_timer) {
        xTimerDelete(_vbus_ctx.debounce_timer, 0);
        _vbus_ctx.debounce_timer = NULL;
    }
    return ret;
}

esp_err_t tinyusb_vbus_monitor_deinit(void)
{
    if (_vbus_ctx.debounce_timer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    // Reset GPIO to the default state
    ESP_ERROR_CHECK(gpio_reset_pin(_vbus_ctx.gpio_num));
    // Remove gpio IRQ for VBUS monitoring
    esp_err_t ret = gpio_isr_handler_remove(_vbus_ctx.gpio_num);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to remove ISR handler for GPIO%d: %s", _vbus_ctx.gpio_num, esp_err_to_name(ret));
        return ret;
    }
    _vbus_ctx.gpio_num = GPIO_NUM_NC;

    // Disable Debounce timer
    if (xTimerIsTimerActive(_vbus_ctx.debounce_timer) == pdTRUE) {
        xTimerStop(_vbus_ctx.debounce_timer, 0);
    }
    xTimerDelete(_vbus_ctx.debounce_timer, 0);
    _vbus_ctx.debounce_timer = NULL;

#if (BVALID_OVERRIDE_SUPPORT_ENABLE)
    // Disable to override the signal from PHY
    usb_dwc_ll_gotgctl_enable_bvalid_override(&USB_DWC_REG, false);
#endif // BVALID_OVERRIDE_SUPPORT_ENABLE

    ESP_LOGD(TAG, "Deinit");
    return ESP_OK;
}
