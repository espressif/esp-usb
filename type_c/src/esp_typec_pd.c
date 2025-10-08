/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

#include "driver/gpio.h"

#include "esp_typec_pd.h"     // public PD API (events, configs)
#include "typec_backend.h"   /* PD_EVT_* and typec_cc_status_t */
#include "fusb302_ctrl.h"     // backend for fusb302

#define TAG "esp_typec_pd"

/* ---------- Per-port context ---------- */
typedef struct pd_port_backend {
    esp_err_t (*set_role)(void *dev, esp_typec_power_role_t role);
    esp_err_t (*service_irq)(void *dev, typec_evt_mask_t *events, typec_cc_status_t *st);
    esp_err_t (*get_status)(void *dev, typec_cc_status_t *st);
    esp_err_t (*deinit)(void *dev);
} pd_port_backend_t;

typedef struct pd_port_ctx {
    // Backend handle
    void *ctrl;
    const pd_port_backend_t *backend;

    // INT GPIO (active-low)
    int gpio_int;

    // Task + callback
    TaskHandle_t task;
    esp_typec_pd_event_cb_t cb;
    void *cb_arg;

    // Cached state
    bool     attached;
    bool     cc2_active;
    uint32_t rp_ma;

    // Snapshot of config (optional)
    esp_typec_pd_port_config_t cfg;
} pd_port_ctx_t;

/* ---------- Tiny helpers ---------- */
static inline void pd_emit_attached(pd_port_ctx_t *ctx)
{
    if (!ctx->cb) {
        return;
    }
    esp_typec_pd_evt_attached_t a = {
        .flags     = ctx->cc2_active ? ESP_TYPEC_PD_FLAG_CC2 : 0,
        .rp_cur_ma = ctx->rp_ma,
    };
    ctx->cb(ESP_TYPEC_PD_EVENT_ATTACHED, &a, ctx->cb_arg);
}

static inline void pd_emit_detached(pd_port_ctx_t *ctx)
{
    if (!ctx->cb) {
        return;
    }
    ctx->cb(ESP_TYPEC_PD_EVENT_DETACHED, NULL, ctx->cb_arg);
}

/* ---------- ISR & Task ---------- */

static void IRAM_ATTR pd_gpio_isr(void *arg)
{
    pd_port_ctx_t *ctx = (pd_port_ctx_t *)arg;
    // Gate further edges until the task drains the device
    gpio_intr_disable(ctx->gpio_int);
    BaseType_t hp = pdFALSE;
    vTaskNotifyGiveFromISR(ctx->task, &hp);
    if (hp) {
        portYIELD_FROM_ISR();
    }
}

static void pd_task(void *arg)
{
    pd_port_ctx_t *ctx = arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        typec_evt_mask_t ev = 0;
        typec_cc_status_t st = {0};

        /* Drain IRQs and get a fresh snapshot in one call */
        ctx->backend->service_irq(ctx->ctrl, &ev, &st);

        /* VBUS-driven attach/detach (sink policy) */
        if (ev & PD_EVT_VBUS) {
            if (st.vbus_ok && !ctx->attached) {
                // VBUS became valid -> read CC once for orientation/current
                ctx->backend->get_status(ctx->ctrl, &st);
                ctx->attached   = true;
                ctx->cc2_active = st.cc2_active;
                ctx->rp_ma      = st.rp_cur_ma;
                pd_emit_attached(ctx);
            } else if (!st.vbus_ok && ctx->attached) {
                // VBUS dropped -> detached
                ctx->attached = false;
                pd_emit_detached(ctx);
            }
        }

        /* Optional: while attached, reflect advertised current changes quietly */
        if ((ev & PD_EVT_CC) && ctx->attached) {
            ctx->backend->get_status(ctx->ctrl, &st);
            ctx->rp_ma = st.rp_cur_ma;
        }

        /* Handle rare stuck-low INT */
        if (gpio_get_level(ctx->gpio_int) == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            (void)ctx->backend->service_irq(ctx->ctrl, &ev, &st);
        }

        gpio_intr_enable(ctx->gpio_int);

    }
}

