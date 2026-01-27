/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_log.h"

#include "esp_typec.h"     // public typec API (events, configs)
#include "typec_backend.h"
#include "fusb302_ctrl.h"     // backend for fusb302

#define TAG "esp_typec"

/* Define the event base */
ESP_EVENT_DEFINE_BASE(ESP_TYPEC_EVENT);

/* ---------- Per-port context ---------- */
typedef struct typec_port_backend {
    esp_err_t (*set_role)(void *dev, esp_typec_power_role_t role);
    esp_err_t (*service_irq)(void *dev, typec_evt_mask_t *events);
    esp_err_t (*get_status)(void *dev, typec_cc_status_t *st);
    esp_err_t (*commit_attach)(void *dev, bool cc2_active, bool is_source);
    esp_err_t (*deinit)(void *dev);
} typec_port_backend_t;

typedef struct typec_port_ctx {
    // Backend handle
    void *ctrl;
    const typec_port_backend_t *backend;
    i2c_master_dev_handle_t i2c_dev; // for cleanup
    // INT GPIO (active-low)
    gpio_num_t gpio_int;
    // Task
    TaskHandle_t task;
    // Cached state
    bool     attached;
    bool     cc2_active;
    uint32_t rp_ma;
    bool     vbus_logged;
    bool     pending_attach;
    bool     pending_attach_from_drp;
    // Current power role (sink/source/drp)
    esp_typec_power_role_t role;
    // Requested/policy role (used to decide whether to fall back to DRP)
    esp_typec_power_role_t policy_role;
    // Config snapshot
    esp_typec_port_config_t cfg;
} typec_port_ctx_t;

/* ---------- Tiny helpers ---------- */
static inline void typec_emit_attached(typec_port_ctx_t *ctx)
{
    esp_typec_evt_attached_t evt = {
        .port       = (esp_typec_port_handle_t)ctx,
        .cc2_active = ctx->cc2_active,
        .rp_cur_ma  = ctx->rp_ma,
    };

    esp_err_t err = esp_event_post(
                        ESP_TYPEC_EVENT,
                        ESP_TYPEC_EVENT_ID_ATTACHED,
                        &evt,
                        sizeof(evt),
                        portMAX_DELAY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to post typec ATTACHED event: %s", esp_err_to_name(err));
    }
}

static inline void typec_emit_detached(typec_port_ctx_t *ctx)
{
    (void)ctx; // if you don't yet have a payload

    esp_err_t err = esp_event_post(
                        ESP_TYPEC_EVENT,
                        ESP_TYPEC_EVENT_ID_DETACHED,
                        NULL,
                        0,
                        portMAX_DELAY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to post typec DETACHED event: %s", esp_err_to_name(err));
    }
}

static inline void typec_set_vbus_source(typec_port_ctx_t *ctx, bool enable)
{
    if (ctx->cfg.src_vbus_gpio != GPIO_NUM_NC) {
        gpio_set_level(ctx->cfg.src_vbus_gpio, enable); // active-high
    }
    if (ctx->cfg.src_vbus_gpio_n != GPIO_NUM_NC) {
        gpio_set_level(ctx->cfg.src_vbus_gpio_n, !enable); // active-low
    }
    ESP_LOGD(TAG, "VBUS=%d", enable);
}

