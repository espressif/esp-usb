/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "usb/usb_tcpm.h"
#include "usb_tcpm_backend.h"

static const char *TAG = "usb_tcpm";

/* Define the event base */
ESP_EVENT_DEFINE_BASE(USB_TCPM_EVENT);

/* ---------- Per-port context ---------- */
/**
 * @brief Per-port runtime context.
 */
typedef struct typec_port {
    struct {
        struct typec_port *next; /**< Next port in global list. */
        bool irq_pending; /**< IRQ pending for shared worker. */
        bool irq_processing; /**< Shared worker is processing this port. */
        bool deleting; /**< Port is being deleted. */
    } dynamic; /**< Fields protected by s_lock. */

    struct {
        bool attached; /**< True if attached. */
        bool cc2_active; /**< True if CC2 is active. */
        uint32_t rp_current_ma; /**< Advertised/observed Rp current in mA. */
        usb_tcpm_power_role_t role; /**< Current power role. */
    } mux_protected; /**< Fields protected by status_lock. */

    struct {
        bool pending_attach; /**< True while waiting for VBUS after commit. */
        bool source_detach_grace_active; /**< True while source detach detection is in startup grace period. */
        TickType_t source_detach_grace_start_tick; /**< Tick when source detach grace was armed. */
    } single_thread; /**< Accessed by shared worker task. */

    struct {
        usb_tcpm_power_role_t policy_role; /**< Policy role for fallback decisions. */
        void *backend_ctx; /**< Backend device handle. */
        const typec_port_backend_t *backend; /**< Backend operations. */
        gpio_num_t gpio_int; /**< INT GPIO (active-low). */
        usb_tcpm_port_config_t cfg; /**< Configuration snapshot. */
    } constant; /**< Initialized during create, then treated as read-only. */

    struct {
        StaticSemaphore_t status_lock_storage; /**< Storage for status lock. */
        SemaphoreHandle_t status_lock; /**< Protects status snapshot fields. */
    } locks;
} typec_port_t;

#define USB_TCPM_TASK_STACK_DEFAULT (4096)
#define USB_TCPM_TASK_PRIO_DEFAULT  (tskIDLE_PRIORITY + 2)
/*
 * Bus-powered sinks can briefly disturb CC while local rails and the sink
 * controller are coming up after VBUS is enabled. Mask source detach checks
 * for a short period after source attach to avoid false detach.
 */
#define USB_TCPM_SRC_DETACH_GRACE_MS (200)

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static typec_port_t *s_ports;
static TaskHandle_t s_task;

static void usb_tcpm_task(void *arg);

static void usb_tcpm_port_list_add(typec_port_t *ctx)
{
    portENTER_CRITICAL(&s_lock);
    ctx->dynamic.next = s_ports;
    s_ports = ctx;
    portEXIT_CRITICAL(&s_lock);
}

static void usb_tcpm_port_list_remove(typec_port_t *ctx)
{
    portENTER_CRITICAL(&s_lock);
    typec_port_t **port_cursor = &s_ports;
    while (*port_cursor && *port_cursor != ctx) {
        port_cursor = &(*port_cursor)->dynamic.next;
    }
    if (*port_cursor == ctx) {
        *port_cursor = ctx->dynamic.next;
    }
    ctx->dynamic.next = NULL;
    portEXIT_CRITICAL(&s_lock);
}

#define USB_TCPM_STATUS_LOCK(ctx) do { \
    (void)xSemaphoreTake((ctx)->locks.status_lock, portMAX_DELAY); \
} while (0)
#define USB_TCPM_STATUS_UNLOCK(ctx) do { \
    (void)xSemaphoreGive((ctx)->locks.status_lock); \
} while (0)

