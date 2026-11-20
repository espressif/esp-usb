/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_private/critical_section.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "usb/cdc_acm_host.h"
#include "cdc_host_common.h"
#include "cdc_host_acm_compliant.h"

static const char *TAG = "cdc_acm";

// Payload capacity; the common layer allocates the setup packet separately.
#define CDC_ACM_CTRL_DATA_SIZE (64)

// CDC-ACM spinlock
DEFINE_CRIT_SECTION_LOCK_STATIC(cdc_acm_lock);
#define CDC_ACM_ENTER_CRITICAL() esp_os_enter_critical(&cdc_acm_lock)
#define CDC_ACM_EXIT_CRITICAL()  esp_os_exit_critical(&cdc_acm_lock)

typedef struct {
    cdc_host_common_driver_handle_t common_driver;
    cdc_host_common_dev_event_cb_handle_t dev_event_cb;
    SemaphoreHandle_t open_close_mutex;
    cdc_acm_new_dev_callback_t new_dev_cb;
    unsigned new_dev_callbacks;
    SLIST_HEAD(list_dev, cdc_dev_s) cdc_devices_list;
} cdc_acm_obj_t;

static cdc_acm_obj_t *p_cdc_acm_obj = NULL;

static const cdc_acm_host_driver_config_t cdc_acm_driver_config_default = {
    .driver_task_stack_size = 4096,
    .driver_task_priority = 10,
    .xCoreID = 0,
    .new_dev_cb = NULL,
};

static bool cdc_acm_device_is_open(cdc_dev_t *cdc_hdl);

static bool acm_callback_enter(cdc_dev_t *cdc_dev, cdc_host_common_port_handle_t common_port)
{
    CDC_ACM_ENTER_CRITICAL();
    // Async rollback can outlive the ACM handle; validate identity before dereferencing it.
    const bool enabled = p_cdc_acm_obj && cdc_acm_device_is_open(cdc_dev) && cdc_dev->common_port == common_port && !cdc_dev->closing;
    if (enabled) {
        cdc_dev->references++;
    }
    CDC_ACM_EXIT_CRITICAL();
    return enabled;
}

static void acm_device_release(cdc_dev_t *cdc_dev)
{
    CDC_ACM_ENTER_CRITICAL();
    const bool destroy = --cdc_dev->references == 0;
    CDC_ACM_EXIT_CRITICAL();
    if (destroy) {
        // A callback may close its own device; keep the VCP context alive until it returns.
        if (cdc_dev->intf_func.del) {
            cdc_dev->intf_func.del(cdc_dev);
        }
        free(cdc_dev);
    }
}

cdc_host_common_port_handle_t cdc_acm_common_port_get(cdc_dev_t *cdc_dev)
{
    CDC_ACM_ENTER_CRITICAL();
    cdc_host_common_port_handle_t port = (!cdc_dev->closing && !cdc_dev->disconnected) ? cdc_dev->common_port : NULL;
    CDC_ACM_EXIT_CRITICAL();
    return port;
}

static bool acm_data_cb(cdc_host_common_port_handle_t common_port, const uint8_t *data, size_t data_len, void *user_arg)
{
    cdc_dev_t *cdc_dev = (cdc_dev_t *)user_arg;
    if (!acm_callback_enter(cdc_dev, common_port)) {
        return true;
    }
    const bool consumed = cdc_dev->in_cb ? cdc_dev->in_cb(data, data_len, cdc_dev->cb_arg) : true;
    acm_device_release(cdc_dev);
    return consumed;
}