static void typec_handle_events(typec_port_ctx_t *ctx, typec_evt_mask_t ev)
{
    if (!(ev & (TYPEC_EVT_TOG | TYPEC_EVT_CC | TYPEC_EVT_VBUS))) {
        return;
    }

    if (ctx->role == ESP_TYPEC_PWR_DRP) {
        if (!(ev & TYPEC_EVT_TOG)) {
            return;
        }

        typec_cc_status_t st = {0};
        esp_err_t st_err = ctx->backend->get_status(ctx->ctrl, &st);
        if (st_err != ESP_OK) {
            ESP_LOGE(TAG, "get_status (DRP) failed: %s", esp_err_to_name(st_err));
            return;
        }

        esp_typec_power_role_t next_role = ESP_TYPEC_PWR_DRP;
        if (st.tog_result == TYPEC_TOG_RESULT_SRC) {
            next_role = ESP_TYPEC_PWR_SOURCE;
        } else if (st.tog_result == TYPEC_TOG_RESULT_SNK) {
            next_role = ESP_TYPEC_PWR_SINK;
        } else {
            return;
        }

        if (next_role == ESP_TYPEC_PWR_SINK) {
            typec_set_vbus_source(ctx, false);
        }

        esp_err_t err = ctx->backend->set_role(ctx->ctrl, next_role);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "set_role(%d) failed: %s", (int)next_role, esp_err_to_name(err));
            return;
        }

        ctx->role = next_role;
        ctx->attached = false;
        ctx->cc2_active = false;
        ctx->rp_ma = 0;
        ctx->vbus_logged = false;
        ctx->pending_attach = false;
        ctx->pending_attach_from_drp = false;

        if (next_role == ESP_TYPEC_PWR_SOURCE) {
            if (ctx->backend->commit_attach) {
                esp_err_t c_err = ctx->backend->commit_attach(ctx->ctrl, st.cc2_active, true);
                if (c_err != ESP_OK) {
                    ESP_LOGE(TAG, "commit_attach failed: %s", esp_err_to_name(c_err));
                    return;
                }
            }
            ctx->attached   = true;
            ctx->cc2_active = st.cc2_active;
            ctx->rp_ma      = st.rp_cur_ma;

            typec_set_vbus_source(ctx, true);
            if (ev & TYPEC_EVT_VBUS) {
                ctx->vbus_logged = true;
            }
            typec_emit_attached(ctx);
            return;
        }

        if (next_role == ESP_TYPEC_PWR_SINK) {
            if (ctx->backend->commit_attach) {
                esp_err_t c_err = ctx->backend->commit_attach(ctx->ctrl, st.cc2_active, false);
                if (c_err != ESP_OK) {
                    ESP_LOGE(TAG, "commit_attach failed: %s", esp_err_to_name(c_err));
                    return;
                }
            }
            ctx->pending_attach = true;
            ctx->pending_attach_from_drp = true;
            ctx->vbus_logged = false;
            ctx->cc2_active = st.cc2_active;
            ctx->rp_ma = st.rp_cur_ma;
            if (ev & TYPEC_EVT_VBUS) {
                ctx->vbus_logged = true;
            }
            return;
        }
        return;
    }

    if (ctx->role == ESP_TYPEC_PWR_SOURCE) {
        if (!ctx->attached) {
            if (!(ev & TYPEC_EVT_TOG)) {
                return;
            }
        } else {
            if (!(ev & TYPEC_EVT_CC)) {
                return;
            }
        }

        typec_cc_status_t st = {0};
        esp_err_t st_err = ctx->backend->get_status(ctx->ctrl, &st);
        if (st_err != ESP_OK) {
            ESP_LOGE(TAG, "get_status (SRC) failed: %s", esp_err_to_name(st_err));
            return;
        }

        if (!ctx->attached) {
            if (st.tog_result != TYPEC_TOG_RESULT_SRC) {
                return;
            }

            if (ctx->backend->commit_attach) {
                esp_err_t c_err = ctx->backend->commit_attach(ctx->ctrl, st.cc2_active, true);
                if (c_err != ESP_OK) {
                    ESP_LOGE(TAG, "commit_attach failed: %s", esp_err_to_name(c_err));
                    return;
                }
            }
            ctx->attached   = true;
            ctx->cc2_active = st.cc2_active;
            ctx->rp_ma      = st.rp_cur_ma;
            ctx->vbus_logged = false;
            ctx->pending_attach = false;
            ctx->pending_attach_from_drp = false;

            typec_set_vbus_source(ctx, true);
            if (ev & TYPEC_EVT_VBUS) {
                ctx->vbus_logged = true;
            }
            typec_emit_attached(ctx);
            return;
        }

        if ((ev & TYPEC_EVT_VBUS) && !ctx->vbus_logged) {
            ctx->vbus_logged = true;
        }

        if (st.attached) {
            return;
        }

        ctx->attached = false;
        ctx->cc2_active = false;
        ctx->rp_ma = 0;
        ctx->vbus_logged = false;
        ctx->pending_attach = false;
        ctx->pending_attach_from_drp = false;

        typec_set_vbus_source(ctx, false);
        typec_emit_detached(ctx);

        if (ctx->policy_role == ESP_TYPEC_PWR_DRP) {
            esp_err_t err = ctx->backend->set_role(ctx->ctrl, ESP_TYPEC_PWR_DRP);
            if (err == ESP_OK) {
                ctx->role = ESP_TYPEC_PWR_DRP;
            } else {
                ESP_LOGE(TAG, "set_role(DRP) failed: %s", esp_err_to_name(err));
            }
        } else {
            esp_err_t err = ctx->backend->set_role(ctx->ctrl, ESP_TYPEC_PWR_SOURCE);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "set_role(SRC rearm) failed: %s", esp_err_to_name(err));
            }
        }
        return;
    }

    if (ctx->role == ESP_TYPEC_PWR_SINK) {
        if (ctx->pending_attach) {
            /* After commit_attach() we disable TOGGLE, so TOGSS is no longer meaningful here.
             * While pending_attach, only VBUS (and optionally CC) matters.
             */
            if (!(ev & (TYPEC_EVT_VBUS | TYPEC_EVT_CC))) {
                return;
            }

            typec_cc_status_t st = {0};
            esp_err_t st_err = ctx->backend->get_status(ctx->ctrl, &st);
            if (st_err != ESP_OK) {
                ESP_LOGE(TAG, "get_status (SNK pending) failed: %s", esp_err_to_name(st_err));
                return;
            }

            /* Optional cancel: if CC clearly disappeared while waiting for VBUS. */
            if ((ev & TYPEC_EVT_CC) && (st.rp_cur_ma == 0)) {
                ctx->pending_attach = false;
                ctx->pending_attach_from_drp = false;
                ctx->vbus_logged = false;
                // Re-arm toggle after commit_attach() disabled it.
                esp_err_t err = ctx->backend->set_role(ctx->ctrl, ESP_TYPEC_PWR_SINK);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "set_role(SNK rearm) failed: %s", esp_err_to_name(err));
                } else {
                    ctx->role = ESP_TYPEC_PWR_SINK;
                }
                return;
            }

            if (!st.vbus_ok) {
                if ((ev & TYPEC_EVT_VBUS) && !ctx->vbus_logged) {
                    ctx->vbus_logged = true;
                }
                return;
            }

            /* VBUS is present => now we are really attached as Sink. */
            ctx->attached = true;
            ctx->pending_attach = false;
            ctx->pending_attach_from_drp = false;
            ctx->rp_ma = st.rp_cur_ma;
            typec_emit_attached(ctx);
            return;
        }

        if (!ctx->attached) {
            if (!(ev & TYPEC_EVT_TOG)) {
                return;
            }
        } else {
            /* While attached, process VBUS for detach; CC can be used to track rp_ma updates. */
            if (!(ev & (TYPEC_EVT_VBUS | TYPEC_EVT_CC))) {
                return;
            }
        }

        typec_cc_status_t st = {0};
        esp_err_t st_err = ctx->backend->get_status(ctx->ctrl, &st);
        if (st_err != ESP_OK) {
            ESP_LOGE(TAG, "get_status (SNK) failed: %s", esp_err_to_name(st_err));
            return;
        }

        if (!ctx->attached) {
            if (st.tog_result != TYPEC_TOG_RESULT_SNK) {
                return;
            }

            /* Commit orientation now (disable toggle + route measurement to active CC). */
            if (ctx->backend->commit_attach) {
                esp_err_t c_err = ctx->backend->commit_attach(ctx->ctrl, st.cc2_active, false);
                if (c_err != ESP_OK) {
                    ESP_LOGE(TAG, "commit_attach (SNK) failed: %s", esp_err_to_name(c_err));
                    return;
                }
            }
            ctx->pending_attach = true;
            ctx->pending_attach_from_drp = false;
            ctx->vbus_logged = false;
            ctx->cc2_active = st.cc2_active;
            ctx->rp_ma = st.rp_cur_ma;
            if (ev & TYPEC_EVT_VBUS) {
                ctx->vbus_logged = true;
            }
            return;
        }

        /* Attached: detach based on VBUSOK drop. */
        if ((ev & TYPEC_EVT_VBUS) && !ctx->vbus_logged) {
            ctx->vbus_logged = true;
        }

        /* Optional: keep updating allowed current while attached. */
        if (ev & TYPEC_EVT_CC) {
            ctx->rp_ma = st.rp_cur_ma;
        }

        if (st.vbus_ok) {
            return;
        }

        ctx->attached = false;
        ctx->cc2_active = false;
        ctx->rp_ma = 0;
        ctx->vbus_logged = false;
        ctx->pending_attach = false;
        ctx->pending_attach_from_drp = false;
        typec_emit_detached(ctx);

        if (ctx->policy_role == ESP_TYPEC_PWR_DRP) {
            esp_err_t err = ctx->backend->set_role(ctx->ctrl, ESP_TYPEC_PWR_DRP);
            if (err == ESP_OK) {
                ctx->role = ESP_TYPEC_PWR_DRP;
            } else {
                ESP_LOGE(TAG, "set_role(DRP) failed: %s", esp_err_to_name(err));
            }
        } else {
            esp_err_t err = ctx->backend->set_role(ctx->ctrl, ESP_TYPEC_PWR_SINK);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "set_role(SNK rearm) failed: %s", esp_err_to_name(err));
            }
        }
    }
}