/* ---------- Tiny helpers ---------- */
static inline void usb_tcpm_emit_attached(const typec_port_t *ctx)
{
    usb_tcpm_evt_attached_t evt = {
        .port       = (usb_tcpm_port_handle_t)ctx,
        .cc2_active = ctx->mux_protected.cc2_active,
        .rp_current_ma = ctx->mux_protected.rp_current_ma,
    };
    usb_tcpm_event_id_t event_id;
    if (ctx->mux_protected.role == USB_TCPM_PWR_SOURCE) {
        event_id = USB_TCPM_EVENT_SOURCE_ATTACHED;
    } else if (ctx->mux_protected.role == USB_TCPM_PWR_SINK) {
        event_id = USB_TCPM_EVENT_SINK_ATTACHED;
    } else {
        ESP_LOGW(TAG, "Attached event with non-source/sink role=%d", (int)ctx->mux_protected.role);
        return;
    }

    const esp_err_t err = esp_event_post(USB_TCPM_EVENT,
                                         event_id,
                                         &evt,
                                         sizeof(evt),
                                         portMAX_DELAY);
    ESP_RETURN_VOID_ON_ERROR(err, TAG, "Failed to post typec ATTACHED event: %s", esp_err_to_name(err));
}

static inline void usb_tcpm_emit_detached(const typec_port_t *ctx)
{
    usb_tcpm_evt_detached_t evt = {
        .port = (usb_tcpm_port_handle_t)ctx,
    };
    usb_tcpm_event_id_t event_id;
    if (ctx->mux_protected.role == USB_TCPM_PWR_SOURCE) {
        event_id = USB_TCPM_EVENT_SOURCE_DETACHED;
    } else if (ctx->mux_protected.role == USB_TCPM_PWR_SINK) {
        event_id = USB_TCPM_EVENT_SINK_DETACHED;
    } else {
        ESP_LOGW(TAG, "Detached event with non-source/sink role=%d", (int)ctx->mux_protected.role);
        return;
    }

    const esp_err_t err = esp_event_post(USB_TCPM_EVENT,
                                         event_id,
                                         &evt,
                                         sizeof(evt),
                                         portMAX_DELAY);
    ESP_RETURN_VOID_ON_ERROR(err, TAG, "Failed to post typec DETACHED event: %s", esp_err_to_name(err));
}

static inline void usb_tcpm_set_vbus_source(const typec_port_t *ctx, bool enable)
{
    if (ctx->constant.cfg.src_vbus_gpio != GPIO_NUM_NC) {
        gpio_set_level(ctx->constant.cfg.src_vbus_gpio, enable); // active-high
    }
    if (ctx->constant.cfg.src_vbus_gpio_n != GPIO_NUM_NC) {
        gpio_set_level(ctx->constant.cfg.src_vbus_gpio_n, !enable); // active-low
    }
    ESP_LOGD(TAG, "VBUS=%d", enable);
}

static inline void usb_tcpm_set_attach_snapshot(typec_port_t *ctx, bool attached, bool cc2_active, uint32_t rp_current_ma)
{
    USB_TCPM_STATUS_LOCK(ctx);
    ctx->mux_protected.attached = attached;
    ctx->mux_protected.cc2_active = cc2_active;
    ctx->mux_protected.rp_current_ma = rp_current_ma;
    USB_TCPM_STATUS_UNLOCK(ctx);
}

static inline void usb_tcpm_clear_pending_attach_state(typec_port_t *ctx)
{
    ctx->single_thread.pending_attach = false;
}

static inline void usb_tcpm_arm_source_detach_grace(typec_port_t *ctx)
{
    if (USB_TCPM_SRC_DETACH_GRACE_MS <= 0) {
        ctx->single_thread.source_detach_grace_active = false;
        return;
    }

    ctx->single_thread.source_detach_grace_active = true;
    ctx->single_thread.source_detach_grace_start_tick = xTaskGetTickCount();
}

static inline bool usb_tcpm_source_detach_grace_active(typec_port_t *ctx)
{
    if (!ctx->single_thread.source_detach_grace_active) {
        return false;
    }

    const TickType_t grace_ticks = pdMS_TO_TICKS(USB_TCPM_SRC_DETACH_GRACE_MS);
    const TickType_t elapsed = xTaskGetTickCount() - ctx->single_thread.source_detach_grace_start_tick;
    if (elapsed >= grace_ticks) {
        ctx->single_thread.source_detach_grace_active = false;
        return false;
    }

    return true;
}