static void acm_event_cb(cdc_host_common_port_handle_t common_port, const cdc_host_common_port_event_data_t *event, void *user_arg)
{
    cdc_dev_t *cdc_dev = (cdc_dev_t *)user_arg;
    if (!acm_callback_enter(cdc_dev, common_port)) {
        return;
    }
    if (event->type == CDC_HOST_COMMON_PORT_EVENT_DISCONNECTED) {
        CDC_ACM_ENTER_CRITICAL();
        cdc_dev->disconnected = true;
        CDC_ACM_EXIT_CRITICAL();
    }
    if (!cdc_dev->event_cb) {
        acm_device_release(cdc_dev);
        return;
    }

    switch (event->type) {
    case CDC_HOST_COMMON_PORT_EVENT_ERROR: {
        const cdc_acm_host_dev_event_data_t acm_event = {
            .type = CDC_ACM_HOST_ERROR,
            .data.error = event->data.error,
        };
        cdc_dev->event_cb(&acm_event, cdc_dev->cb_arg);
        break;
    }
    case CDC_HOST_COMMON_PORT_EVENT_NOTIFICATION: {
        if (event->data.notification.data_len < sizeof(cdc_notification_t)) {
            ESP_LOGW(TAG, "Short CDC notification: %zu", event->data.notification.data_len);
            break;
        }
        const cdc_notification_t *notif = (const cdc_notification_t *)event->data.notification.data;
        switch (notif->bNotificationCode) {
        case USB_CDC_NOTIF_NETWORK_CONNECTION: {
            const cdc_acm_host_dev_event_data_t net_conn_event = {
                .type = CDC_ACM_HOST_NETWORK_CONNECTION,
                .data.network_connected = (bool)notif->wValue,
            };
            cdc_dev->event_cb(&net_conn_event, cdc_dev->cb_arg);
            break;
        }
        case USB_CDC_NOTIF_SERIAL_STATE: {
            // SERIAL_STATE carries a 2-byte bitmap after the notification header.
            if (notif->wLength < sizeof(uint16_t) || event->data.notification.data_len < sizeof(cdc_notification_t) + notif->wLength) {
                ESP_LOGW(TAG, "Invalid SERIAL_STATE length: %u, received: %zu", notif->wLength, event->data.notification.data_len);
                break;
            }
            memcpy(&cdc_dev->serial_state.val, notif->Data, sizeof(cdc_dev->serial_state.val));
            const cdc_acm_host_dev_event_data_t serial_state_event = {
                .type = CDC_ACM_HOST_SERIAL_STATE,
                .data.serial_state = cdc_dev->serial_state,
            };
            cdc_dev->event_cb(&serial_state_event, cdc_dev->cb_arg);
            break;
        }
        case USB_CDC_NOTIF_RESPONSE_AVAILABLE:
        default:
            ESP_LOGW(TAG, "Unsupported notification type 0x%02X", notif->bNotificationCode);
            ESP_LOG_BUFFER_HEX(TAG, event->data.notification.data, event->data.notification.data_len);
            break;
        }
        break;
    }
    case CDC_HOST_COMMON_PORT_EVENT_RX_OVERFLOW: {
        cdc_acm_host_dev_event_data_t overrun_event = {
            .type = CDC_ACM_HOST_SERIAL_STATE,
            .data.serial_state = cdc_dev->serial_state,
        };
        // Local RX loss is a transient event, not a change to the device's UART state.
        overrun_event.data.serial_state.bOverRun = 1;
        cdc_dev->event_cb(&overrun_event, cdc_dev->cb_arg);
        break;
    }
    case CDC_HOST_COMMON_PORT_EVENT_DISCONNECTED: {
        const cdc_acm_host_dev_event_data_t disconn_event = {
            .type = CDC_ACM_HOST_DEVICE_DISCONNECTED,
            .data.cdc_hdl = (cdc_acm_dev_hdl_t)cdc_dev,
        };
        cdc_dev->event_cb(&disconn_event, cdc_dev->cb_arg);
        break;
    }
#ifdef CDC_HOST_SUSPEND_RESUME_API_SUPPORTED
    case CDC_HOST_COMMON_PORT_EVENT_SUSPENDED: {
        const cdc_acm_host_dev_event_data_t suspend_event = {
            .type = CDC_ACM_HOST_DEVICE_SUSPENDED,
            .data.cdc_hdl = (cdc_acm_dev_hdl_t)cdc_dev,
        };
        cdc_dev->event_cb(&suspend_event, cdc_dev->cb_arg);
        break;
    }
    case CDC_HOST_COMMON_PORT_EVENT_RESUMED: {
        const cdc_acm_host_dev_event_data_t resume_event = {
            .type = CDC_ACM_HOST_DEVICE_RESUMED,
            .data.cdc_hdl = (cdc_acm_dev_hdl_t)cdc_dev,
        };
        cdc_dev->event_cb(&resume_event, cdc_dev->cb_arg);
        break;
    }
#endif
    default:
        ESP_LOGE(TAG, "Unrecognized common CDC event");
        break;
    }
    acm_device_release(cdc_dev);
}

