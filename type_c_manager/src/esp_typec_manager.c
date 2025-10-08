/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_typec_manager.h"
#include "esp_event.h"
#include "esp_check.h"
#include "esp_log.h"
#include "tusb.h"           // for tud_connect()/tud_disconnect() when in SINK_DEVICE mode
// (and later usb_host.h for SOURCE_HOST)

typedef struct esp_typec_manager_ctx {
    esp_typec_manager_config_t cfg;
    esp_typec_pd_port_handle_t  pd_port;
    bool     attached;
    bool     cc2_active;
    uint32_t rp_ma;
    esp_event_handler_instance_t evt_inst;  // so we can unregister later
} esp_typec_manager_ctx_t;

static const char *TAG = "esp_typec_manager";

static void pd_evt_handler(void *arg, esp_event_base_t base,
                           int32_t id, void *data)
{
    esp_typec_manager_ctx_t *manager = arg;
    (void)base;

    switch (id) {
    case ESP_TYPEC_PD_EVENT_ID_ATTACHED: {
        const esp_typec_pd_evt_attached_t *a = data;
        // Ignore events from other ports (future multi-port support)
        if (manager->pd_port && manager->pd_port != a->port) {
            return;
        }
        manager->attached   = true;
        manager->cc2_active = a->cc2_active;
        manager->rp_ma      = a->rp_cur_ma;

        if (manager->cfg.mode == ESP_TYPEC_MANAGER_MODE_SINK_DEVICE &&
                a->rp_cur_ma >= manager->cfg.min_rp_cur_ma) {
            tud_connect();
            ESP_LOGI(TAG, "Connecting USB device (TinyUSB)");
        } else {
            ESP_LOGI(TAG, "Attach ignored: rp_cur_ma=%"PRIu32" < min_rp_cur_ma=%"PRIu32,
                     a->rp_cur_ma, manager->cfg.min_rp_cur_ma);
        }
        break;
    }
    case ESP_TYPEC_PD_EVENT_ID_DETACHED:
        manager->attached = false;

        if (manager->cfg.mode == ESP_TYPEC_MANAGER_MODE_SINK_DEVICE) {
            tud_disconnect();
            ESP_LOGI(TAG, "Disconnecting USB device (TinyUSB)");
        }
        break;

    default:
        break;
    }
}

esp_err_t esp_typec_manager_install(const esp_typec_manager_config_t *cfg,
                                    esp_typec_manager_handle_t *out)
{
    ESP_RETURN_ON_FALSE(cfg && out, ESP_ERR_INVALID_ARG, TAG, "bad args");

    esp_typec_manager_ctx_t *manager = calloc(1, sizeof(*manager));
    ESP_RETURN_ON_FALSE(manager, ESP_ERR_NO_MEM, TAG, "no mem");

    manager->cfg     = *cfg;
    manager->pd_port = cfg->pd_port;

    // Start with device logically disconnected
    if (cfg->mode == ESP_TYPEC_MANAGER_MODE_SINK_DEVICE) {
        tud_disconnect();
    }

    // Register for PD events
    esp_err_t err = esp_event_handler_instance_register(
                        ESP_TYPEC_PD_EVENT,
                        ESP_EVENT_ANY_ID,
                        &pd_evt_handler,
                        manager,
                        &manager->evt_inst);
    if (err != ESP_OK) {
        free(manager);
        return err;
    }

    *out = (esp_typec_manager_handle_t)manager;
    return ESP_OK;
}

esp_err_t esp_typec_manager_uninstall(esp_typec_manager_handle_t handle)
{
    esp_typec_manager_ctx_t *manager = (esp_typec_manager_ctx_t *)handle;
    if (!manager) {
        return ESP_ERR_INVALID_ARG;
    }

    if (manager->evt_inst) {
        esp_event_handler_instance_unregister(
            ESP_TYPEC_PD_EVENT,
            ESP_EVENT_ANY_ID,
            manager->evt_inst);
    }

    free(manager);
    return ESP_OK;
}

esp_err_t esp_typec_manager_is_bus_present(esp_typec_manager_handle_t handle, bool *present)
{
    esp_typec_manager_ctx_t *manager = (esp_typec_manager_ctx_t *)handle;
    if (!manager || !present) {
        return ESP_ERR_INVALID_ARG;
    }

    // simple policy: attached + Rp >= min_rp_cur_ma
    *present = manager->attached &&
               (manager->rp_ma >= manager->cfg.min_rp_cur_ma);
    return ESP_OK;
}
