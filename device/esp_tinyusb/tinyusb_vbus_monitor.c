/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "driver/gpio.h"
#include "tinyusb.h"
#include "tinyusb_vbus_monitor.h"
#include "sdkconfig.h"

const static char *TAG = "VBUS mon";

#if (CONFIG_IDF_TARGET_ESP32P4)
#include "soc/usb_dwc_struct.h"
// On ESP32-P4 USB OTG 2.0 signals are not wired to GPIO matrix
// So we need to override the Bvalid signal from PHY
#define USB_DWC_REG                     USB_DWC_HS
#endif // CONFIG_IDF_TARGET_ESP32P4

#define GLITCH_FILTER       0           /* TODO: make configurable ? */
typedef struct {
    gpio_num_t vbus_io_num;
    bool vbus_prev_state;
    bool vbus_state_changed;
} vbus_context_t;

static vbus_context_t _vbus_ctx;
static TimerHandle_t _vbus_debounce_timer = NULL;

//
// Additional low-level USB DWC functions, which are not present in the IDF USB DWC HAL
//

// --------------- GOTGCTL register ------------------

static void usb_dwc_ll_gotgctl_set_bvalid_override_value(usb_dwc_dev_t *hw, uint8_t value)
{
    hw->gotgctl_reg.bvalidovval = value;
}

static void usb_dwc_ll_gotgctl_enable_bvalid_override(usb_dwc_dev_t *hw, bool enable)
{
    hw->gotgctl_reg.bvalidoven = enable ? 1 : 0;
}

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
    usb_dwc_ll_gotgctl_set_bvalid_override_value(&USB_DWC_REG, 1);
    usb_dwc_ll_dctl_set_soft_disconnect(&USB_DWC_REG, false);
}

/**
 * @brief Handle VBUS disappeared event
 */
static void vbus_disappeared(void)
{
    ESP_LOGD(TAG, "Disappeared");
    usb_dwc_ll_gotgctl_set_bvalid_override_value(&USB_DWC_REG, 0);
    usb_dwc_ll_dctl_set_soft_disconnect(&USB_DWC_REG, true);
}

/**
 * @brief GPIO interrupt handler for VBUS monitoring io
 *
 * @param arg GPIO number
 */
static void vbus_io_cb(void *arg)
{
    int vbus_io_num  = (int) arg;
    // disable interrupts for a while to debounce
    gpio_intr_disable(vbus_io_num);

    bool vbus_curr_state = gpio_get_level(vbus_io_num);

    if (_vbus_ctx.vbus_prev_state != vbus_curr_state) {
        _vbus_ctx.vbus_prev_state = vbus_curr_state;
        _vbus_ctx.vbus_state_changed = true;
        // VBUS pin state has changed, start the debounce timer
        BaseType_t yield = pdFALSE;
        if (xTimerStartFromISR(_vbus_debounce_timer, &yield) != pdPASS) {
            ESP_EARLY_LOGE(TAG, "Failed to start VBUS debounce timer");
        }
        if (yield == pdTRUE) {
            portYIELD_FROM_ISR();
        }
    } else {
        /* TODO: Verify happening with and without glitch filter */
        // VBUS gpio glitch, ignore and re-enable interrupt
        _vbus_ctx.vbus_state_changed = false;
        gpio_intr_enable(vbus_io_num);
    }
}

/**
 * @brief VBUS debounce timer callback
 *
 * @param xTimer Timer handle
 */
static void vbus_debounce_timer_cb(TimerHandle_t xTimer)
{
    int vbus_io_num = _vbus_ctx.vbus_io_num;
    bool vbus_prev_state = _vbus_ctx.vbus_prev_state;
    bool vbus_curr_state = gpio_get_level(vbus_io_num);

    if (vbus_curr_state && vbus_prev_state) {
        vbus_appeared();
    } else if (!vbus_curr_state && !vbus_prev_state) {
        vbus_disappeared();
    } else {
        // State changed again during debounce period, ignore
    }
    // Update the state
    _vbus_ctx.vbus_prev_state = vbus_curr_state;
    // Re-enable GPIO interrupt
    gpio_intr_enable(vbus_io_num);
}

// -------------- Public API ------------------