/* ---------- ISR & Task ---------- */

static void IRAM_ATTR typec_gpio_isr(void *arg)
{
    typec_port_ctx_t *ctx = (typec_port_ctx_t *)arg;
    // Gate further edges until the task drains the device
    gpio_intr_disable(ctx->gpio_int);
    BaseType_t hp = pdFALSE;
    vTaskNotifyGiveFromISR(ctx->task, &hp);
    if (hp) {
        portYIELD_FROM_ISR();
    }
}

static void typec_task(void *arg)
{
    typec_port_ctx_t *ctx = arg;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        typec_evt_mask_t ev = 0;

        esp_err_t irq_err = ctx->backend->service_irq(ctx->ctrl, &ev);
        if (irq_err != ESP_OK) {
            ESP_LOGE(TAG, "service_irq failed: %s", esp_err_to_name(irq_err));
            gpio_intr_enable(ctx->gpio_int);
            continue;
        }

        ESP_LOGD(TAG, "typec_task: policy=%d role=%d events=0x%02x attached=%d",
                 ctx->policy_role, ctx->role, ev, ctx->attached);
        typec_handle_events(ctx, ev);

        /* Handle rare stuck-low INT: drain and process any new events */
        if (gpio_get_level(ctx->gpio_int) == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            typec_evt_mask_t ev2 = 0;
            if (ctx->backend->service_irq(ctx->ctrl, &ev2) == ESP_OK && ev2) {
                ESP_LOGD(TAG, "typec_task: extra events=0x%02x attached=%d", ev2, ctx->attached);
                typec_handle_events(ctx, ev2);
            }
        }

        gpio_intr_enable(ctx->gpio_int);
    }
}

