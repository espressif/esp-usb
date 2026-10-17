/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_log.h"
#include "mock_usb_host_layer_callbacks.h"
#include "mock_usb_host_layer_internal.h"
#include "usb/usb_types_stack.h"

#define MOCK_USB_HOST_LAYER_CB_CHECK(cond, ret_val) ({ \
    if (!(cond)) {                                     \
        return (ret_val);                              \
    }                                                  \
})

static const char *MOCK_USB_HOST_LAYER_CB_TAG = "Mock USB Host Cb";

static mock_usb_host_layer_callbacks_t s_callbacks = {};

mock_usb_host_layer_callbacks_t *mock_usb_host_layer_callbacks_get(void)
{
    return &s_callbacks;
}

esp_err_t usbh_register_mock_callback(const usbh_config_t *usbh_config, int call_count)
{
    (void)call_count;
    MOCK_USB_HOST_LAYER_CB_CHECK(usbh_config != NULL, ESP_ERR_INVALID_ARG);
    ESP_LOGD(MOCK_USB_HOST_LAYER_CB_TAG, "USBH register callback");

    s_callbacks.usbh_event_cb = usbh_config->event_cb;
    return ESP_OK;
}

esp_err_t usbh_deregister_mock_callback(int call_count)
{
    (void)call_count;
    ESP_LOGD(MOCK_USB_HOST_LAYER_CB_TAG, "USBH deregister callback");

    s_callbacks.usbh_event_cb = NULL;
    return ESP_OK;
}

esp_err_t enum_register_mock_callback(enum_config_t *enum_config, void **client_ret, int call_count)
{
    (void)call_count;
    MOCK_USB_HOST_LAYER_CB_CHECK(enum_config != NULL && client_ret != NULL, ESP_ERR_INVALID_ARG);
    ESP_LOGD(MOCK_USB_HOST_LAYER_CB_TAG, "Enum register callback");

    static int enum_client_ret;
    *client_ret = &enum_client_ret;

    s_callbacks.enum_event_cb = enum_config->enum_event_cb;
#if ENABLE_ENUM_FILTER_CALLBACK
    s_callbacks.enum_filter_cb = enum_config->enum_filter_cb;
#endif
    return ESP_OK;
}

esp_err_t enum_deregister_mock_callback(int call_count)
{
    (void)call_count;
    ESP_LOGD(MOCK_USB_HOST_LAYER_CB_TAG, "Enum deregister callback");

    s_callbacks.enum_event_cb = NULL;
#if ENABLE_ENUM_FILTER_CALLBACK
    s_callbacks.enum_filter_cb = NULL;
#endif
    return ESP_OK;
}

#if ENABLE_ENUM_FILTER_CALLBACK
usb_host_enum_filter_cb_t mock_usb_host_layer_get_enum_filter_cb(void)
{
    return mock_usb_host_layer_callbacks_get()->enum_filter_cb;
}
#endif

esp_err_t hub_register_mock_callback(hub_config_t *hub_config, void **client_ret, int call_count)
{
    (void)call_count;
    MOCK_USB_HOST_LAYER_CB_CHECK(hub_config != NULL && client_ret != NULL, ESP_ERR_INVALID_ARG);
    ESP_LOGD(MOCK_USB_HOST_LAYER_CB_TAG, "Hub register callback");

    static int hub_client_ret;
    *client_ret = &hub_client_ret;

    s_callbacks.hub_event_cb = hub_config->event_cb;
    return ESP_OK;
}

esp_err_t hub_deregister_mock_callback(int call_count)
{
    (void)call_count;
    ESP_LOGD(MOCK_USB_HOST_LAYER_CB_TAG, "Hub deregister callback");

    s_callbacks.hub_event_cb = NULL;
    return ESP_OK;
}