static inline void usb_tcpm_set_sink_pending_attach(typec_port_t *ctx,
                                                    const typec_cc_status_t *status)
{
    ctx->single_thread.pending_attach = true;

    USB_TCPM_STATUS_LOCK(ctx);
    ctx->mux_protected.cc2_active = status->cc2_active;
    ctx->mux_protected.rp_current_ma = status->rp_current_ma;
    USB_TCPM_STATUS_UNLOCK(ctx);
}

static inline void usb_tcpm_sink_attach_or_wait_vbus(typec_port_t *ctx,
                                                     const typec_cc_status_t *status)
{
    if (status->vbus_ok) {
        bool cc2_active = status->cc2_active;
        uint32_t rp_current_ma = status->rp_current_ma;
        if (rp_current_ma == 0) {
            typec_cc_status_t sink_status = {0};
            if (ctx->constant.backend->get_status(ctx->constant.backend_ctx, &sink_status) == ESP_OK &&
                    sink_status.vbus_ok) {
                rp_current_ma = sink_status.rp_current_ma;
                if (sink_status.tog_result == TYPEC_TOG_RESULT_SNK) {
                    cc2_active = sink_status.cc2_active;
                }
            }
        }
        usb_tcpm_set_attach_snapshot(ctx, true, cc2_active, rp_current_ma);
        usb_tcpm_clear_pending_attach_state(ctx);
        usb_tcpm_emit_attached(ctx);
    } else {
        usb_tcpm_set_sink_pending_attach(ctx, status);
    }
}

static inline void usb_tcpm_rearm_after_detach(typec_port_t *ctx, usb_tcpm_power_role_t rearm_role)
{
    if (ctx->constant.policy_role == USB_TCPM_PWR_DRP) {
        const esp_err_t err = ctx->constant.backend->set_role(ctx->constant.backend_ctx, USB_TCPM_PWR_DRP);
        ESP_RETURN_VOID_ON_ERROR(err, TAG, "set_role(DRP) failed: %s", esp_err_to_name(err));

        USB_TCPM_STATUS_LOCK(ctx);
        ctx->mux_protected.role = USB_TCPM_PWR_DRP;
        USB_TCPM_STATUS_UNLOCK(ctx);
        return;
    }

    const esp_err_t err = ctx->constant.backend->set_role(ctx->constant.backend_ctx, rearm_role);
    ESP_RETURN_VOID_ON_ERROR(err, TAG, "set_role(%d rearm) failed: %s", (int)rearm_role, esp_err_to_name(err));
}

/* Locking note:
 * usb_tcpm_handle_events() runs in the shared worker task context and once
 * during typec_port_new() before the port is visible to the worker.
 * Lock-free reads of mux_protected fields are intentional in this function.
 * status_lock is still used for updates that must stay coherent for cross-task
 * readers (e.g. usb_tcpm_get_status()).
 */
