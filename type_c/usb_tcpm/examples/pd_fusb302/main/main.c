/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "driver/i2c_master.h"
#include "esp_event.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "usb/fusb302.h"
#include "usb/usb_tcpm.h"

static const char *TAG = "example_usb_tcpm_fusb302";
#define VBUS_GPIO_PH CONFIG_USB_TCPM_PD_FUSB302_VBUS_GPIO_PH   // active-high enable
#define VBUS_GPIO_N  CONFIG_USB_TCPM_PD_FUSB302_VBUS_GPIO_N    // active-low enable (e.g., PW2609A)

/* --- I2C master bus --- */
#define I2C_SDA CONFIG_USB_TCPM_PD_FUSB302_I2C_SDA_GPIO
#define I2C_SCL CONFIG_USB_TCPM_PD_FUSB302_I2C_SCL_GPIO
#define INT_GPIO CONFIG_USB_TCPM_PD_FUSB302_FUSB_INT_GPIO

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
        const void *port_handle = attached_event ? (void *)attached_event->port : NULL;
        ESP_LOGI(TAG, "%s ATTACHED: port=%p, cc2=%d, rp=%u mA", role, port_handle, cc2_active, rp_current_ma);
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

/* ===== App entry ===== */
void app_main(void)
{
    /* --- Install TCPM --- */
    const usb_tcpm_install_config_t install_cfg = {
        .task_stack = 4096,
        .task_prio = 5,
    };
    ESP_ERROR_CHECK(usb_tcpm_install(&install_cfg));

    i2c_master_bus_handle_t bus = NULL;
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = 0,                         /* use I2C0 */
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = 1 },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    /* --- Create typec port (FUSB302 backend) --- */
    usb_tcpm_port_handle_t port = NULL;

    const usb_tcpm_port_config_t port_cfg = {
        .default_power_role = USB_TCPM_PWR_DRP,
        .rp_current = USB_TCPM_RP_1A5,
        .src_vbus_gpio   = VBUS_GPIO_PH,
        .src_vbus_gpio_n = VBUS_GPIO_N,
    };

    const usb_tcpm_fusb302_config_t hw = {
        .i2c_bus  = bus,
        .i2c_addr_7b = USB_TCPM_FUSB302_I2C_ADDR_7B_010,
        .gpio_int = INT_GPIO,
    };

    ESP_ERROR_CHECK(esp_event_handler_register(
                        USB_TCPM_EVENT,
                        ESP_EVENT_ANY_ID,
                        &on_usb_tcpm_event, NULL));
    ESP_ERROR_CHECK(usb_tcpm_port_create_fusb302(&port_cfg, &hw, &port));
    ESP_LOGI(TAG, "Waiting for attach/detach events...");

    /* One-shot status snapshot after startup */
    vTaskDelay(pdMS_TO_TICKS(5000));
    usb_tcpm_port_status_t status = {0};
    if (usb_tcpm_get_status(port, &status) == ESP_OK) {
        ESP_LOGI(TAG, "status: attached=%d cc2=%d rp=%u mA role=%d",
                 status.attached, status.cc2_active, (unsigned)status.rp_current_ma, (int)status.role);
    }

    /* App can idle; the Type-C task & ISR do the work */
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