/* ---------- Public API ---------- */

esp_err_t esp_typec_pd_install(const esp_typec_pd_install_config_t *config)
{
    (void)config;
    return ESP_OK;
}

esp_err_t esp_typec_pd_uninstall(void)
{
    return ESP_OK;
}

esp_err_t esp_typec_pd_lib_handle_events(TickType_t timeout_ticks, uint32_t *event_flags_ret)
{
    (void)timeout_ticks;
    (void)event_flags_ret;
    // No library-global pump; all work is per-port task
    return ESP_OK;
}

esp_err_t esp_typec_pd_lib_info(esp_typec_pd_lib_info_t *info_ret)
{
    if (!info_ret) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static const pd_port_backend_t fusb302_backend = {
    .set_role = (esp_err_t (*)(void *, esp_typec_power_role_t))fusb302_set_role,
    .service_irq = (esp_err_t (*)(void *, typec_evt_mask_t *, typec_cc_status_t *))fusb302_service_irq,
    .get_status  = (esp_err_t (*)(void *, typec_cc_status_t *))fusb302_get_status,
    .deinit = (esp_err_t (*)(void *))fusb302_deinit,
};

esp_err_t esp_typec_pd_port_create_fusb302(const esp_typec_pd_port_config_t *port_cfg,
                                           const esp_typec_pd_fusb302_config_t *hw_cfg,
                                           esp_typec_pd_event_cb_t cb, void *cb_arg,
                                           esp_typec_pd_port_handle_t *port_hdl_ret)
{
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_FALSE(port_cfg && hw_cfg && cb && port_hdl_ret, ESP_ERR_INVALID_ARG, TAG, "bad args");

    pd_port_ctx_t *ctx = (pd_port_ctx_t *)calloc(1, sizeof(*ctx));
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_NO_MEM, TAG, "no mem");

    ctx->cfg    = *port_cfg;
    ctx->cb     = cb;
    ctx->cb_arg = cb_arg;
    ctx->backend = &fusb302_backend;

    // Init backend
    fusb302_hw_cfg_t hw = {
        .i2c_dev  = hw_cfg->i2c_dev,   // NEW: i2c_master_dev_handle_t
        .gpio_int = hw_cfg->gpio_int,
    };
    fusb302_dev_t *dev = NULL;
    ESP_RETURN_ON_ERROR(fusb302_init(&hw, &dev), TAG, "ctrl init failed");
    ctx->ctrl = dev;

    ctx->gpio_int = hw_cfg->gpio_int;

    // Configure INT GPIO as input with pull-up and any-edge interrupt
    gpio_config_t gc = {
        .pin_bit_mask = 1ULL << ctx->gpio_int,
                             .mode = GPIO_MODE_INPUT,
                             .pull_up_en = GPIO_PULLUP_ENABLE,
                             .pull_down_en = GPIO_PULLDOWN_DISABLE,
                             .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_GOTO_ON_ERROR(gpio_config(&gc), fail, TAG, "gpio_config");
    int lvl = gpio_get_level(ctx->gpio_int);
    ESP_LOGI(TAG, "INT gpio=%d configured, current level=%d", ctx->gpio_int, lvl);

    esp_err_t isr_ret = gpio_install_isr_service(0);
    if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) {
        goto fail;
    }
    ESP_GOTO_ON_ERROR(gpio_isr_handler_add(ctx->gpio_int, pd_gpio_isr, ctx), fail, TAG, "gpio_isr_add");

    // Create task
    const uint32_t stack_words = (port_cfg->task_stack ? port_cfg->task_stack : 4096) / sizeof(StackType_t);
    const UBaseType_t prio     = (port_cfg->task_prio ? port_cfg->task_prio : (tskIDLE_PRIORITY + 2));

    BaseType_t ok = xTaskCreate(pd_task, "pd_fusb302", stack_words, ctx, prio, &ctx->task);
    if (ok != pdPASS) {
        gpio_isr_handler_remove(ctx->gpio_int);
        ctx->backend->deinit(ctx->ctrl);
        free(ctx);
        return ESP_ERR_NO_MEM;
    }

    /* Drain any pre-existing latched causes before arming GPIO */
    typec_evt_mask_t ev = 0;
    typec_cc_status_t st = (typec_cc_status_t) {
        0
    };
    ESP_GOTO_ON_ERROR(ctx->backend->service_irq(ctx->ctrl, &ev, &st), fail, TAG, "prime irq");

    /* Now arm the GPIO */
    gpio_intr_enable(ctx->gpio_int);

    /* If INT is already asserted low right now, kick the task once */
    if (gpio_get_level(ctx->gpio_int) == 0) {
        BaseType_t hp = pdFALSE;
        vTaskNotifyGiveFromISR(ctx->task, &hp);
        if (hp) {
            portYIELD_FROM_ISR();
        }
    }

    *port_hdl_ret = (esp_typec_pd_port_handle_t)ctx;
    ESP_LOGI(TAG, "PD task started");
    return ESP_OK;
fail:
    ESP_LOGE(TAG, "failed, err=%d", ret);
    if (ctx) {
        if (ctx->task) {
            vTaskDelete(ctx->task);
            ctx->task = NULL;
        }
        if (ctx->ctrl) {
            ctx->backend->deinit(ctx->ctrl);
            ctx->ctrl = NULL;
        }
        free(ctx);
    }
    return ret;
}