static void usb_tcpm_handle_events(typec_port_t *ctx, typec_evt_mask_t events)
{
    if (!(events & (TYPEC_EVT_TOG | TYPEC_EVT_CC | TYPEC_EVT_VBUS))) {
        return;
    }

    if (ctx->mux_protected.role == USB_TCPM_PWR_DRP) {
        if (!(events & TYPEC_EVT_TOG)) {
            return;
        }

        typec_cc_status_t status = {0};
        ESP_RETURN_VOID_ON_ERROR(ctx->constant.backend->get_status(ctx->constant.backend_ctx, &status),
                                 TAG, "get_status (DRP) failed");

        usb_tcpm_power_role_t next_role = USB_TCPM_PWR_DRP;
        if (status.tog_result == TYPEC_TOG_RESULT_SRC) {
            next_role = USB_TCPM_PWR_SOURCE;
        } else if (status.tog_result == TYPEC_TOG_RESULT_SNK) {
            next_role = USB_TCPM_PWR_SINK;
        } else {
            return;
        }

        if (next_role == USB_TCPM_PWR_SINK) {
            usb_tcpm_set_vbus_source(ctx, false);
        }

        ESP_RETURN_VOID_ON_ERROR(ctx->constant.backend->set_role(ctx->constant.backend_ctx, next_role),
                                 TAG, "set_role(%d) failed", (int)next_role);

        USB_TCPM_STATUS_LOCK(ctx);
        ctx->mux_protected.role = next_role;
        ctx->mux_protected.attached = false;
        ctx->mux_protected.cc2_active = false;
        ctx->mux_protected.rp_current_ma = 0;
        USB_TCPM_STATUS_UNLOCK(ctx);
        usb_tcpm_clear_pending_attach_state(ctx);

        if (next_role == USB_TCPM_PWR_SOURCE) {
            if (ctx->constant.backend->commit_attach) {
                ESP_RETURN_VOID_ON_ERROR(ctx->constant.backend->commit_attach(ctx->constant.backend_ctx,
                                                                              status.cc2_active, true),
                                         TAG, "commit_attach failed");
            }
            usb_tcpm_set_attach_snapshot(ctx, true, status.cc2_active, status.rp_current_ma);
            usb_tcpm_arm_source_detach_grace(ctx);

            usb_tcpm_set_vbus_source(ctx, true);
            usb_tcpm_emit_attached(ctx);
            return;
        }

        ctx->single_thread.source_detach_grace_active = false;
        if (ctx->constant.backend->commit_attach) {
            ESP_RETURN_VOID_ON_ERROR(ctx->constant.backend->commit_attach(ctx->constant.backend_ctx,
                                                                          status.cc2_active, false),
                                     TAG, "commit_attach failed");
        }
        usb_tcpm_sink_attach_or_wait_vbus(ctx, &status);
        return;
    }

    if (ctx->mux_protected.role == USB_TCPM_PWR_SOURCE) {
        const bool attached = ctx->mux_protected.attached;
        const typec_evt_mask_t required_ev = attached ? TYPEC_EVT_CC : TYPEC_EVT_TOG;
        if (!(events & required_ev)) {
            return;
        }

        typec_cc_status_t status = {0};
        ESP_RETURN_VOID_ON_ERROR(ctx->constant.backend->get_status(ctx->constant.backend_ctx, &status),
                                 TAG, "get_status (SRC) failed");

        if (!attached) {
            if (status.tog_result != TYPEC_TOG_RESULT_SRC) {
                return;
            }

            if (ctx->constant.backend->commit_attach) {
                ESP_RETURN_VOID_ON_ERROR(ctx->constant.backend->commit_attach(ctx->constant.backend_ctx,
                                                                              status.cc2_active, true),
                                         TAG, "commit_attach failed");
            }
            usb_tcpm_set_attach_snapshot(ctx, true, status.cc2_active, status.rp_current_ma);
            usb_tcpm_clear_pending_attach_state(ctx);
            usb_tcpm_arm_source_detach_grace(ctx);

            usb_tcpm_set_vbus_source(ctx, true);
            usb_tcpm_emit_attached(ctx);
            return;
        }

        if (status.attached) {
            return;
        }

        if (usb_tcpm_source_detach_grace_active(ctx)) {
            return;
        }

        usb_tcpm_set_attach_snapshot(ctx, false, false, 0);
        usb_tcpm_clear_pending_attach_state(ctx);
        ctx->single_thread.source_detach_grace_active = false;

        usb_tcpm_set_vbus_source(ctx, false);
        usb_tcpm_emit_detached(ctx);

        usb_tcpm_rearm_after_detach(ctx, USB_TCPM_PWR_SOURCE);
        return;
    }

    if (ctx->mux_protected.role == USB_TCPM_PWR_SINK) {
        if (ctx->single_thread.pending_attach) {
            /* After commit_attach() we disable TOGGLE, so TOGSS is no longer meaningful here.
             * While pending_attach, only VBUS (and optionally CC) matters.
             */
            if (!(events & (TYPEC_EVT_VBUS | TYPEC_EVT_CC))) {
                return;
            }

            typec_cc_status_t status = {0};
            ESP_RETURN_VOID_ON_ERROR(ctx->constant.backend->get_status(ctx->constant.backend_ctx, &status),
                                     TAG, "get_status (SNK pending) failed");

            /* Optional cancel: if CC clearly disappeared while waiting for VBUS. */
            if ((events & TYPEC_EVT_CC) && (status.rp_current_ma == 0)) {
                usb_tcpm_clear_pending_attach_state(ctx);
                usb_tcpm_rearm_after_detach(ctx, USB_TCPM_PWR_SINK);
                return;
            }

            if (!status.vbus_ok) {
                return;
            }

            /* VBUS is present => now we are really attached as Sink. */
            usb_tcpm_set_attach_snapshot(ctx, true, ctx->mux_protected.cc2_active, status.rp_current_ma);
            usb_tcpm_clear_pending_attach_state(ctx);
            usb_tcpm_emit_attached(ctx);
            return;
        }

        const bool attached = ctx->mux_protected.attached;
        /* While attached, process VBUS for detach; CC can be used to track rp_current_ma updates. */
        const typec_evt_mask_t required_ev = attached ? (TYPEC_EVT_VBUS | TYPEC_EVT_CC) : TYPEC_EVT_TOG;
        if (!(events & required_ev)) {
            return;
        }

        typec_cc_status_t status = {0};
        ESP_RETURN_VOID_ON_ERROR(ctx->constant.backend->get_status(ctx->constant.backend_ctx, &status),
                                 TAG, "get_status (SNK) failed");

        if (!attached) {
            if (status.tog_result != TYPEC_TOG_RESULT_SNK) {
                return;
            }

            /* Commit orientation now (disable toggle + route measurement to active CC). */
            if (ctx->constant.backend->commit_attach) {
                ESP_RETURN_VOID_ON_ERROR(ctx->constant.backend->commit_attach(ctx->constant.backend_ctx,
                                                                              status.cc2_active, false),
                                         TAG, "commit_attach (SNK) failed");
            }
            usb_tcpm_sink_attach_or_wait_vbus(ctx, &status);
            return;
        }

        /* Optional: keep updating allowed current while attached. */
        if (events & TYPEC_EVT_CC) {
            USB_TCPM_STATUS_LOCK(ctx);
            ctx->mux_protected.rp_current_ma = status.rp_current_ma;
            USB_TCPM_STATUS_UNLOCK(ctx);
        }

        if (status.vbus_ok) {
            return;
        }

        usb_tcpm_set_attach_snapshot(ctx, false, false, 0);
        usb_tcpm_clear_pending_attach_state(ctx);
        usb_tcpm_emit_detached(ctx);

        usb_tcpm_rearm_after_detach(ctx, USB_TCPM_PWR_SINK);
    }
}