static void acm_dev_event_cb(const cdc_host_common_dev_event_data_t *event, void *user_arg)
{
    if (event->type != CDC_HOST_COMMON_DEV_EVENT_NEW) {
        return;
    }

    CDC_ACM_ENTER_CRITICAL();
    cdc_acm_obj_t *cdc_acm_obj = p_cdc_acm_obj;
    cdc_acm_new_dev_callback_t new_dev_cb = cdc_acm_obj ? cdc_acm_obj->new_dev_cb : NULL;
    if (new_dev_cb) {
        cdc_acm_obj->new_dev_callbacks++;
    }
    CDC_ACM_EXIT_CRITICAL();

    if (new_dev_cb) {
        new_dev_cb(event->data.new_dev.dev_hdl);
        CDC_ACM_ENTER_CRITICAL();
        cdc_acm_obj->new_dev_callbacks--;
        CDC_ACM_EXIT_CRITICAL();
    }
}

static bool cdc_acm_device_is_open(cdc_dev_t *cdc_hdl)
{
    cdc_dev_t *current = NULL;
    SLIST_FOREACH(current, &p_cdc_acm_obj->cdc_devices_list, list_entry) {
        if (current == cdc_hdl) {
            return true;
        }
    }
    return false;
}

esp_err_t cdc_acm_host_install(const cdc_acm_host_driver_config_t *driver_config)
{
    CDC_ACM_CHECK(!p_cdc_acm_obj, ESP_ERR_INVALID_STATE);

    if (!driver_config) {
        driver_config = &cdc_acm_driver_config_default;
    }

    esp_err_t ret;
    cdc_acm_obj_t *cdc_acm_obj = calloc(1, sizeof(cdc_acm_obj_t));
    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    ESP_GOTO_ON_FALSE(cdc_acm_obj && mutex, ESP_ERR_NO_MEM, err, TAG, "Failed to allocate CDC ACM object");

    const cdc_host_common_driver_config_t common_config = {
        .task_stack_size = driver_config->driver_task_stack_size,
        .task_priority = driver_config->driver_task_priority,
        .task_coreid = driver_config->xCoreID,
    };
    ESP_GOTO_ON_ERROR(cdc_host_common_acquire(&common_config, &cdc_acm_obj->common_driver), err, TAG, "Failed to acquire CDC common driver");
    ESP_GOTO_ON_ERROR(cdc_host_common_register_dev_event_cb(cdc_acm_obj->common_driver, acm_dev_event_cb, NULL,
                                                            &cdc_acm_obj->dev_event_cb), common_err, TAG, "Failed to register CDC ACM device callback");

    cdc_acm_obj->open_close_mutex = mutex;
    cdc_acm_obj->new_dev_cb = driver_config->new_dev_cb;
    SLIST_INIT(&cdc_acm_obj->cdc_devices_list);

    CDC_ACM_ENTER_CRITICAL();
    if (p_cdc_acm_obj) {
        ret = ESP_ERR_INVALID_STATE;
        CDC_ACM_EXIT_CRITICAL();
        goto cb_err;
    }
    p_cdc_acm_obj = cdc_acm_obj;
    CDC_ACM_EXIT_CRITICAL();
    return ESP_OK;

cb_err:
    cdc_host_common_unregister_dev_event_cb(cdc_acm_obj->dev_event_cb);
common_err:
    cdc_host_common_release(cdc_acm_obj->common_driver);
err:
    if (mutex) {
        vSemaphoreDelete(mutex);
    }
    free(cdc_acm_obj);
    return ret;
}

