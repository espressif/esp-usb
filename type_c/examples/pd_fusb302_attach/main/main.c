/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include "esp_log.h"

/* Make sure TickType_t is known before including our PD header */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"

#include "esp_typec_pd.h"   // your library header

static const char *TAG = "example_pd";

/* ===== Event callback ===== */
static void on_pd_event(esp_typec_pd_event_t evt, const void *data, void *arg)
{
    (void)arg;
    switch (evt) {
    case ESP_TYPEC_PD_EVENT_ATTACHED: {
        const esp_typec_pd_evt_attached_t *a = (const esp_typec_pd_evt_attached_t *)data;
        bool cc2 = (a && (a->flags & ESP_TYPEC_PD_FLAG_CC2));
        uint32_t ma = a ? a->rp_cur_ma : 0;
        ESP_LOGI(TAG, "ATTACHED: cc2=%d, rp=%u mA", cc2 ? 1 : 0, (unsigned)ma);
        break;
    }
    case ESP_TYPEC_PD_EVENT_DETACHED:
        ESP_LOGI(TAG, "DETACHED");
        break;
    case ESP_TYPEC_PD_EVENT_ERROR:
    default:
        ESP_LOGW(TAG, "EVENT %d", evt);
        break;
    }
}

/* ===== App entry ===== */
void app_main(void)
{
    ESP_LOGI("main_task", "Calling app_main()");

    /* --- I2C master bus (new driver) --- */
    const int I2C_SDA = 7;
    const int I2C_SCL = 8;
    const int INT_GPIO = 9;
    const uint8_t FUSB_ADDR_7B = 0x22;   /* from your bring-up */

    i2c_master_bus_handle_t bus = NULL;
    i2c_master_dev_handle_t fusb_i2c = NULL;

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = 0,                         /* use I2C0 */
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = 1 },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    i2c_device_config_t dev_cfg = {
        .device_address = FUSB_ADDR_7B,
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .scl_speed_hz = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &fusb_i2c));

    /* --- Create PD port (FUSB302 backend) --- */
    esp_typec_pd_port_handle_t port = NULL;

    esp_typec_pd_port_config_t port_cfg = {
        .default_power_role = ESP_TYPEC_PWR_SINK,
        .task_stack = 4096,
        .task_prio  = 5,
        /* only fields guaranteed by your current header */
    };

    esp_typec_pd_fusb302_config_t hw = {
        .i2c_dev  = fusb_i2c,
        .gpio_int = INT_GPIO,
    };

    /* If your library requires install(), call it here; otherwise skip */
    // ESP_ERROR_CHECK(esp_typec_pd_install(NULL));

    ESP_ERROR_CHECK(esp_typec_pd_port_create_fusb302(&port_cfg, &hw,
                                                     on_pd_event, NULL, &port));
    ESP_ERROR_CHECK(esp_typec_pd_set_power_role(port, ESP_TYPEC_PWR_SOURCE));
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
