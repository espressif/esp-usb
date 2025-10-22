/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "driver/i2c.h"

#include "esp_typec_pd.h"

#define TAG "example_pd"

#ifndef EX_FUSB302_I2C_PORT
#define EX_FUSB302_I2C_PORT I2C_NUM_0
#endif

#ifndef EX_FUSB302_SDA_IO
#define EX_FUSB302_SDA_IO  7   // <- SDA = GPIO7
#endif
#ifndef EX_FUSB302_SCL_IO
#define EX_FUSB302_SCL_IO  8   // <- SCL = GPIO8
#endif

#ifndef EX_FUSB302_INT_IO
#define EX_FUSB302_INT_IO  9   // <- INT_N = GPIO9 (open-drain, active-low)
#endif

#ifndef EX_FUSB302_I2C_ADDR
#define EX_FUSB302_I2C_ADDR 0x22  // 7-bit from “0100 010R/W”
#endif

static void on_pd(esp_typec_pd_event_t evt, const void *payload, void *arg)
{
    (void)arg;
    switch (evt) {
    case ESP_TYPEC_PD_EVENT_ATTACHED: {
        const esp_typec_pd_evt_attached_t *a = payload;
        bool cc2 = (a->flags & ESP_TYPEC_PD_FLAG_CC2) != 0;
        ESP_LOGI(TAG, "ATTACHED: cc2=%d, rp=%u mA",
                 cc2, (unsigned)a->rp_cur_ma);
        break;
    }
    case ESP_TYPEC_PD_EVENT_DETACHED:
        ESP_LOGI(TAG, "DETACHED");
        break;
    default:
        ESP_LOGD(TAG, "evt=%d", evt);
        break;
    }
}

static esp_err_t i2c_master_simple_init(void)
{
    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = EX_FUSB302_SDA_IO,
        .scl_io_num = EX_FUSB302_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
        .clk_flags = 0,
    };
    ESP_RETURN_ON_ERROR(i2c_param_config(EX_FUSB302_I2C_PORT, &cfg), TAG, "i2c_param_config");
    ESP_RETURN_ON_ERROR(i2c_driver_install(EX_FUSB302_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0), TAG, "i2c_driver_install");
    return ESP_OK;
}

void app_main(void)
{
    ESP_ERROR_CHECK(i2c_master_simple_init());

    esp_typec_pd_install_config_t lib_cfg = {
        .skip_ctrl_intr_alloc = true,   // start with polling
        .intr_flags = 0,
    };
    ESP_ERROR_CHECK(esp_typec_pd_install(&lib_cfg));

    esp_typec_pd_port_config_t port_cfg = {
        .default_power_role   = ESP_TYPEC_PD_PWR_SINK,
        .sink_i_min_ma        = 500,
        .sink_fixed_pref_mv   = 0,      // auto (request later when PD implemented)
        .enable_pps           = false,
        .sink_pps_v_min_mv    = 0, .sink_pps_v_max_mv = 0, .sink_pps_i_max_ma = 0,
        .src_pdos             = NULL, .src_pdo_count = 0,
        .task_stack           = 4096, .task_prio = 5,
    };

    esp_typec_pd_fusb302_config_t hw = {
        .i2c_port = EX_FUSB302_I2C_PORT,
        .i2c_addr = EX_FUSB302_I2C_ADDR,
        .gpio_int = EX_FUSB302_INT_IO,
        .use_intr = false,              // polling first; switch to true later
    };

    esp_typec_pd_port_handle_t h = NULL;
    ESP_ERROR_CHECK(esp_typec_pd_port_create_fusb302(&port_cfg, &hw, on_pd, NULL, &h));

    ESP_LOGI(TAG, "Waiting for attach/detach events...");
    // PD task runs in the component; nothing else to do here.
}