/* ---------- ISR & Task ---------- */
static void IRAM_ATTR usb_tcpm_gpio_isr(void *arg)
{
    typec_port_t *ctx = (typec_port_t *)arg;
    // Gate further edges until the task drains the device
    gpio_intr_disable(ctx->constant.gpio_int);

    BaseType_t higher_priority_task_woken = pdFALSE;
    TaskHandle_t task = NULL;

    portENTER_CRITICAL_ISR(&s_lock);
    if (!ctx->dynamic.deleting) {
        ctx->dynamic.irq_pending = true;
        task = s_task;
    }
    portEXIT_CRITICAL_ISR(&s_lock);

    if (task) {
        vTaskNotifyGiveFromISR(task, &higher_priority_task_woken);
    }

    if (higher_priority_task_woken) {
        portYIELD_FROM_ISR();
    }
}

static void usb_tcpm_task(void *arg)
{
    (void)arg;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        for (;;) {
            typec_port_t *ctx = NULL;

            portENTER_CRITICAL(&s_lock);
            for (typec_port_t *it = s_ports; it; it = it->dynamic.next) {
                if (it->dynamic.irq_pending && !it->dynamic.irq_processing && !it->dynamic.deleting) {
                    it->dynamic.irq_pending = false;
                    it->dynamic.irq_processing = true;
                    ctx = it;
                    break;
                }
            }
            portEXIT_CRITICAL(&s_lock);

            if (!ctx) {
                break;
            }

            typec_evt_mask_t events = 0;

            const esp_err_t irq_err = ctx->constant.backend->service_irq(ctx->constant.backend_ctx, &events);
            if (irq_err != ESP_OK) {
                ESP_LOGE(TAG, "service_irq failed: %s", esp_err_to_name(irq_err));
            } else {
                ESP_LOGD(TAG, "usb_tcpm_task: policy=%d role=%d events=0x%02x attached=%d",
                         ctx->constant.policy_role, ctx->mux_protected.role, events, ctx->mux_protected.attached);
                usb_tcpm_handle_events(ctx, events);

                /* Handle rare stuck-low INT: drain and process any new events */
                if (gpio_get_level(ctx->constant.gpio_int) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    typec_evt_mask_t extra_events = 0;
                    if (ctx->constant.backend->service_irq(ctx->constant.backend_ctx, &extra_events) == ESP_OK && extra_events) {
                        ESP_LOGD(TAG, "usb_tcpm_task: extra events=0x%02x attached=%d",
                                 extra_events, ctx->mux_protected.attached);
                        usb_tcpm_handle_events(ctx, extra_events);
                    }
                }
            }

            bool deleting = false;
            gpio_num_t gpio_int = GPIO_NUM_NC;
            portENTER_CRITICAL(&s_lock);
            deleting = ctx->dynamic.deleting;
            gpio_int = ctx->constant.gpio_int;
            ctx->dynamic.irq_processing = false;
            portEXIT_CRITICAL(&s_lock);

            if (!deleting) {
                gpio_intr_enable(gpio_int);
            }
        }
    }
}

