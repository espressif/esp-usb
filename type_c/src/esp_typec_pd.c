/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"

#include "esp_typec_pd.h"
#include "fusb302_ctrl.h"  // internal controller interface

#define TAG "esp_typec_pd"

/* ---------------- Library singleton ---------------- */
static bool s_lib_installed = false;

/* ---------------- Per-port context ----------------- */
typedef struct pd_port_ctx {
    // controller
    fusb302_dev_t *ctrl;

    // task
    TaskHandle_t task;

    // user callback
    esp_typec_pd_event_cb_t cb;
    void *cb_arg;

    // cached state
    bool attached;
    bool cc2_active;
    uint32_t rp_ma;

    // config snapshot
    esp_typec_pd_port_config_t cfg;

} pd_port_ctx_t;

static void pd_task(void *arg);

/* ---------------- Public API ----------------------- */

esp_err_t esp_typec_pd_install(const esp_typec_pd_install_config_t *config)
{
    (void)config;
    if (s_lib_installed) {
        return ESP_ERR_INVALID_STATE;
    }
    s_lib_installed = true;
    return ESP_OK;
}

esp_err_t esp_typec_pd_uninstall(void)
{
    if (!s_lib_installed) {
        return ESP_ERR_INVALID_STATE;
    }
    s_lib_installed = false;
    return ESP_OK;
}

esp_err_t esp_typec_pd_lib_handle_events(TickType_t timeout_ticks, uint32_t *event_flags_ret)
{
    (void)timeout_ticks; (void)event_flags_ret;
    // Polling design needs no library-level pump for now
    return ESP_OK;
}

esp_err_t esp_typec_pd_lib_info(esp_typec_pd_lib_info_t *info_ret)
{
    if (!info_ret) {
        return ESP_ERR_INVALID_ARG;
    }
    info_ret->num_ports = 0; // TODO: maintain count when creating/destroying ports
    return ESP_OK;
}

esp_err_t esp_typec_pd_port_create_fusb302(const esp_typec_pd_port_config_t *port_cfg,
                                           const esp_typec_pd_fusb302_config_t *hw_cfg,
                                           esp_typec_pd_event_cb_t cb, void *cb_arg,
                                           esp_typec_pd_port_handle_t *port_hdl_ret)
{
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_FALSE(port_cfg && hw_cfg && cb && port_hdl_ret, ESP_ERR_INVALID_ARG, TAG, "bad args");

    *port_hdl_ret = NULL; // avoid stale handle on failure

    pd_port_ctx_t *ctx = calloc(1, sizeof(*ctx));
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_NO_MEM, TAG, "no mem");

    ctx->cfg     = *port_cfg;
    ctx->cb      = cb;
    ctx->cb_arg  = cb_arg;

    fusb302_hw_cfg_t hw = {
        .i2c_port = hw_cfg->i2c_port,
        .i2c_addr = hw_cfg->i2c_addr,
        .gpio_int = hw_cfg->gpio_int,
        .use_intr = false, // start with polling; enable later
    };
    ESP_GOTO_ON_ERROR(fusb302_init(&hw, &ctx->ctrl), fail, TAG, "ctrl init failed");

    fusb302_role_t role = FUSB302_ROLE_SINK;
    switch (port_cfg->default_power_role) {
    case ESP_TYPEC_PD_PWR_SOURCE: role = FUSB302_ROLE_SOURCE; break;
    case ESP_TYPEC_PD_PWR_DRP:    role = FUSB302_ROLE_DRP;    break;
    default: break; // sink
    }
    ESP_GOTO_ON_ERROR(fusb302_set_role(ctx->ctrl, role), fail, TAG, "set role failed");

    BaseType_t ok = xTaskCreate(pd_task, "pd_fusb302",
                                (port_cfg->task_stack ? port_cfg->task_stack : 4096) / sizeof(StackType_t),
                                ctx,
                                (port_cfg->task_prio ? port_cfg->task_prio : (tskIDLE_PRIORITY + 2)),
                                &ctx->task);
    ESP_GOTO_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, fail, TAG, "task alloc failed");

    *port_hdl_ret = (esp_typec_pd_port_handle_t)ctx;
    return ESP_OK;

fail:
    if (ctx) {
        if (ctx->task) {
            vTaskDelete(ctx->task);
            ctx->task = NULL;
        }
        if (ctx->ctrl) {
            fusb302_deinit(ctx->ctrl);
            ctx->ctrl = NULL;
        }
        free(ctx);
    }
    return ret; // preserve specific error from ESP_GOTO_ON_ERROR
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
    if (ctx->ctrl) {
        fusb302_deinit(ctx->ctrl);
        ctx->ctrl = NULL;
    }
    free(ctx);
    return ESP_OK;
}

esp_err_t esp_typec_pd_set_power_role(esp_typec_pd_port_handle_t port_hdl, esp_typec_pd_power_role_t role)
{
    pd_port_ctx_t *ctx = (pd_port_ctx_t *)port_hdl;
    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }

    fusb302_role_t r = FUSB302_ROLE_SINK;
    switch (role) {
    case ESP_TYPEC_PD_PWR_SOURCE: r = FUSB302_ROLE_SOURCE; break;
    case ESP_TYPEC_PD_PWR_DRP:    r = FUSB302_ROLE_DRP;    break;
    default: break;
    }
    return fusb302_set_role(ctx->ctrl, r);
}

/* The following APIs remain stubs until PRL/PE is implemented */
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

/* ---------------- Task & helpers --------------------- */

static inline void pd_emit_attached(pd_port_ctx_t *ctx)
{
    if (!ctx->cb) {
        return;
    }
    esp_typec_pd_evt_attached_t a = {
        .flags = ctx->cc2_active ? ESP_TYPEC_PD_FLAG_CC2 : 0,
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

static void pd_commit_status(pd_port_ctx_t *ctx, const fusb302_cc_status_t *st)
{
    if (!ctx->attached && st->attached) {
        ctx->attached   = true;
        ctx->cc2_active = st->cc2_active;
        ctx->rp_ma      = st->rp_cur_ma;
        pd_emit_attached(ctx);
    } else if (ctx->attached && !st->attached) {
        ctx->attached = false;
        pd_emit_detached(ctx);
    } else if (ctx->attached && (ctx->cc2_active != st->cc2_active)) {
        ctx->cc2_active = st->cc2_active;
        // Optional: add a PD ORIENTATION event later if desired
    }
}

static void pd_task(void *arg)
{
    pd_port_ctx_t *ctx = (pd_port_ctx_t *)arg;

    const TickType_t period = pdMS_TO_TICKS(50); // polling + basic debounce
    TickType_t last = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last, period);

        fusb302_cc_status_t st = {0};
        if (fusb302_read_cc_status(ctx->ctrl, &st) == ESP_OK) {
            pd_commit_status(ctx, &st);
        }
    }
}
