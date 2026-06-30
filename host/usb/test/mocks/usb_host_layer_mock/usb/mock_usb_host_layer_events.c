/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_log.h"
#include "mock_usb_host_layer_events.h"
#include "mock_usb_host_layer_internal.h"

#define MOCK_USB_HOST_LAYER_EVENT_CHECK(cond, ret_val) ({ \
    if (!(cond)) {                                          \
        return (ret_val);                                   \
    }                                                       \
})

static const char *MOCK_USB_HOST_LAYER_EVENT_TAG = "Mock USB Host Event";

static mock_usb_host_layer_callbacks_t *callbacks(void)
{
    return mock_usb_host_layer_callbacks_get();
}

// ------------------------------------------------- USBH events -------------------------------------------------------

static void invoke_usbh_event(const usbh_event_data_t *event_data)
{
    callbacks()->usbh_event_cb((usbh_event_data_t *)event_data, NULL);
}

esp_err_t usbh_mock_event_ctrl_xfer(void *dev_obj, urb_t *urb)
{
    MOCK_USB_HOST_LAYER_EVENT_CHECK(callbacks()->usbh_event_cb != NULL, ESP_ERR_INVALID_STATE);
    ESP_LOGD(MOCK_USB_HOST_LAYER_EVENT_TAG, "Propagating USBH_EVENT_CTRL_XFER event");

    usbh_event_data_t event_data = {
        .event = USBH_EVENT_CTRL_XFER,
        .ctrl_xfer_data = {
            .dev_hdl = (usb_device_handle_t)dev_obj,
            .urb = urb,
        },
    };
    invoke_usbh_event(&event_data);
    return ESP_OK;
}

esp_err_t usbh_mock_event_new_dev(uint8_t dev_addr)
{
    MOCK_USB_HOST_LAYER_EVENT_CHECK(callbacks()->usbh_event_cb != NULL, ESP_ERR_INVALID_STATE);
    ESP_LOGD(MOCK_USB_HOST_LAYER_EVENT_TAG, "Propagating USBH_EVENT_NEW_DEV event");

    usbh_event_data_t event_data = {
        .event = USBH_EVENT_NEW_DEV,
        .new_dev_data = {
            .dev_addr = dev_addr,
        },
    };
    invoke_usbh_event(&event_data);
    return ESP_OK;
}

esp_err_t usbh_mock_event_dev_gone(uint8_t dev_addr, void *dev_obj)
{
    MOCK_USB_HOST_LAYER_EVENT_CHECK(callbacks()->usbh_event_cb != NULL, ESP_ERR_INVALID_STATE);
    ESP_LOGD(MOCK_USB_HOST_LAYER_EVENT_TAG, "Propagating USBH_EVENT_DEV_GONE event");

    usbh_event_data_t event_data = {
        .event = USBH_EVENT_DEV_GONE,
        .dev_gone_data = {
            .dev_addr = dev_addr,
            .dev_hdl = (usb_device_handle_t)dev_obj,
        },
    };
    invoke_usbh_event(&event_data);
    return ESP_OK;
}

esp_err_t usbh_mock_event_dev_free(unsigned int dev_uid, void *parent_dev_hdl, uint8_t parent_port_num)
{
    MOCK_USB_HOST_LAYER_EVENT_CHECK(callbacks()->usbh_event_cb != NULL, ESP_ERR_INVALID_STATE);
    ESP_LOGD(MOCK_USB_HOST_LAYER_EVENT_TAG, "Propagating USBH_EVENT_DEV_FREE event");

    usbh_event_data_t event_data = {
        .event = USBH_EVENT_DEV_FREE,
        .dev_free_data = {
            .dev_uid = dev_uid,
            .parent_dev_hdl = (usb_device_handle_t)parent_dev_hdl,
            .port_num = parent_port_num,
        },
    };
    invoke_usbh_event(&event_data);
    return ESP_OK;
}

esp_err_t usbh_mock_event_all_free(void)
{
    MOCK_USB_HOST_LAYER_EVENT_CHECK(callbacks()->usbh_event_cb != NULL, ESP_ERR_INVALID_STATE);
    ESP_LOGD(MOCK_USB_HOST_LAYER_EVENT_TAG, "Propagating USBH_EVENT_ALL_FREE event");

    usbh_event_data_t event_data = {
        .event = USBH_EVENT_ALL_FREE,
    };
    invoke_usbh_event(&event_data);
    return ESP_OK;
}