/* ---------- Public API ---------- */

esp_err_t usb_tcpm_install(const usb_tcpm_install_config_t *config)
{
    // Make sure the default event loop exists.
    const esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(err, TAG, "Failed to create default event loop: %s", esp_err_to_name(err));
    }

    portENTER_CRITICAL(&s_lock);
    const bool already_installed = (s_task != NULL);
    portEXIT_CRITICAL(&s_lock);
    if (already_installed) {
        return ESP_OK;
    }

    uint32_t stack = USB_TCPM_TASK_STACK_DEFAULT;
    UBaseType_t prio = USB_TCPM_TASK_PRIO_DEFAULT;
    if (config) {
        if (config->task_stack) {
            stack = config->task_stack;
        }
        if (config->task_prio) {
            prio = (UBaseType_t)config->task_prio;
        }
    }

    TaskHandle_t task = NULL;
    if (xTaskCreate(usb_tcpm_task, "typec_port", stack, NULL, prio, &task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_lock);
    if (!s_task) {
        s_task = task;
        task = NULL;
    }
    portEXIT_CRITICAL(&s_lock);
    if (task) {
        vTaskDelete(task);
    }

    return ESP_OK;
}

esp_err_t usb_tcpm_uninstall(void)
{
    portENTER_CRITICAL(&s_lock);
    if (s_ports != NULL) {
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGE(TAG, "Cannot uninstall while ports are active");
        return ESP_ERR_INVALID_STATE;
    }
    const TaskHandle_t task = s_task;
    s_task = NULL;
    portEXIT_CRITICAL(&s_lock);

    if (task) {
        vTaskDelete(task);
    }
    return ESP_OK;
}

