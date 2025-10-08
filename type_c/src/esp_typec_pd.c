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

/* Define the event base */
ESP_EVENT_DEFINE_BASE(ESP_TYPEC_PD_EVENT);

/* ---------- Per-port context ---------- */
typedef struct pd_port_backend {
    esp_err_t (*set_role)(void *dev, esp_typec_power_role_t role);
    esp_err_t (*service_irq)(void *dev, typec_evt_mask_t *events);
    esp_err_t (*get_status)(void *dev, typec_cc_status_t *st);
    esp_err_t (*deinit)(void *dev);
} pd_port_backend_t;

typedef struct pd_port_ctx {
    // Backend handle
    void *ctrl;
    const pd_port_backend_t *backend;
    // INT GPIO (active-low)
    int gpio_int;
    // Task
    TaskHandle_t task;
    // Cached state
    bool     attached;
    bool     cc2_active;
    uint32_t rp_ma;
    // Current power role (sink/source/drp)
    esp_typec_power_role_t role;
    // Config snapshot
    esp_typec_pd_port_config_t cfg;
} pd_port_ctx_t;


/* ---------- Tiny helpers ---------- */
static inline void pd_emit_attached(pd_port_ctx_t *ctx)
{
    esp_typec_pd_evt_attached_t evt = {
        .port       = (esp_typec_pd_port_handle_t)ctx,
        .cc2_active = ctx->cc2_active,
        .rp_cur_ma  = ctx->rp_ma,
    };

    esp_err_t err = esp_event_post(
                        ESP_TYPEC_PD_EVENT,
                        ESP_TYPEC_PD_EVENT_ID_ATTACHED,
                        &evt,
                        sizeof(evt),
                        portMAX_DELAY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to post PD ATTACHED event: %s", esp_err_to_name(err));
    }
}


static inline void pd_emit_detached(pd_port_ctx_t *ctx)
{
    (void)ctx; // if you don’t yet have a payload

    esp_err_t err = esp_event_post(
                        ESP_TYPEC_PD_EVENT,
                        ESP_TYPEC_PD_EVENT_ID_DETACHED,
                        NULL,
                        0,
                        portMAX_DELAY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to post PD DETACHED event: %s", esp_err_to_name(err));
    }
}

