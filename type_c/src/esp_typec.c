/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_check.h"

#include "esp_typec.h"
#include "husb320_ctrl.h"   // internal

#define TAG "esp_typec"
typedef struct typec_port_ctx {
    // controller
    husb320_dev_t *ctrl;

    // tasking
    TaskHandle_t task;
    QueueHandle_t q;

    // user cb
    esp_typec_event_cb_t cb;
    void *cb_arg;

    // cached state
    bool attached;
    bool cc2_active;
    uint32_t rp_ma;

    // config snapshot
    esp_typec_port_config_t cfg;

} typec_port_ctx_t;

typedef enum {
    TC_EVT_POLL = 1,
} tc_evt_t;

// ---- Forward ----
static void typec_task(void *arg);
static void typec_commit_status(typec_port_ctx_t *ctx, const husb320_cc_status_t *st);

// ------------------------------------------------- Public API -------------------------------------------------

esp_err_t esp_typec_install(void)
{
    return ESP_OK;
}

esp_err_t esp_typec_uninstall(void)
{
    return ESP_OK;
}

esp_err_t esp_typec_lib_info(esp_typec_lib_info_t *info)
{
    if (!info) {
        return ESP_ERR_INVALID_ARG;
    }
    // TODO: track number of ports created
    info->num_ports = 0;
    return ESP_OK;
}

esp_err_t esp_typec_port_create_husb320(const esp_typec_port_config_t *port_cfg,
                                        const esp_typec_husb320_config_t *hw_cfg,
                                        esp_typec_event_cb_t cb, void *cb_arg,
                                        esp_typec_port_handle_t *port_hdl_ret)
{
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_FALSE(port_cfg && hw_cfg && cb && port_hdl_ret, ESP_ERR_INVALID_ARG, TAG, "bad args");

    typec_port_ctx_t *ctx = calloc(1, sizeof(*ctx));
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_NO_MEM, TAG, "no mem");

    ctx->cfg   = *port_cfg;
    ctx->cb    = cb;
    ctx->cb_arg = cb_arg;

    husb320_hw_cfg_t hw = {
        .i2c_port = hw_cfg->i2c_port,
        .i2c_addr = hw_cfg->i2c_addr,
        .gpio_int = hw_cfg->gpio_int,
        .use_intr = false, // start with polling; IRQ later
    };
    ESP_GOTO_ON_ERROR(husb320_init(&hw, &ctx->ctrl), fail, TAG, "ctrl init failed");

    // Apply initial role
    esp_typec_power_role_t role = ESP_TYPEC_PWR_SINK;
    switch (port_cfg->default_power_role) {
    case ESP_TYPEC_PWR_SOURCE: role = ESP_TYPEC_PWR_SOURCE; break;
    case ESP_TYPEC_PWR_DRP:    role = ESP_TYPEC_PWR_DRP;    break;
    default: break;
    }
    ESP_GOTO_ON_ERROR(husb320_set_role(ctx->ctrl, role), fail, TAG, "set role failed");

    // Create queue and task
    ctx->q = xQueueCreate(4, sizeof(uint32_t));
    ESP_GOTO_ON_FALSE(ctx->q, ESP_ERR_NO_MEM, fail, TAG, "queue alloc failed");

    BaseType_t ok = xTaskCreate(typec_task, "typec_husb320",
                                (port_cfg->task_stack ? port_cfg->task_stack : 3072) / sizeof(StackType_t),
                                ctx,
                                (port_cfg->task_prio ? port_cfg->task_prio : tskIDLE_PRIORITY + 2),
                                &ctx->task);
    ESP_GOTO_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, fail, TAG, "task alloc failed");

    // Kick initial poll
    uint32_t e = TC_EVT_POLL;
    (void)xQueueSend(ctx->q, &e, 0);

    *port_hdl_ret = (esp_typec_port_handle_t)ctx;
    return ret;

fail:
    if (ctx) {
        if (ctx->ctrl) {
            husb320_deinit(ctx->ctrl);
        }
        if (ctx->q) {
            vQueueDelete(ctx->q);
        }
        free(ctx);
    }
    return ret;
}