esp_err_t usbh_mock_event_dev_suspend(uint8_t dev_addr, void *dev_obj)
{
    MOCK_USB_HOST_LAYER_EVENT_CHECK(callbacks()->usbh_event_cb != NULL, ESP_ERR_INVALID_STATE);
    ESP_LOGD(MOCK_USB_HOST_LAYER_EVENT_TAG, "Propagating USBH_EVENT_DEV_SUSPEND event");

    usbh_event_data_t event_data = {
        .event = USBH_EVENT_DEV_SUSPEND,
        .dev_suspend_resume_data = {
            .dev_addr = dev_addr,
            .dev_hdl = (usb_device_handle_t)dev_obj,
        },
    };
    invoke_usbh_event(&event_data);
    return ESP_OK;
}

esp_err_t usbh_mock_event_dev_resume(uint8_t dev_addr, void *dev_obj)
{
    MOCK_USB_HOST_LAYER_EVENT_CHECK(callbacks()->usbh_event_cb != NULL, ESP_ERR_INVALID_STATE);
    ESP_LOGD(MOCK_USB_HOST_LAYER_EVENT_TAG, "Propagating USBH_EVENT_DEV_RESUME event");

    usbh_event_data_t event_data = {
        .event = USBH_EVENT_DEV_RESUME,
        .dev_suspend_resume_data = {
            .dev_addr = dev_addr,
            .dev_hdl = (usb_device_handle_t)dev_obj,
        },
    };
    invoke_usbh_event(&event_data);
    return ESP_OK;
}

esp_err_t usbh_mock_event_all_idle(void)
{
    MOCK_USB_HOST_LAYER_EVENT_CHECK(callbacks()->usbh_event_cb != NULL, ESP_ERR_INVALID_STATE);
    ESP_LOGD(MOCK_USB_HOST_LAYER_EVENT_TAG, "Propagating USBH_EVENT_ALL_IDLE event");

    usbh_event_data_t event_data = {
        .event = USBH_EVENT_ALL_IDLE,
    };
    invoke_usbh_event(&event_data);
    return ESP_OK;
}

esp_err_t usbh_mock_event_dev_removed(uint8_t dev_addr)
{
    MOCK_USB_HOST_LAYER_EVENT_CHECK(callbacks()->usbh_event_cb != NULL, ESP_ERR_INVALID_STATE);
    ESP_LOGD(MOCK_USB_HOST_LAYER_EVENT_TAG, "Propagating USBH_EVENT_DEV_REMOVED event");

    usbh_event_data_t event_data = {
        .event = USBH_EVENT_DEV_REMOVED,
        .dev_removed_data = {
            .dev_addr = dev_addr,
        },
    };
    invoke_usbh_event(&event_data);
    return ESP_OK;
}

// ------------------------------------------------- Hub events --------------------------------------------------------

static void invoke_hub_event(const hub_event_data_t *event_data)
{
    callbacks()->hub_event_cb((hub_event_data_t *)event_data, NULL);
}

esp_err_t hub_mock_event_connected(unsigned int uid)
{
    MOCK_USB_HOST_LAYER_EVENT_CHECK(callbacks()->hub_event_cb != NULL, ESP_ERR_INVALID_STATE);
    ESP_LOGD(MOCK_USB_HOST_LAYER_EVENT_TAG, "Propagating HUB_EVENT_CONNECTED event");

    hub_event_data_t event_data = {
        .event = HUB_EVENT_CONNECTED,
        .connected = {
            .uid = uid,
        },
    };
    invoke_hub_event(&event_data);
    return ESP_OK;
}

esp_err_t hub_mock_event_reset_completed(unsigned int uid)
{
    MOCK_USB_HOST_LAYER_EVENT_CHECK(callbacks()->hub_event_cb != NULL, ESP_ERR_INVALID_STATE);
    ESP_LOGD(MOCK_USB_HOST_LAYER_EVENT_TAG, "Propagating HUB_EVENT_RESET_COMPLETED event");

    hub_event_data_t event_data = {
        .event = HUB_EVENT_RESET_COMPLETED,
        .reset_completed = {
            .uid = uid,
        },
    };
    invoke_hub_event(&event_data);
    return ESP_OK;
}

esp_err_t hub_mock_event_disconnected(unsigned int uid)
{
    MOCK_USB_HOST_LAYER_EVENT_CHECK(callbacks()->hub_event_cb != NULL, ESP_ERR_INVALID_STATE);
    ESP_LOGD(MOCK_USB_HOST_LAYER_EVENT_TAG, "Propagating HUB_EVENT_DISCONNECTED event");

    hub_event_data_t event_data = {
        .event = HUB_EVENT_DISCONNECTED,
        .disconnected = {
            .uid = uid,
        },
    };
    invoke_hub_event(&event_data);
    return ESP_OK;
}