/* ---------- Public API ---------- */

esp_err_t esp_typec_install(const esp_typec_install_config_t *config)
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

esp_err_t esp_typec_uninstall(void)
{
    return ESP_OK;
}

static const typec_port_backend_t fusb302_backend = {
    .set_role = (esp_err_t (*)(void *, esp_typec_power_role_t))fusb302_set_role,
    .service_irq = (esp_err_t (*)(void *, typec_evt_mask_t *))fusb302_service_irq,
    .get_status  = (esp_err_t (*)(void *, typec_cc_status_t *))fusb302_get_status,
    .commit_attach = (esp_err_t (*)(void *, bool, bool))fusb302_commit_attach,
    .deinit = (esp_err_t (*)(void *))fusb302_deinit,
};

esp_err_t esp_typec_port_create_fusb302(const esp_typec_port_config_t *port_cfg,
                                        const esp_typec_fusb302_config_t *hw_cfg,
                                        esp_typec_port_handle_t *port_hdl_ret)
{
    esp_err_t ret = ESP_OK;
    bool isr_handler_added = false;
    ESP_RETURN_ON_FALSE(port_cfg && hw_cfg && port_hdl_ret,
                        ESP_ERR_INVALID_ARG, TAG, "bad args");
    esp_typec_rp_current_t rp = (port_cfg->rp_current <= ESP_TYPEC_RP_3A0) ?
                                port_cfg->rp_current : ESP_TYPEC_RP_DEFAULT;

    typec_port_ctx_t *ctx = (typec_port_ctx_t *)calloc(1, sizeof(*ctx));
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_NO_MEM, TAG, "no mem");

    ctx->cfg     = *port_cfg;
    ctx->cfg.rp_current = rp;
    ctx->backend = &fusb302_backend;
    ctx->policy_role = port_cfg->default_power_role;

    // Configure VBUS MOSFET GPIOs if present
    if (ctx->cfg.src_vbus_gpio != GPIO_NUM_NC) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << ctx->cfg.src_vbus_gpio,
                                 .mode         = GPIO_MODE_OUTPUT,
                                 .pull_up_en   = GPIO_PULLUP_DISABLE,
                                 .pull_down_en = GPIO_PULLDOWN_DISABLE,
                                 .intr_type    = GPIO_INTR_DISABLE,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io), fail, TAG, "src_vbus gpio_config failed");

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
        ESP_GOTO_ON_ERROR(gpio_config(&io2), fail, TAG, "src_vbus gpio_n config failed");

        // Ensure VBUS is OFF at startup (active-low)
        gpio_set_level(ctx->cfg.src_vbus_gpio_n, 1);
    }

    if ((ctx->cfg.default_power_role == ESP_TYPEC_PWR_SOURCE ||
            ctx->cfg.default_power_role == ESP_TYPEC_PWR_DRP) &&
            ctx->cfg.src_vbus_gpio == GPIO_NUM_NC &&
            ctx->cfg.src_vbus_gpio_n == GPIO_NUM_NC) {
        ESP_LOGW(TAG, "Source/DRP role requested but no VBUS GPIOs are set; VBUS will not be driven");
    }

    // Create I2C device for FUSB302 on provided bus
    i2c_device_config_t dev_cfg = {
        .device_address = hw_cfg->i2c_addr_7b,
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .scl_speed_hz = 400000, // default to 400 kHz
    };
    ESP_GOTO_ON_ERROR(i2c_master_bus_add_device(hw_cfg->i2c_bus, &dev_cfg, &ctx->i2c_dev),
                      fail, TAG, "fusb add failed");

    // Init backend
    fusb302_hw_cfg_t hw = {
        .i2c_dev   = ctx->i2c_dev,
        .gpio_int  = hw_cfg->gpio_int,
        .rp_current = rp,
    };
    fusb302_dev_t *dev = NULL;
    ESP_GOTO_ON_ERROR(fusb302_init(&hw, &dev), fail, TAG, "ctrl init failed");
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

    // Configure INT GPIO as input with pull-up and low-level interrupt
    gpio_config_t gc = {
        .pin_bit_mask = 1ULL << ctx->gpio_int,
                             .mode = GPIO_MODE_INPUT,
                             .pull_up_en = GPIO_PULLUP_ENABLE,
                             .pull_down_en = GPIO_PULLDOWN_DISABLE,
                             .intr_type = GPIO_INTR_LOW_LEVEL,
    };
    ESP_GOTO_ON_ERROR(gpio_config(&gc), fail, TAG, "gpio_config");

    esp_err_t isr_ret = gpio_install_isr_service(0);
    if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) {
        ret = isr_ret;
        goto fail;
    }
    ESP_GOTO_ON_ERROR(gpio_isr_handler_add(ctx->gpio_int, typec_gpio_isr, ctx), fail, TAG, "gpio_isr_add");
    isr_handler_added = true;

    // Create task
    const uint32_t stack_words = (port_cfg->task_stack ? port_cfg->task_stack : 4096) / sizeof(StackType_t);
    const UBaseType_t prio     = (port_cfg->task_prio ? port_cfg->task_prio : (tskIDLE_PRIORITY + 2));

    BaseType_t ok = xTaskCreate(typec_task, "typec_fusb302", stack_words, ctx, prio, &ctx->task);
    if (ok != pdPASS) {
        gpio_isr_handler_remove(ctx->gpio_int);
        ctx->backend->deinit(ctx->ctrl);
        if (ctx->i2c_dev) {
            i2c_master_bus_rm_device(ctx->i2c_dev);
            ctx->i2c_dev = NULL;
        }
        free(ctx);
        return ESP_ERR_NO_MEM;
    }

    /* Drain any pre-existing latched causes before arming GPIO */
    typec_evt_mask_t ev = 0;

    ESP_GOTO_ON_ERROR(ctx->backend->service_irq(ctx->ctrl, &ev), fail, TAG, "prime irq");

    /* Now arm the GPIO */
    gpio_intr_enable(ctx->gpio_int);

    *port_hdl_ret = (esp_typec_port_handle_t)ctx;
    ESP_LOGI(TAG, "Type-C task started");
    return ESP_OK;
