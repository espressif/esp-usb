/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_typec_pd.h"

static const char *TAG = "example_pd";
#define VBUS_GPIO_PH 28   // active-high enable
#define VBUS_GPIO_N   15  // active-low enable (e.g., PW2609A)

/* ===== Event callback ===== */
static void on_pd_event(void *handler_arg, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)handler_arg;
    (void)base;

    switch (id) {
    case ESP_TYPEC_PD_EVENT_ID_ATTACHED: {
        const esp_typec_pd_evt_attached_t *a = (const esp_typec_pd_evt_attached_t *)event_data;
        bool cc2 = a ? a->cc2_active : false;
        uint32_t ma = a ? a->rp_cur_ma : 0;
        ESP_LOGI(TAG, "ATTACHED: cc2=%d, rp=%u mA", cc2 ? 1 : 0, (unsigned)ma);
        break;
    }
    case ESP_TYPEC_PD_EVENT_ID_DETACHED:
        ESP_LOGI(TAG, "DETACHED");
        break;
    case ESP_TYPEC_PD_EVENT_ID_ERROR:
    default:
        ESP_LOGW(TAG, "EVENT %d", (int)id);
        break;
    }
}

/* ===== App entry ===== */
void app_main(void)
{
    ESP_LOGI("main_task", "Calling app_main()");

    ESP_ERROR_CHECK(esp_typec_pd_install(NULL));
    /* --- I2C master bus (new driver) --- */
    const int I2C_SDA = 7;
    const int I2C_SCL = 8;
    const int INT_GPIO = 9;
    const uint8_t FUSB_ADDR_7B = 0x22;

    i2c_master_bus_handle_t bus = NULL;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = 0,                         /* use I2C0 */
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = 1 },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    /* --- Create PD port (FUSB302 backend) --- */
    esp_typec_pd_port_handle_t port = NULL;

    esp_typec_pd_port_config_t port_cfg = {
        .default_power_role = ESP_TYPEC_PWR_SOURCE,
        .task_stack = 4096,
        .task_prio  = 5,
        .src_vbus_gpio   = VBUS_GPIO_PH,
        .src_vbus_gpio_n = VBUS_GPIO_N,
        .rp_current = ESP_TYPEC_RP_1A5,
    };

    esp_typec_pd_fusb302_config_t hw = {
        .i2c_bus  = bus,
        .i2c_addr_7b = FUSB_ADDR_7B,
        .gpio_int = INT_GPIO,
    };

    ESP_ERROR_CHECK(esp_typec_pd_port_create_fusb302(&port_cfg, &hw, &port));
    //ESP_ERROR_CHECK(esp_typec_pd_set_power_role(port, ESP_TYPEC_PWR_SOURCE));
    ESP_ERROR_CHECK(esp_event_handler_register(
                        ESP_TYPEC_PD_EVENT,
                        ESP_EVENT_ANY_ID,
                        &on_pd_event, NULL));
    ESP_LOGI(TAG, "Waiting for attach/detach events...");

    /* App can idle; the PD task & ISR do the work */
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /* (Optional cleanup if you ever leave the loop)
    esp_typec_pd_port_destroy(port);
    i2c_master_bus_rm_device(fusb_i2c);
    i2c_del_master_bus(bus);
    */
}