esp_err_t typec_port_new(const typec_port_backend_t *backend,
                         void *backend_ctx,
                         const usb_tcpm_port_config_t *port_cfg,
                         typec_port_handle_t *out)
{
    esp_err_t ret = ESP_OK;
    typec_port_t *ctx = NULL;
    ESP_RETURN_ON_FALSE(backend && backend_ctx && port_cfg && out,
                        ESP_ERR_INVALID_ARG, TAG, "bad args");
    portENTER_CRITICAL(&s_lock);
    const bool installed = (s_task != NULL);
    portEXIT_CRITICAL(&s_lock);
    if (!installed) {
        ret = ESP_ERR_INVALID_STATE;
        goto fail;
    }

    usb_tcpm_rp_current_t rp = port_cfg->rp_current;
    if (rp > USB_TCPM_RP_3A0) {
        ESP_LOGW(TAG, "Invalid rp_current=%d; using default", (int)rp);
        rp = USB_TCPM_RP_DEFAULT;
    }

    ctx = (typec_port_t *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    ctx->constant.backend = backend;
    ctx->constant.backend_ctx = backend_ctx;

    ctx->locks.status_lock = xSemaphoreCreateMutexStatic(&ctx->locks.status_lock_storage);
    if (!ctx->locks.status_lock) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }

    ctx->constant.cfg = *port_cfg;
    ctx->constant.cfg.rp_current = rp;
    ctx->constant.policy_role = ctx->constant.cfg.default_power_role;
    ctx->mux_protected.role = ctx->constant.cfg.default_power_role;

    // Configure VBUS MOSFET GPIOs if present
    if (ctx->constant.cfg.src_vbus_gpio != GPIO_NUM_NC) {
        const gpio_config_t io = {
            .pin_bit_mask = 1ULL << ctx->constant.cfg.src_vbus_gpio,
                                 .mode         = GPIO_MODE_OUTPUT,
                                 .pull_up_en   = GPIO_PULLUP_DISABLE,
                                 .pull_down_en = GPIO_PULLDOWN_DISABLE,
                                 .intr_type    = GPIO_INTR_DISABLE,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io), fail, TAG, "src_vbus gpio_config failed");

        // Ensure VBUS is OFF at startup (active-high)
        gpio_set_level(ctx->constant.cfg.src_vbus_gpio, 0);
    }
    if (ctx->constant.cfg.src_vbus_gpio_n != GPIO_NUM_NC) {
        const gpio_config_t io2 = {
            .pin_bit_mask = 1ULL << ctx->constant.cfg.src_vbus_gpio_n,
                                 .mode         = GPIO_MODE_OUTPUT,
                                 .pull_up_en   = GPIO_PULLUP_DISABLE,
                                 .pull_down_en = GPIO_PULLDOWN_DISABLE,
                                 .intr_type    = GPIO_INTR_DISABLE,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io2), fail, TAG, "src_vbus gpio_n config failed");

        // Ensure VBUS is OFF at startup (active-low)
        gpio_set_level(ctx->constant.cfg.src_vbus_gpio_n, 1);
    }

    if ((ctx->constant.cfg.default_power_role == USB_TCPM_PWR_SOURCE ||
            ctx->constant.cfg.default_power_role == USB_TCPM_PWR_DRP) &&
            ctx->constant.cfg.src_vbus_gpio == GPIO_NUM_NC &&
            ctx->constant.cfg.src_vbus_gpio_n == GPIO_NUM_NC) {
        ESP_LOGW(TAG, "Source/DRP role requested but no VBUS GPIOs are set; VBUS will not be driven");
    }

    if (ctx->constant.backend->init) {
        ESP_GOTO_ON_ERROR(ctx->constant.backend->init(ctx->constant.backend_ctx, &ctx->constant.cfg),
                          fail, TAG, "backend init failed");
    }

    /* Apply initial role from config for all default roles. */
    ESP_GOTO_ON_ERROR(ctx->constant.backend->set_role(ctx->constant.backend_ctx, ctx->constant.cfg.default_power_role),
                      fail, TAG, "set role failed");

    if (!ctx->constant.backend->get_irq_gpio) {
        ret = ESP_ERR_NOT_SUPPORTED;
        goto fail;
    }
    ESP_GOTO_ON_ERROR(ctx->constant.backend->get_irq_gpio(ctx->constant.backend_ctx, &ctx->constant.gpio_int),
                      fail, TAG, "get irq gpio failed");

    // Configure INT GPIO as input with pull-up; enable interrupt later
    const gpio_config_t gc = {
        .pin_bit_mask = 1ULL << ctx->constant.gpio_int,
                             .mode = GPIO_MODE_INPUT,
                             .pull_up_en = GPIO_PULLUP_ENABLE,
                             .pull_down_en = GPIO_PULLDOWN_DISABLE,
                             .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_GOTO_ON_ERROR(gpio_config(&gc), fail, TAG, "gpio_config");

    const esp_err_t isr_ret = gpio_install_isr_service(0);
    if (isr_ret != ESP_ERR_INVALID_STATE) {
        ESP_GOTO_ON_ERROR(isr_ret, fail, TAG, "gpio_install_isr_service failed");
    }
    ESP_GOTO_ON_ERROR(gpio_isr_handler_add(ctx->constant.gpio_int, usb_tcpm_gpio_isr, ctx), fail, TAG, "gpio_isr_add");

    /* Drain and process any pre-existing latched causes before arming GPIO */
    typec_evt_mask_t events = 0;

    ESP_GOTO_ON_ERROR(ctx->constant.backend->service_irq(ctx->constant.backend_ctx, &events), fail_isr, TAG, "prime irq");
    if (events) {
        usb_tcpm_handle_events(ctx, events);
    }

    /* Now arm the GPIO */
    ESP_GOTO_ON_ERROR(gpio_set_intr_type(ctx->constant.gpio_int, GPIO_INTR_LOW_LEVEL),
                      fail_isr, TAG, "gpio_set_intr_type");
    usb_tcpm_port_list_add(ctx);
    gpio_intr_enable(ctx->constant.gpio_int);

    *out = (typec_port_handle_t)ctx;
    ESP_LOGI(TAG, "Type-C port started");
    return ESP_OK;
fail_isr:
    gpio_intr_disable(ctx->constant.gpio_int);
    gpio_isr_handler_remove(ctx->constant.gpio_int);
fail:
    ESP_LOGE(TAG, "Init failed, err=%d", ret);
    if (ctx) {
        free(ctx);
    }
    return ret;
}