fail:
    ESP_LOGE(TAG, "failed, err=%d", ret);
    if (ctx) {
        if (ctx->task) {
            vTaskDelete(ctx->task);
            ctx->task = NULL;
        }
        if (isr_handler_added) {
            gpio_intr_disable(ctx->gpio_int);
            gpio_isr_handler_remove(ctx->gpio_int);
        }
        if (ctx->ctrl) {
            ctx->backend->deinit(ctx->ctrl);
            ctx->ctrl = NULL;
        }
        if (ctx->i2c_dev) {
            i2c_master_bus_rm_device(ctx->i2c_dev);
            ctx->i2c_dev = NULL;
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

    gpio_intr_disable(ctx->gpio_int);
    gpio_isr_handler_remove(ctx->gpio_int);

    if (ctx->task) {
        vTaskDelete(ctx->task);
        ctx->task = NULL;
    }

    if (ctx->ctrl && ctx->backend) {
        ctx->backend->deinit(ctx->ctrl);
        ctx->ctrl = NULL;
    }
    if (ctx->i2c_dev) {
        i2c_master_bus_rm_device(ctx->i2c_dev);
        ctx->i2c_dev = NULL;
    }

    free(ctx);
    return ESP_OK;
}

esp_err_t esp_typec_set_power_role(esp_typec_port_handle_t port_hdl,
                                   esp_typec_power_role_t role)
{
    typec_port_ctx_t *ctx = (typec_port_ctx_t *)port_hdl;
    if (!ctx || !ctx->backend) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ctx->backend->set_role(ctx->ctrl, role);
    if (err != ESP_OK) {
        return err;
    }

    ctx->role = role;
    ctx->policy_role = role;
    ctx->attached = false;
    ctx->cc2_active = false;
    ctx->rp_ma = 0;
    ctx->vbus_logged = false;
    ctx->pending_attach = false;
    ctx->pending_attach_from_drp = false;

    // Simple safety policy: only explicit SOURCE keeps VBUS on
    if (role != ESP_TYPEC_PWR_SOURCE) {
        typec_set_vbus_source(ctx, false);
    }

    return ESP_OK;
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