esp_err_t esp_typec_port_destroy(esp_typec_port_handle_t port_hdl)
{
    typec_port_ctx_t *ctx = (typec_port_ctx_t *)port_hdl;
    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }

    if (ctx->task) {
        vTaskDelete(ctx->task);
        ctx->task = NULL;
    }
    if (ctx->q) {
        vQueueDelete(ctx->q);
        ctx->q = NULL;
    }
    if (ctx->ctrl) {
        husb320_deinit(ctx->ctrl);
        ctx->ctrl = NULL;
    }
    free(ctx);
    return ESP_OK;
}

esp_err_t esp_typec_set_power_role(esp_typec_port_handle_t port_hdl, esp_typec_power_role_t role)
{
    typec_port_ctx_t *ctx = (typec_port_ctx_t *)port_hdl;
    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }
    return husb320_set_role(ctx->ctrl, role);
}

esp_err_t esp_typec_get_orientation(esp_typec_port_handle_t port_hdl, bool *cc2_active)
{
    typec_port_ctx_t *ctx = (typec_port_ctx_t *)port_hdl;
    if (!ctx || !cc2_active) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ctx->attached) {
        return ESP_ERR_INVALID_STATE;
    }
    *cc2_active = ctx->cc2_active;
    return ESP_OK;
}

esp_err_t esp_typec_is_attached(esp_typec_port_handle_t port_hdl, bool *attached)
{
    typec_port_ctx_t *ctx = (typec_port_ctx_t *)port_hdl;
    if (!ctx || !attached) {
        return ESP_ERR_INVALID_ARG;
    }
    *attached = ctx->attached;
    return ESP_OK;
}

// ------------------------------------------------- Task & helpers -------------------------------------------------

static void typec_emit_attached(typec_port_ctx_t *ctx)
{
    if (!ctx->cb) {
        return;
    }
    esp_typec_evt_attached_t a = {
        .flags     = ctx->cc2_active ? ESP_TYPEC_FLAG_CC2 : 0,
        .rp_cur_ma = ctx->rp_ma,
    };
    ctx->cb(ESP_TYPEC_EVENT_ATTACHED, &a, ctx->cb_arg);
}

static void typec_emit_detached(typec_port_ctx_t *ctx)
{
    if (!ctx->cb) {
        return;
    }
    ctx->cb(ESP_TYPEC_EVENT_DETACHED, NULL, ctx->cb_arg);
}

static void typec_emit_orientation(typec_port_ctx_t *ctx)
{
    if (!ctx->cb) {
        return;
    }
    bool cc2 = ctx->cc2_active;
    ctx->cb(ESP_TYPEC_EVENT_ORIENTATION, &cc2, ctx->cb_arg);
}

static void typec_commit_status(typec_port_ctx_t *ctx, const husb320_cc_status_t *st)
{
    if (!ctx->attached && st->attached) {
        ctx->attached   = true;
        ctx->cc2_active = st->cc2_active;
        ctx->rp_ma      = st->rp_cur_ma;
        typec_emit_attached(ctx);
    } else if (ctx->attached && !st->attached) {
        ctx->attached = false;
        typec_emit_detached(ctx);
    } else if (ctx->attached && (ctx->cc2_active != st->cc2_active)) {
        ctx->cc2_active = st->cc2_active;
        typec_emit_orientation(ctx);
    }
}

static void typec_task(void *arg)
{
    typec_port_ctx_t *ctx = (typec_port_ctx_t *)arg;

    const TickType_t period = pdMS_TO_TICKS(50); // simple debounce/poll period
    TickType_t last = xTaskGetTickCount();

    for (;;) {
        // periodic poll (no IRQ yet)
        vTaskDelayUntil(&last, period);

        husb320_cc_status_t st = {0};
        if (husb320_read_cc_status(ctx->ctrl, &st) == ESP_OK) {
            typec_commit_status(ctx, &st);
        }
    }
}