esp_err_t tinyusb_vbus_monitor_init(tinyusb_vbus_monitor_config_t *config)
{
    esp_err_t ret;

    // There could be only one instance of VBUS monitoring
    if (_vbus_debounce_timer) {
        ESP_LOGE(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    _vbus_ctx.vbus_io_num = config->gpio_num;
    _vbus_ctx.vbus_prev_state = false;
    _vbus_ctx.vbus_state_changed = false;

    // VBUS Debounce timer
    _vbus_debounce_timer = xTimerCreate("vbus_debounce_timer",
                                        pdMS_TO_TICKS(config->debounce_delay_ms),
                                        pdFALSE,
                                        NULL,
                                        vbus_debounce_timer_cb);

    if (_vbus_debounce_timer == NULL) {
        ESP_LOGE(TAG, "Create VBUS debounce timer failed");
        return ESP_ERR_NO_MEM;
    }

    // Init gpio IRQ for VBUS monitoring
    const gpio_config_t vbus_io_cfg = {
        .pin_bit_mask = BIT64(_vbus_ctx.vbus_io_num),
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };

    ret = gpio_config(&vbus_io_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Config VBUS GPIO failed");
        goto gpio_fail;
    }

#if (GLITCH_FILTER)
    ESP_LOGW(TAG, "Apply glitch filter to the VBUS GPIO");
    gpio_glitch_filter_handle_t filter;
    gpio_flex_glitch_filter_config_t filter_cfg = {
        .clk_src = GLITCH_FILTER_CLK_SRC_DEFAULT,
        .gpio_num = _vbus_ctx.vbus_io_num,
        .window_thres_ns = 500 /* DEBOUNCE_DELAY_MS * 1000 * 1000 */,
        .window_width_ns = 500 /* DEBOUNCE_DELAY_MS * 1000 * 1000 */,
    };
    ret = (gpio_new_flex_glitch_filter(&filter_cfg, &filter));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Create glitch filter failed");
        // goto filter_fail;
    }

    ESP_LOGW(TAG, "Enable the glitch filter");
    ret = gpio_glitch_filter_enable(filter);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Enable glitch filter failed");
        // goto enable_filter_fail;
    }
#endif // GLITCH_FILTER

    ret = gpio_install_isr_service(ESP_INTR_FLAG_LOWMED);
    // Service could be already installed, it is not an error
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Install GPIO ISR service failed");
        goto isr_fail;
    }

    ret = gpio_isr_handler_add(_vbus_ctx.vbus_io_num, vbus_io_cb, (void *) _vbus_ctx.vbus_io_num);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Add GPIO ISR handler failed");
        goto add_isr_hdl_fail;
    }
    // Disable GPIO interrupt
    gpio_intr_disable(_vbus_ctx.vbus_io_num);
    // Set initial Bvalid override value and enable override
    usb_dwc_ll_gotgctl_set_bvalid_override_value(&USB_DWC_REG, 0);
    // Wait 1 microsecond (sufficient for >5 PHY clocks)
    esp_rom_delay_us(1);
    // Enable to override the signal from PHY
    usb_dwc_ll_gotgctl_enable_bvalid_override(&USB_DWC_REG, true);

    // Device could be already connected, check the status and start the timer if needed
    if (gpio_get_level(_vbus_ctx.vbus_io_num)) {
        _vbus_ctx.vbus_prev_state = true;
        // Start debounce timer
        if (xTimerStart(_vbus_debounce_timer, 0) != pdPASS) {
            ESP_LOGE(TAG, "Failed to start VBUS debounce timer");
        }
    } else {
        // Enable GPIO interrupt
        gpio_intr_enable(_vbus_ctx.vbus_io_num);
    }

    ESP_LOGD(TAG, "Configured via GPIO%d, debounce delay: %"PRIu32" ms", _vbus_ctx.vbus_io_num, config->debounce_delay_ms);
    return ESP_OK;

add_isr_hdl_fail:
    gpio_uninstall_isr_service();
isr_fail:
    gpio_reset_pin(_vbus_ctx.vbus_io_num);
gpio_fail:
    if (_vbus_debounce_timer) {
        xTimerDelete(_vbus_debounce_timer, 0);
        _vbus_debounce_timer = NULL;
    }
    return ret;
}

void tinyusb_vbus_monitor_deinit(void)
{
    // Deinit gpio IRQ for VBUS monitoring
    if (xTimerIsTimerActive(_vbus_debounce_timer) == pdTRUE) {
        xTimerStop(_vbus_debounce_timer, 0);
    }
    xTimerDelete(_vbus_debounce_timer, 0);
    _vbus_debounce_timer = NULL;

    // Disable to override the signal from PHY
    usb_dwc_ll_gotgctl_enable_bvalid_override(&USB_DWC_REG, false);

    // Deinit gpio IRQ for VBUS monitoring
    ESP_ERROR_CHECK(gpio_isr_handler_remove(_vbus_ctx.vbus_io_num));
    gpio_intr_disable(_vbus_ctx.vbus_io_num);
    gpio_uninstall_isr_service();

    ESP_LOGD(TAG, "Deinit");
}