esp_err_t cdc_acm_host_uninstall(void)
{
    esp_err_t ret = ESP_OK;

    CDC_ACM_ENTER_CRITICAL();
    CDC_ACM_CHECK_FROM_CRIT(p_cdc_acm_obj, ESP_ERR_INVALID_STATE);
    cdc_acm_obj_t *cdc_acm_obj = p_cdc_acm_obj;
    CDC_ACM_EXIT_CRITICAL();

    xSemaphoreTake(cdc_acm_obj->open_close_mutex, portMAX_DELAY);
    CDC_ACM_ENTER_CRITICAL();
    if (!SLIST_EMPTY(&cdc_acm_obj->cdc_devices_list) || cdc_acm_obj->new_dev_callbacks) {
        ret = ESP_ERR_INVALID_STATE;
        CDC_ACM_EXIT_CRITICAL();
        goto unblock;
    }
    p_cdc_acm_obj = NULL;
    CDC_ACM_EXIT_CRITICAL();

    cdc_host_common_unregister_dev_event_cb(cdc_acm_obj->dev_event_cb);
    cdc_acm_obj->dev_event_cb = NULL;
    ret = cdc_host_common_release(cdc_acm_obj->common_driver);
    if (ret != ESP_OK) {
        esp_err_t cb_ret = cdc_host_common_register_dev_event_cb(cdc_acm_obj->common_driver, acm_dev_event_cb, NULL, &cdc_acm_obj->dev_event_cb);
        if (cb_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to restore device callback: %s", esp_err_to_name(cb_ret));
        }
        CDC_ACM_ENTER_CRITICAL();
        p_cdc_acm_obj = cdc_acm_obj;
        CDC_ACM_EXIT_CRITICAL();
        goto unblock;
    }
    xSemaphoreGive(cdc_acm_obj->open_close_mutex);
    vSemaphoreDelete(cdc_acm_obj->open_close_mutex);
    free(cdc_acm_obj);
    return ret;

unblock:
    xSemaphoreGive(cdc_acm_obj->open_close_mutex);
    return ret;
}

esp_err_t cdc_acm_host_register_new_dev_callback(cdc_acm_new_dev_callback_t new_dev_cb)
{
    CDC_ACM_CHECK(p_cdc_acm_obj, ESP_ERR_INVALID_STATE);
    CDC_ACM_ENTER_CRITICAL();
    p_cdc_acm_obj->new_dev_cb = new_dev_cb;
    CDC_ACM_EXIT_CRITICAL();
    return ESP_OK;
}

esp_err_t cdc_acm_host_open_v1_dispatch(uint16_t vid, uint16_t pid, uint8_t interface_idx,
                                        const cdc_acm_host_device_config_t *dev_config, cdc_acm_dev_hdl_t *cdc_hdl_ret)
{
    CDC_ACM_CHECK(dev_config, ESP_ERR_INVALID_ARG);
    const cdc_acm_host_open_config_t cfg = {
        .vid = vid,
        .pid = pid,
        .interface_idx = interface_idx,
        .dev_addr = CDC_HOST_ANY_DEV_ADDR,
        .connection_timeout_ms = dev_config->connection_timeout_ms,
        .out_buffer_size = dev_config->out_buffer_size,
        .in_buffer_size = dev_config->in_buffer_size,
        .event_cb = dev_config->event_cb,
        .data_cb = dev_config->data_cb,
        .user_arg = dev_config->user_arg,
    };
    return cdc_acm_host_open_v2(&cfg, cdc_hdl_ret);
}