esp_err_t usb_tcpm_port_destroy(usb_tcpm_port_handle_t port_hdl)
{
    typec_port_t *ctx = (typec_port_t *)port_hdl;
    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_lock);
    ctx->dynamic.deleting = true;
    ctx->dynamic.irq_pending = false;
    portEXIT_CRITICAL(&s_lock);

    gpio_intr_disable(ctx->constant.gpio_int);
    gpio_isr_handler_remove(ctx->constant.gpio_int);

    usb_tcpm_port_list_remove(ctx);

    for (;;) {
        bool busy = false;
        portENTER_CRITICAL(&s_lock);
        busy = ctx->dynamic.irq_processing;
        portEXIT_CRITICAL(&s_lock);
        if (!busy) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    /* Fail-safe: always stop driving local source VBUS on port teardown. */
    usb_tcpm_set_vbus_source(ctx, false);

    if (ctx->constant.backend_ctx && ctx->constant.backend && ctx->constant.backend->deinit) {
        ctx->constant.backend->deinit(ctx->constant.backend_ctx);
    }

    free(ctx);
    return ESP_OK;
}

esp_err_t usb_tcpm_get_status(usb_tcpm_port_handle_t port_hdl, usb_tcpm_port_status_t *status)
{
    typec_port_t *ctx = (typec_port_t *)port_hdl;
    if (!ctx || !status) {
        return ESP_ERR_INVALID_ARG;
    }
    USB_TCPM_STATUS_LOCK(ctx);
    *status = (usb_tcpm_port_status_t) {
        .attached = ctx->mux_protected.attached,
        .cc2_active = ctx->mux_protected.cc2_active,
        .rp_current_ma = ctx->mux_protected.rp_current_ma,
        .role = ctx->mux_protected.role,
    };
    USB_TCPM_STATUS_UNLOCK(ctx);
    return ESP_OK;
}
