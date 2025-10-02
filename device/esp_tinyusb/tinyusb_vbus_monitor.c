/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "esp_timer.h"

#include "tinyusb.h"
#include "sdkconfig.h"

const static char *TAG = "TinyUSB vbus monitor";

#define GPIO_NUM            20 /* TODO: make as argument */
#define DEBOUNCE_DELAY_MS   250

static volatile bool vbus_enabled = false;
esp_timer_handle_t vbus_debounce_timer = NULL;

static void vbus_io_cb(void *arg)
{
    ESP_EARLY_LOGW(TAG, "VBUS IO interrupt");

    if (gpio_get_level(GPIO_NUM)) {
        // ESP_EARLY_LOGI(TAG, "VBUS connected");
        vbus_enabled = true;
    } else { // Button pressed
        // ESP_EARLY_LOGI(TAG, "VBUS disconnected");
        vbus_enabled = false;
    }

    // disable interrupts for a while to debounce
    gpio_intr_disable(GPIO_NUM);
    // enable debounce timer
    esp_timer_start_once(vbus_debounce_timer, DEBOUNCE_DELAY_MS * 1000);

}

static void vbus_debounce_timer_cb(void *arg)
{
    (void) arg;
    if (gpio_get_level(GPIO_NUM) && vbus_enabled) {
        // VBUS connected
        ESP_EARLY_LOGI(TAG, "VBUS connected");
    }

    if (!gpio_get_level(GPIO_NUM) && !vbus_enabled) {
        // VBUS disconnected
        ESP_EARLY_LOGI(TAG, "VBUS disconnected");
    }

    ESP_EARLY_LOGI(TAG, "VBUS debounce timer expired");
    // re-enable GPIO interrupt
    gpio_intr_enable(GPIO_NUM);
}


void tinyusb_vbus_monitor_init(int vbus_io)
{
    (void) vbus_io;
#if (CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3)
    // For ESP32S2/S3, we MUX the BVALID signal
    const usb_phy_otg_io_conf_t otg_io_conf = USB_PHY_SELF_POWERED_DEVICE(config->phy.vbus_monitor_io);
    phy_conf.otg_io_conf = &otg_io_conf;
#elif CONFIG_IDF_TARGET_ESP32P4
    ESP_LOGW(TAG, "Self-powered mode is supported via esp_gpio");

    // Debounce timer
    const esp_timer_create_args_t vbus_timer_args = {
        .callback = &vbus_debounce_timer_cb,
        .arg = (void *) xTaskGetCurrentTaskHandle(),
    };
    ESP_ERROR_CHECK(esp_timer_create(&vbus_timer_args, &vbus_debounce_timer));

    // Init gpio IRQ for VBUS monitoring
    const gpio_config_t vbus_pin = {
        .pin_bit_mask = BIT64(GPIO_NUM),
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&vbus_pin));
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_LOWMED));
    ESP_ERROR_CHECK(gpio_isr_handler_add(GPIO_NUM, vbus_io_cb, (void *) xTaskGetCurrentTaskHandle()));


#else
    ESP_LOGW(TAG, "Self-powered mode is not supported on this chip, ignoring the setting");
#endif // CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3
}

void tinyusb_vbus_monitor_deinit(void)
{
#if (CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3)
    // For ESP32S2/S3, nothing to deinit
#elif CONFIG_IDF_TARGET_ESP32P4
    // Deinit gpio IRQ for VBUS monitoring
#else
    // Nothing to deinit
#endif // CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32
}