// ------------------------------------------------- Enum events -------------------------------------------------------

static void invoke_enum_event(const enum_event_data_t *event_data)
{
    callbacks()->enum_event_cb((enum_event_data_t *)event_data, NULL);
}

esp_err_t enum_mock_event_started(unsigned int node_uid, usb_device_handle_t dev_hdl)
{
    MOCK_USB_HOST_LAYER_EVENT_CHECK(callbacks()->enum_event_cb != NULL, ESP_ERR_INVALID_STATE);
    ESP_LOGD(MOCK_USB_HOST_LAYER_EVENT_TAG, "Propagating ENUM_EVENT_STARTED event");

    enum_event_data_t event_data = {
        .event = ENUM_EVENT_STARTED,
        .node_uid = node_uid,
        .dev_hdl = dev_hdl,
    };
    invoke_enum_event(&event_data);
    return ESP_OK;
}

esp_err_t enum_mock_event_reset_required(unsigned int node_uid, usb_device_handle_t dev_hdl)
{
    MOCK_USB_HOST_LAYER_EVENT_CHECK(callbacks()->enum_event_cb != NULL, ESP_ERR_INVALID_STATE);
    ESP_LOGD(MOCK_USB_HOST_LAYER_EVENT_TAG, "Propagating ENUM_EVENT_RESET_REQUIRED event");

    enum_event_data_t event_data = {
        .event = ENUM_EVENT_RESET_REQUIRED,
        .node_uid = node_uid,
        .dev_hdl = dev_hdl,
    };
    invoke_enum_event(&event_data);
    return ESP_OK;
}

esp_err_t enum_mock_event_completed(unsigned int node_uid, usb_device_handle_t dev_hdl)
{
    MOCK_USB_HOST_LAYER_EVENT_CHECK(callbacks()->enum_event_cb != NULL, ESP_ERR_INVALID_STATE);
    ESP_LOGD(MOCK_USB_HOST_LAYER_EVENT_TAG, "Propagating ENUM_EVENT_COMPLETED event");

    enum_event_data_t event_data = {
        .event = ENUM_EVENT_COMPLETED,
        .node_uid = node_uid,
        .dev_hdl = dev_hdl,
    };
    invoke_enum_event(&event_data);
    return ESP_OK;
}

esp_err_t enum_mock_event_canceled(unsigned int node_uid, usb_device_handle_t dev_hdl)
{
    MOCK_USB_HOST_LAYER_EVENT_CHECK(callbacks()->enum_event_cb != NULL, ESP_ERR_INVALID_STATE);
    ESP_LOGD(MOCK_USB_HOST_LAYER_EVENT_TAG, "Propagating ENUM_EVENT_CANCELED event");

    enum_event_data_t event_data = {
        .event = ENUM_EVENT_CANCELED,
        .node_uid = node_uid,
        .dev_hdl = dev_hdl,
    };
    invoke_enum_event(&event_data);
    return ESP_OK;
}

// ------------------------------------------------- Processing requests -----------------------------------------------

static void invoke_proc_req(usb_proc_req_source_t source)
{
    callbacks()->proc_req_cb(source, false, NULL);
}

esp_err_t usbh_mock_proc_req(void)
{
    MOCK_USB_HOST_LAYER_EVENT_CHECK(callbacks()->proc_req_cb != NULL, ESP_ERR_INVALID_STATE);
    ESP_LOGD(MOCK_USB_HOST_LAYER_EVENT_TAG, "USBH processing request");

    invoke_proc_req(USB_PROC_REQ_SOURCE_USBH);
    return ESP_OK;
}

esp_err_t enum_mock_proc_req(void)
{
    MOCK_USB_HOST_LAYER_EVENT_CHECK(callbacks()->proc_req_cb != NULL, ESP_ERR_INVALID_STATE);
    ESP_LOGD(MOCK_USB_HOST_LAYER_EVENT_TAG, "Enum processing request");

    invoke_proc_req(USB_PROC_REQ_SOURCE_ENUM);
    return ESP_OK;
}

esp_err_t hub_mock_proc_req(void)
{
    MOCK_USB_HOST_LAYER_EVENT_CHECK(callbacks()->proc_req_cb != NULL, ESP_ERR_INVALID_STATE);
    ESP_LOGD(MOCK_USB_HOST_LAYER_EVENT_TAG, "Hub processing request");

    invoke_proc_req(USB_PROC_REQ_SOURCE_HUB);
    return ESP_OK;
}
