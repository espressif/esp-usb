/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "unity.h"

#include "usb/fusb302.h"
#include "usb/usb_tcpm.h"

static const char *TAG = "test_typec_fusb302";
#define VBUS_GPIO_PH CONFIG_USB_TCPM_PD_FUSB302_VBUS_GPIO_PH   // active-high enable
#define VBUS_GPIO_N  CONFIG_USB_TCPM_PD_FUSB302_VBUS_GPIO_N    // active-low enable (e.g., PW2609A)

/* --- I2C master bus --- */
#define I2C_SDA CONFIG_USB_TCPM_PD_FUSB302_I2C_SDA_GPIO
#define I2C_SCL CONFIG_USB_TCPM_PD_FUSB302_I2C_SCL_GPIO
#define INT_GPIO CONFIG_USB_TCPM_PD_FUSB302_FUSB_INT_GPIO

static i2c_master_bus_handle_t s_i2c_bus;
static usb_tcpm_port_handle_t s_port;
static bool s_event_handler_registered;
static bool s_default_loop_created;
static bool s_usb_tcpm_installed;

/* ===== Event callback ===== */
static void on_usb_tcpm_event(void *handler_arg, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)handler_arg;
    (void)base;

    switch (id) {
    case USB_TCPM_EVENT_SINK_ATTACHED:
    case USB_TCPM_EVENT_SOURCE_ATTACHED: {
        const usb_tcpm_evt_attached_t *const attached_event = (const usb_tcpm_evt_attached_t *)event_data;
        const char *const role = (id == USB_TCPM_EVENT_SOURCE_ATTACHED) ? "SRC" : "SNK";
        const uint32_t rp_current_ma = attached_event ? attached_event->rp_current_ma : 0;
        const bool cc2_active = attached_event ? attached_event->cc2_active : false;
        ESP_LOGI(TAG, "%s ATTACHED: cc2=%d, rp=%u mA", role, cc2_active, (unsigned)rp_current_ma);
        break;
    }
    case USB_TCPM_EVENT_SINK_DETACHED:
    case USB_TCPM_EVENT_SOURCE_DETACHED: {
        const usb_tcpm_evt_detached_t *const detached_event = (const usb_tcpm_evt_detached_t *)event_data;
        const char *const role = (id == USB_TCPM_EVENT_SOURCE_DETACHED) ? "SRC" : "SNK";
        ESP_LOGI(TAG, "%s DETACHED: port=%p", role, detached_event ? (void *)detached_event->port : NULL);
        break;
    }
    case USB_TCPM_EVENT_ERROR:
    default:
        ESP_LOGW(TAG, "EVENT %d", (int)id);
        break;
    }
}

void test_pd_fusb302_setup(void)
{
    const esp_err_t loop_err = esp_event_loop_create_default();
    TEST_ASSERT_TRUE_MESSAGE(loop_err == ESP_OK || loop_err == ESP_ERR_INVALID_STATE,
                             "Failed to create default event loop");
    s_default_loop_created = (loop_err == ESP_OK);

    const usb_tcpm_install_config_t install_cfg = {
        .task_stack = 4096,
        .task_prio = 5,
    };
    TEST_ASSERT_EQUAL(ESP_OK, usb_tcpm_install(&install_cfg));
    s_usb_tcpm_installed = true;

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = 0,                         /* use I2C0 */
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = 1 },
    };
    TEST_ASSERT_EQUAL(ESP_OK, i2c_new_master_bus(&bus_cfg, &s_i2c_bus));

    const usb_tcpm_port_config_t port_cfg = {
        .default_power_role = USB_TCPM_PWR_SINK,
        .rp_current = USB_TCPM_RP_1A5,
        .src_vbus_gpio   = VBUS_GPIO_PH,
        .src_vbus_gpio_n = VBUS_GPIO_N,
    };

    const usb_tcpm_fusb302_config_t hw = {
        .i2c_bus  = s_i2c_bus,
        .i2c_addr_7b = USB_TCPM_FUSB302_I2C_ADDR_7B_010,
        .gpio_int = INT_GPIO,
    };

    TEST_ASSERT_EQUAL(ESP_OK, esp_event_handler_register(USB_TCPM_EVENT,
                                                         ESP_EVENT_ANY_ID,
                                                         &on_usb_tcpm_event,
                                                         NULL));
    s_event_handler_registered = true;

    TEST_ASSERT_EQUAL(ESP_OK, usb_tcpm_port_create_fusb302(&port_cfg, &hw, &s_port));
}

void test_pd_fusb302_teardown(void)
{
    if (s_port != NULL) {
        TEST_ASSERT_EQUAL(ESP_OK, usb_tcpm_port_destroy(s_port));
        s_port = NULL;
    }

    if (s_event_handler_registered) {
        TEST_ASSERT_EQUAL(ESP_OK, esp_event_handler_unregister(USB_TCPM_EVENT,
                                                               ESP_EVENT_ANY_ID,
                                                               &on_usb_tcpm_event));
        s_event_handler_registered = false;
    }

    if (s_i2c_bus != NULL) {
        TEST_ASSERT_EQUAL(ESP_OK, i2c_del_master_bus(s_i2c_bus));
        s_i2c_bus = NULL;
    }

    if (s_usb_tcpm_installed) {
        TEST_ASSERT_EQUAL(ESP_OK, usb_tcpm_uninstall());
        s_usb_tcpm_installed = false;
    }

    TEST_ASSERT_EQUAL(ESP_OK, gpio_uninstall_isr_service());

    if (s_default_loop_created) {
        TEST_ASSERT_EQUAL(ESP_OK, esp_event_loop_delete_default());
        s_default_loop_created = false;
    }

    vTaskDelay(pdMS_TO_TICKS(200));
}

TEST_CASE("memory_leakage", "[type_c]")
{
    ESP_LOGI(TAG, "Waiting for attach/detach events...");

    vTaskDelay(pdMS_TO_TICKS(200));
}