esp_err_t cdc_acm_host_open_v2(const cdc_acm_host_open_config_t *open_config, cdc_acm_dev_hdl_t *cdc_hdl_ret)
{
    CDC_ACM_CHECK(p_cdc_acm_obj, ESP_ERR_INVALID_STATE);
    CDC_ACM_CHECK(open_config && cdc_hdl_ret, ESP_ERR_INVALID_ARG);
    esp_err_t ret;

    cdc_dev_t *cdc_dev = calloc(1, sizeof(cdc_dev_t));
    ESP_GOTO_ON_FALSE(cdc_dev, ESP_ERR_NO_MEM, err, TAG, "Failed to allocate CDC ACM device");
    cdc_dev->references = 1; // Owned by the caller until explicit close.
    cdc_dev->in_cb = open_config->data_cb;
    cdc_dev->event_cb = open_config->event_cb;
    cdc_dev->cb_arg = open_config->user_arg;

    const cdc_host_common_open_config_t common_config = {
        .vid = open_config->vid,
        .pid = open_config->pid,
        .dev_addr = open_config->dev_addr,
        .interface_idx = open_config->interface_idx,
        .connection_timeout_ms = open_config->connection_timeout_ms,
        .ctrl_buffer_size = CDC_ACM_CTRL_DATA_SIZE,
        .in_buffer_size = open_config->in_buffer_size,
        .out_buffer_size = open_config->out_buffer_size,
        .flags = CDC_HOST_COMMON_OPEN_FLAG_NONE,
        .data_cb = open_config->data_cb ? acm_data_cb : NULL,
        .event_cb = acm_event_cb,
        .user_arg = cdc_dev,
    };

    xSemaphoreTake(p_cdc_acm_obj->open_close_mutex, portMAX_DELAY);
    CDC_ACM_ENTER_CRITICAL();
    cdc_dev->opening = true;
    SLIST_INSERT_HEAD(&p_cdc_acm_obj->cdc_devices_list, cdc_dev, list_entry);
    CDC_ACM_EXIT_CRITICAL();
    ESP_GOTO_ON_ERROR(cdc_host_common_open(p_cdc_acm_obj->common_driver, &common_config, &cdc_dev->common_port),
                      unlock, TAG, "Failed to open CDC common port");
    ESP_GOTO_ON_ERROR(cdc_host_common_get_intf_desc(cdc_dev->common_port, &cdc_dev->notif.intf_desc, &cdc_dev->data.intf_desc),
                      unlock, TAG, "Failed to get CDC interface descriptors");

    // Keep VCP descriptors valid through del(), after common close releases its port.
    if (cdc_dev->notif.intf_desc) {
        cdc_dev->notif.desc = *cdc_dev->notif.intf_desc;
        cdc_dev->notif.intf_desc = &cdc_dev->notif.desc;
    }
    if (cdc_dev->data.intf_desc) {
        cdc_dev->data.desc = *cdc_dev->data.intf_desc;
        cdc_dev->data.intf_desc = &cdc_dev->data.desc;
    }

    const usb_standard_desc_t *acm_desc = NULL;
    if (cdc_host_common_cdc_desc_get(cdc_dev->common_port, USB_CDC_DESC_SUBTYPE_ACM, &acm_desc) == ESP_OK) {
        cdc_dev->intf_func.line_coding_get = acm_compliant_line_coding_get;
        cdc_dev->intf_func.line_coding_set = acm_compliant_line_coding_set;
        cdc_dev->intf_func.set_control_line_state = acm_compliant_set_control_line_state;
        cdc_dev->intf_func.send_break = acm_compliant_send_break;
    }

    CDC_ACM_ENTER_CRITICAL();
    if (cdc_dev->closing || cdc_dev->disconnected) {
        CDC_ACM_EXIT_CRITICAL();
        ret = ESP_ERR_INVALID_STATE;
        goto unlock;
    }
    // Publish before callbacks are allowed to release the caller's ownership.
    *cdc_hdl_ret = (cdc_acm_dev_hdl_t)cdc_dev;
    cdc_dev->opening = false;
    CDC_ACM_EXIT_CRITICAL();
    xSemaphoreGive(p_cdc_acm_obj->open_close_mutex);
    return ESP_OK;

unlock:
    CDC_ACM_ENTER_CRITICAL();
    cdc_dev->closing = true;
    SLIST_REMOVE(&p_cdc_acm_obj->cdc_devices_list, cdc_dev, cdc_dev_s, list_entry);
    CDC_ACM_EXIT_CRITICAL();
    xSemaphoreGive(p_cdc_acm_obj->open_close_mutex);
err:
    if (cdc_dev) {
        if (cdc_dev->common_port) {
            // No handle is returned on failure; common owns the remaining cancellation work.
            cdc_host_common_close_async(cdc_dev->common_port);
        }
        acm_device_release(cdc_dev);
    }
    *cdc_hdl_ret = NULL;
    return ret;
}