esp_err_t esp_typec_pd_port_destroy(esp_typec_pd_port_handle_t port_hdl)
{
    pd_port_ctx_t *ctx = (pd_port_ctx_t *)port_hdl;
    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }

    if (ctx->task) {
        vTaskDelete(ctx->task);
        ctx->task = NULL;
    }

    gpio_isr_handler_remove(ctx->gpio_int);

    if (ctx->ctrl && ctx->backend) {
        ctx->backend->deinit(ctx->ctrl);
        ctx->ctrl = NULL;
    }

    free(ctx);
    return ESP_OK;
}

esp_err_t esp_typec_pd_set_power_role(esp_typec_pd_port_handle_t port_hdl,
                                      esp_typec_power_role_t role)
{
    pd_port_ctx_t *ctx = (pd_port_ctx_t *)port_hdl;
    if (!ctx || !ctx->backend) {
        return ESP_ERR_INVALID_ARG;
    }
    return ctx->backend->set_role(ctx->ctrl, role);
}

/* Stubs for later PD support */
esp_err_t esp_typec_pd_sink_request_fixed(esp_typec_pd_port_handle_t port, uint32_t mv, uint32_t ma)
{
    (void)port;
    (void)mv;
    (void)ma;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_typec_pd_sink_request_pps(esp_typec_pd_port_handle_t port, uint32_t mv, uint32_t ma)
{
    (void)port;
    (void)mv;
    (void)ma;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_typec_pd_get_contract(esp_typec_pd_port_handle_t port, esp_typec_pd_contract_t *out)
{
    (void)port;
    (void)out;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_typec_pd_get_orientation(esp_typec_pd_port_handle_t port_hdl, bool *cc2_active)
{
    pd_port_ctx_t *ctx = (pd_port_ctx_t *)port_hdl;
    if (!ctx || !cc2_active) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ctx->attached) {
        return ESP_ERR_INVALID_STATE;
    }
    *cc2_active = ctx->cc2_active;
    return ESP_OK;
}

esp_err_t esp_typec_pd_is_attached(esp_typec_pd_port_handle_t port_hdl, bool *attached)
{
    pd_port_ctx_t *ctx = (pd_port_ctx_t *)port_hdl;
    if (!ctx || !attached) {
        return ESP_ERR_INVALID_ARG;
    }
    *attached = ctx->attached;
    return ESP_OK;
}