static inline void pd_set_vbus_source(pd_port_ctx_t *ctx, bool enable)
{
    if (ctx->cfg.src_vbus_gpio != GPIO_NUM_NC) {
        gpio_set_level(ctx->cfg.src_vbus_gpio, enable); // active-high
    }
    if (ctx->cfg.src_vbus_gpio_n != GPIO_NUM_NC) {
        gpio_set_level(ctx->cfg.src_vbus_gpio_n, enable ? 0 : 1); // active-low
    }
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
        typec_cc_status_t st = (typec_cc_status_t) {
            0
        };

        /* Drain IRQs and get a fresh snapshot in one call */
        esp_err_t irq_err = ctx->backend->service_irq(ctx->ctrl, &ev);
        if (irq_err != ESP_OK) {
            ESP_LOGE(TAG, "service_irq failed: %s", esp_err_to_name(irq_err));
            gpio_intr_enable(ctx->gpio_int);
            continue;
        }
        ESP_LOGI(TAG, "pd_task: role=%d events=0x%02x attached=%d",
                 ctx->role, ev, ctx->attached);

        if (ctx->role == ESP_TYPEC_PWR_SOURCE) {
            /* -------- SOURCE policy: CC-driven attach/detach + VBUS MOSFET -------- */
            if (ev & PD_EVT_CC) {
                ctx->backend->get_status(ctx->ctrl, &st);
                if (st.attached && !ctx->attached) {
                    // Sink just appeared on one of the CC pins
                    ctx->attached   = true;
                    ctx->cc2_active = st.cc2_active;
                    ctx->rp_ma      = st.rp_cur_ma;

                    // Now it’s safe to start sourcing 5 V
                    pd_set_vbus_source(ctx, true);
                    pd_emit_attached(ctx);

                } else if (!st.attached && ctx->attached) {
                    // Cable removed / sink detached
                    ctx->attached = false;
                    pd_set_vbus_source(ctx, false);
                    pd_emit_detached(ctx);

                } else if (ctx->attached) {
                    // Orientation or advertised current changed while attached
                    ctx->cc2_active = st.cc2_active;
                    ctx->rp_ma      = st.rp_cur_ma;
                }
            }

        } else {
            /* -------- SINK policy: VBUS-driven attach/detach only -------- */
            if (ev & PD_EVT_VBUS) {
                // First, read current VBUS/CC state
                ctx->backend->get_status(ctx->ctrl, &st);

                if (st.vbus_ok && !ctx->attached) {
                    // VBUS became valid: give CC comparators time to settle
                    vTaskDelay(pdMS_TO_TICKS(30));

                    // Re-read after the delay for orientation + Rp
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

            // IMPORTANT: in sink mode we ignore PD_EVT_CC completely
        }

        /* Handle rare stuck-low INT */
        if (gpio_get_level(ctx->gpio_int) == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            irq_err = ctx->backend->service_irq(ctx->ctrl, &ev);
            if (irq_err != ESP_OK) {
                ESP_LOGE(TAG, "service_irq retry failed: %s", esp_err_to_name(irq_err));
            }
        }

        gpio_intr_enable(ctx->gpio_int);
    }
}

/* ---------- Public API ---------- */

esp_err_t esp_typec_pd_install(const esp_typec_pd_install_config_t *config)
{
    (void)config;

    // Make sure the default event loop exists.
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to create default event loop: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}


esp_err_t esp_typec_pd_uninstall(void)
{
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
    .service_irq = (esp_err_t (*)(void *, typec_evt_mask_t *))fusb302_service_irq,
    .get_status  = (esp_err_t (*)(void *, typec_cc_status_t *))fusb302_get_status,
    .deinit = (esp_err_t (*)(void *))fusb302_deinit,
};

esp_err_t esp_typec_pd_port_create_fusb302(const esp_typec_pd_port_config_t *port_cfg,
                                           const esp_typec_pd_fusb302_config_t *hw_cfg,
                                           esp_typec_pd_port_handle_t *port_hdl_ret)
{
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_FALSE(port_cfg && hw_cfg && port_hdl_ret,
                        ESP_ERR_INVALID_ARG, TAG, "bad args");
    esp_typec_rp_current_t rp = (port_cfg->rp_current <= ESP_TYPEC_RP_3A0) ?
                                port_cfg->rp_current : ESP_TYPEC_RP_DEFAULT;

    pd_port_ctx_t *ctx = (pd_port_ctx_t *)calloc(1, sizeof(*ctx));
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_NO_MEM, TAG, "no mem");

    ctx->cfg     = *port_cfg;
    ctx->cfg.rp_current = rp;
    ctx->cfg.src_vbus_gpio_n = (ctx->cfg.src_vbus_gpio_n == 0) ? GPIO_NUM_NC : ctx->cfg.src_vbus_gpio_n;
    ctx->backend = &fusb302_backend;

    // Configure VBUS MOSFET GPIOs if present
    if (ctx->cfg.src_vbus_gpio != GPIO_NUM_NC) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << ctx->cfg.src_vbus_gpio,
                                 .mode         = GPIO_MODE_OUTPUT,
                                 .pull_up_en   = GPIO_PULLUP_DISABLE,
                                 .pull_down_en = GPIO_PULLDOWN_DISABLE,
                                 .intr_type    = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "src_vbus gpio_config failed");

        // Ensure VBUS is OFF at startup (active-high)
        gpio_set_level(ctx->cfg.src_vbus_gpio, 0);
    }
    if (ctx->cfg.src_vbus_gpio_n != GPIO_NUM_NC) {
        gpio_config_t io2 = {
            .pin_bit_mask = 1ULL << ctx->cfg.src_vbus_gpio_n,
                                 .mode         = GPIO_MODE_OUTPUT,
                                 .pull_up_en   = GPIO_PULLUP_DISABLE,
                                 .pull_down_en = GPIO_PULLDOWN_DISABLE,
                                 .intr_type    = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&io2), TAG, "src_vbus gpio_n config failed");

        // Ensure VBUS is OFF at startup (active-low)
        gpio_set_level(ctx->cfg.src_vbus_gpio_n, 1);
    }

    if ((ctx->cfg.default_power_role == ESP_TYPEC_PWR_SOURCE ||
            ctx->cfg.default_power_role == ESP_TYPEC_PWR_DRP) &&
            ctx->cfg.src_vbus_gpio == GPIO_NUM_NC &&
            ctx->cfg.src_vbus_gpio_n == GPIO_NUM_NC) {
        ESP_LOGW(TAG, "Source/DRP role requested but no VBUS GPIOs are set; VBUS will not be driven");
    }

    // Init backend
    fusb302_hw_cfg_t hw = {
        .i2c_dev  = hw_cfg->i2c_dev,   // NEW: i2c_master_dev_handle_t
        .gpio_int = hw_cfg->gpio_int,
    };
    fusb302_dev_t *dev = NULL;
    ESP_RETURN_ON_ERROR(fusb302_init(&hw, &dev), TAG, "ctrl init failed");
    ctx->ctrl = dev;
    // apply initial role from config -----
    esp_typec_power_role_t role = port_cfg->default_power_role;
    if (role == ESP_TYPEC_PWR_SOURCE || role == ESP_TYPEC_PWR_DRP) {
        // only if the role is not default sink
        ESP_GOTO_ON_ERROR(ctx->backend->set_role(ctx->ctrl, role),
                          fail, TAG, "set role failed");
    }
    ctx->role = role;
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

    ESP_GOTO_ON_ERROR(ctx->backend->service_irq(ctx->ctrl, &ev), fail, TAG, "prime irq");

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
    if (role == ESP_TYPEC_PWR_DRP) {
        return ESP_ERR_NOT_SUPPORTED;// TODO: add DRP support later
    }

    pd_port_ctx_t *ctx = (pd_port_ctx_t *)port_hdl;
    if (!ctx || !ctx->backend) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ctx->backend->set_role(ctx->ctrl, role);
    if (err != ESP_OK) {
        return err;
    }

    ctx->role = role;

    // Simple safety policy: only SOURCE and DRP is allowed to drive VBUS
    if (role != ESP_TYPEC_PWR_SOURCE && role != ESP_TYPEC_PWR_DRP) {
        pd_set_vbus_source(ctx, false);
    }

    return ESP_OK;
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