esp_err_t cdc_acm_host_close(cdc_acm_dev_hdl_t cdc_hdl)
{
    CDC_ACM_CHECK(p_cdc_acm_obj, ESP_ERR_INVALID_STATE);
    CDC_ACM_CHECK(cdc_hdl, ESP_ERR_INVALID_ARG);
    cdc_dev_t *cdc_dev = (cdc_dev_t *)cdc_hdl;

    CDC_ACM_ENTER_CRITICAL();
    if (!cdc_acm_device_is_open(cdc_dev) || cdc_dev->closing) {
        CDC_ACM_EXIT_CRITICAL();
        return ESP_OK;
    }
    cdc_dev->closing = true;
    const bool opening = cdc_dev->opening;
    CDC_ACM_EXIT_CRITICAL();
    if (opening) {
        return ESP_OK; // Open unwinds the unpublished handle after the callback returns.
    }

    // Never hold the ACM mutex while common close waits for a callback to return.
    esp_err_t ret = cdc_host_common_close(cdc_dev->common_port);
    if (ret != ESP_OK) {
        CDC_ACM_ENTER_CRITICAL();
        cdc_dev->closing = false;
        CDC_ACM_EXIT_CRITICAL();
        ESP_LOGE(TAG, "Failed to close CDC common port: %s", esp_err_to_name(ret));
        return ret;
    }

    CDC_ACM_ENTER_CRITICAL();
    SLIST_REMOVE(&p_cdc_acm_obj->cdc_devices_list, cdc_dev, cdc_dev_s, list_entry);
    CDC_ACM_EXIT_CRITICAL();
    acm_device_release(cdc_dev);
    return ESP_OK;
}

esp_err_t cdc_acm_host_data_tx_blocking(cdc_acm_dev_hdl_t cdc_hdl, const uint8_t *data, size_t data_len, uint32_t timeout_ms)
{
    CDC_ACM_CHECK(cdc_hdl, ESP_ERR_INVALID_ARG);
    cdc_host_common_port_handle_t port = cdc_acm_common_port_get((cdc_dev_t *)cdc_hdl);
    CDC_ACM_CHECK(port, ESP_ERR_INVALID_STATE);
    return cdc_host_common_write_bytes(port, data, data_len, pdMS_TO_TICKS(timeout_ms));
}

esp_err_t cdc_acm_host_send_custom_request(cdc_acm_dev_hdl_t cdc_hdl, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue,
                                           uint16_t wIndex, uint16_t wLength, uint8_t *data)
{
    CDC_ACM_CHECK(cdc_hdl, ESP_ERR_INVALID_ARG);
    cdc_host_common_port_handle_t port = cdc_acm_common_port_get((cdc_dev_t *)cdc_hdl);
    CDC_ACM_CHECK(port, ESP_ERR_INVALID_STATE);
    return cdc_host_common_send_control(port, bmRequestType, bRequest, wValue, wIndex, wLength, data);
}

void cdc_acm_host_desc_print(cdc_acm_dev_hdl_t cdc_hdl)
{
    assert(cdc_hdl);
    cdc_host_common_port_handle_t port = cdc_acm_common_port_get((cdc_dev_t *)cdc_hdl);
    if (port) {
        cdc_host_common_desc_print(port);
    }
}

esp_err_t cdc_acm_host_protocols_get(cdc_acm_dev_hdl_t cdc_hdl, cdc_comm_protocol_t *comm, cdc_data_protocol_t *data)
{
    CDC_ACM_CHECK(cdc_hdl, ESP_ERR_INVALID_ARG);
    cdc_host_common_port_handle_t port = cdc_acm_common_port_get((cdc_dev_t *)cdc_hdl);
    CDC_ACM_CHECK(port, ESP_ERR_INVALID_STATE);
    return cdc_host_common_protocols_get(port, comm, data);
}

esp_err_t cdc_acm_host_cdc_desc_get(cdc_acm_dev_hdl_t cdc_hdl, cdc_desc_subtype_t desc_type, const usb_standard_desc_t **desc_out)
{
    CDC_ACM_CHECK(cdc_hdl, ESP_ERR_INVALID_ARG);
    cdc_host_common_port_handle_t port = cdc_acm_common_port_get((cdc_dev_t *)cdc_hdl);
    CDC_ACM_CHECK(port, ESP_ERR_INVALID_STATE);
    return cdc_host_common_cdc_desc_get(port, desc_type, desc_out);
}

#ifdef CDC_HOST_REMOTE_WAKE_SUPPORTED
esp_err_t cdc_acm_host_enable_remote_wakeup(cdc_acm_dev_hdl_t cdc_hdl, bool enable)
{
    CDC_ACM_CHECK(cdc_hdl, ESP_ERR_INVALID_ARG);
    cdc_host_common_port_handle_t port = cdc_acm_common_port_get((cdc_dev_t *)cdc_hdl);
    CDC_ACM_CHECK(port, ESP_ERR_INVALID_STATE);
    return cdc_host_common_enable_remote_wakeup(port, enable);
}
#endif
