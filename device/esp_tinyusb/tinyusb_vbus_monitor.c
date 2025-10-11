/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "esp_timer.h"

#include "tinyusb.h"
#include "sdkconfig.h"

const static char *TAG = "VBUS mon";

#define DEBOUNCE_DELAY_MS   500         /* TODO: make configurable */

#if (CONFIG_IDF_TARGET_ESP32P4)
// On ESP32-P4 USB OTG 2.0 signals are not wired to GPIO matrix
// So we need to override the Bvalid signal from PHY
#define ENABLE_BVALID_OVERRIDE          1
#define USB_DWC_REG                     USB_DWC_HS
#endif // CONFIG_IDF_TARGET_ESP32P4

#if (ENABLE_BVALID_OVERRIDE)
#include "soc/usb_dwc_struct.h"
#endif // ENABLE_BVALID_OVERRIDE

static esp_timer_handle_t _vbus_debounce_timer = NULL;

static void vbus_io_cb(void *arg)
{
    int vbus_io_num  = (int) arg;
    // disable interrupts for a while to debounce
    gpio_intr_disable(vbus_io_num);
    // enable debounce timer
    esp_timer_start_once(_vbus_debounce_timer, DEBOUNCE_DELAY_MS * 1000);
}

static void vbus_debounce_timer_cb(void *arg)
{
    int vbus_io_num = (int) arg;

    if (gpio_get_level(vbus_io_num)) {
        // If the VBUS is still present
#if (ENABLE_BVALID_OVERRIDE)
        USB_DWC_REG.gotgctl_reg.bvalidovval = 1;
#endif // ENABLE_BVALID_OVERRIDE
    } else {
        // VBUS is not present
#if (ENABLE_BVALID_OVERRIDE)
        USB_DWC_REG.gotgctl_reg.bvalidovval = 0;
#endif // ENABLE_BVALID_OVERRIDE

        // Note:
        // When device stayed connected in the USB Host port, we need to disable the pull-up resistor on D+ D- first
        // Disable pull-up resistor on D+ D-
        // But this creates a breaking change, so we can call it in the notification callback
    }

    // Re-enable GPIO interrupt
    gpio_intr_enable(vbus_io_num);
}

esp_err_t tinyusb_vbus_monitor_init(int vbus_io_num)
{
    esp_err_t ret;

    // There could be only one instance of VBUS monitoring
    if (_vbus_debounce_timer) {
        ESP_LOGE(TAG, "Already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // VBUS Debounce timer
    const esp_timer_create_args_t vbus_timer_args = {
        .callback = &vbus_debounce_timer_cb,
        .arg = (void *) vbus_io_num,
    };
    ret = esp_timer_create(&vbus_timer_args, &_vbus_debounce_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Create VBUS debounce timer failed");
        return ret;
    }

    // Init gpio IRQ for VBUS monitoring
    const gpio_config_t vbus_io_cfg = {
        .pin_bit_mask = BIT64(vbus_io_num),
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };

    ret = gpio_config(&vbus_io_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Config VBUS GPIO failed");
        goto gpio_fail;
    }

    ret = gpio_install_isr_service(ESP_INTR_FLAG_LOWMED);
    // Service could be already installed, it is not an error
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Install GPIO ISR service failed");
        goto isr_fail;
    }

    ret = gpio_isr_handler_add(vbus_io_num, vbus_io_cb, (void *) vbus_io_num);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Add GPIO ISR handler failed");
        goto add_isr_hdl_fail;
    }

#if (ENABLE_BVALID_OVERRIDE)
    // Set Bvalid signal to 0 initially
    USB_DWC_REG.gotgctl_reg.bvalidovval = 0;
    // Wait 1 microsecond (sufficient for >5 PHY clocks)
    esp_rom_delay_us(1);
    // Enable to override the signal from PHY
    // Device could be already connected, check the status and start the timer if needed
    if (gpio_get_level(vbus_io_num)) {
        USB_DWC_REG.gotgctl_reg.bvalidoven = 1;
        esp_timer_start_once(_vbus_debounce_timer, DEBOUNCE_DELAY_MS * 1000);
    } else {
        USB_DWC_REG.gotgctl_reg.bvalidoven = 0;
    }
#endif // ENABLE_BVALID_OVERRIDE

    ESP_LOGW(TAG, "Configured via GPIO%d", vbus_io_num);
    return ESP_OK;

add_isr_hdl_fail:
    gpio_uninstall_isr_service();
isr_fail:
    gpio_reset_pin(vbus_io_num);
gpio_fail:
    if (_vbus_debounce_timer) {
        esp_timer_delete(_vbus_debounce_timer);
        _vbus_debounce_timer = NULL;
    }
    return ret;
}

void tinyusb_vbus_monitor_deinit(int vbus_io_num)
{
    // Deinit gpio IRQ for VBUS monitoring
    if (esp_timer_is_active(_vbus_debounce_timer)) {
        ESP_ERROR_CHECK(esp_timer_stop(_vbus_debounce_timer));
    }
    esp_timer_delete(_vbus_debounce_timer);
    _vbus_debounce_timer = NULL;

    // Deinit gpio IRQ for VBUS monitoring
    ESP_ERROR_CHECK(gpio_isr_handler_remove(vbus_io_num));
    gpio_intr_disable(vbus_io_num);
    gpio_uninstall_isr_service();

    ESP_LOGD(TAG, "Deinit");
}
