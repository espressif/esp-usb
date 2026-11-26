/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <cstring>
#include <initializer_list>
#include <catch2/catch_test_macros.hpp>
#include "usb/cdc_acm_host.h"
#include "esp_private/cdc_host_common.h"

extern "C" {
#include "Mockcdc_host_common.h"
#include "Mockqueue.h"
#include "Mockportmacro.h"
}

static cdc_host_common_open_config_t common_config;
static int port_storage;
static int mutex_storage;
static const auto common_port = reinterpret_cast<cdc_host_common_port_handle_t>(&port_storage);
static usb_intf_desc_t interface_desc;
static bool disconnect_during_open;
static cdc_host_common_dev_event_cb_t new_device_callback;

static esp_err_t register_device_callback(cdc_host_common_driver_handle_t, cdc_host_common_dev_event_cb_t callback, void *, cdc_host_common_dev_event_cb_handle_t *, int)
{
    new_device_callback = callback;
    return ESP_OK;
}

static void uninstall_from_new_device(usb_device_handle_t)
{
    REQUIRE(ESP_ERR_INVALID_STATE == cdc_acm_host_uninstall());
}

struct callback_state {
    bool close_in_callback = false;
    bool callback_active = false;
    unsigned events = 0;
    unsigned deletes = 0;
    cdc_acm_uart_state_t serial_state = {};
    cdc_acm_host_dev_event_t last_event = CDC_ACM_HOST_ERROR;
    bool network_connected = false;
};

static void delete_context(cdc_acm_dev_hdl_t dev);

static esp_err_t open_port(cdc_host_common_driver_handle_t, const cdc_host_common_open_config_t *config, cdc_host_common_port_handle_t *port, int)
{
    common_config = *config;
    REQUIRE(config->ctrl_buffer_size == 64);
    *port = common_port;
    if (disconnect_during_open) {
        auto *dev = static_cast<cdc_dev_t *>(config->user_arg);
        dev->intf_func.del = delete_context;
        dev->intf_func.user_data = dev->cb_arg;
        cdc_host_common_port_event_data_t event = {};
        event.type = CDC_HOST_COMMON_PORT_EVENT_DISCONNECTED;
        config->event_cb(common_port, &event, config->user_arg);
    }
    return ESP_OK;
}

static esp_err_t get_interfaces(cdc_host_common_port_handle_t, const usb_intf_desc_t **notif, const usb_intf_desc_t **data, int)
{
    *notif = &interface_desc;
    *data = &interface_desc;
    return ESP_OK;
}

static void delete_context(cdc_acm_dev_hdl_t dev)
{
    auto *state = static_cast<callback_state *>(dev->intf_func.user_data);
    REQUIRE_FALSE(state->callback_active);
    REQUIRE(dev->data.intf_desc != nullptr);
    REQUIRE(dev->data.intf_desc->bInterfaceNumber == 7);
    ++state->deletes;
}

static void device_event(const cdc_acm_host_dev_event_data_t *event, void *arg)
{
    auto *state = static_cast<callback_state *>(arg);
    state->callback_active = true;
    ++state->events;
    state->last_event = event->type;
    if (event->type == CDC_ACM_HOST_SERIAL_STATE) {
        state->serial_state = event->data.serial_state;
    } else if (event->type == CDC_ACM_HOST_NETWORK_CONNECTION) {
        state->network_connected = event->data.network_connected;
    }
    if (state->close_in_callback) {
        REQUIRE(ESP_OK == cdc_acm_host_close(event->data.cdc_hdl));
        REQUIRE(state->deletes == 0);
    }
    state->callback_active = false;
}

static bool receive_and_close(const uint8_t *, size_t, void *arg)
{
    auto *dev = static_cast<cdc_dev_t *>(common_config.user_arg);
    auto *state = static_cast<callback_state *>(arg);
    state->callback_active = true;
    REQUIRE(ESP_OK == cdc_acm_host_close(dev));
    REQUIRE(state->deletes == 0);
    state->callback_active = false;
    return true;
}

static void install_mock_driver()
{
    Mockcdc_host_common_Init();
    Mockqueue_Init();
    Mockportmacro_Init();
    vPortEnterCritical_Ignore();
    vPortExitCritical_Ignore();
    xQueueCreateMutex_IgnoreAndReturn(reinterpret_cast<QueueHandle_t>(&mutex_storage));
    xQueueSemaphoreTake_IgnoreAndReturn(pdTRUE);
    xQueueGenericSend_IgnoreAndReturn(pdTRUE);
    vQueueDelete_Ignore();
    cdc_host_common_acquire_IgnoreAndReturn(ESP_OK);
    cdc_host_common_register_dev_event_cb_Stub(register_device_callback);
    cdc_host_common_open_Stub(open_port);
    cdc_host_common_get_intf_desc_Stub(get_interfaces);
    cdc_host_common_cdc_desc_get_IgnoreAndReturn(ESP_ERR_NOT_FOUND);
    disconnect_during_open = false;
    interface_desc.bInterfaceNumber = 7;
    REQUIRE(ESP_OK == cdc_acm_host_install(nullptr));
}

static void uninstall_mock_driver()
{
    cdc_host_common_unregister_dev_event_cb_ExpectAnyArgsAndReturn(ESP_OK);
    cdc_host_common_release_ExpectAnyArgsAndReturn(ESP_OK);
    REQUIRE(ESP_OK == cdc_acm_host_uninstall());
    Mockcdc_host_common_Verify();
    Mockcdc_host_common_Destroy();
    Mockqueue_Destroy();
    Mockportmacro_Destroy();
}

TEST_CASE("ACM handles survive disconnect until explicit close", "[acm_lifecycle]")
{
    install_mock_driver();

    callback_state state;
    bool release_blocked = false;
    bool close_blocked = false;
    disconnect_during_open = false;
    cdc_acm_host_device_config_t config = {};
    config.user_arg = &state;
    interface_desc.bInterfaceNumber = 7;

    SECTION("No event callback") {}
    SECTION("New-device callback cannot uninstall its driver") {
        REQUIRE(ESP_OK == cdc_acm_host_register_new_dev_callback(uninstall_from_new_device));
        cdc_host_common_dev_event_data_t event = {};
        event.type = CDC_HOST_COMMON_DEV_EVENT_NEW;
        new_device_callback(&event, nullptr);
    }
    SECTION("Failed uninstall can be retried") {
        release_blocked = true;
    }
    SECTION("Timed-out close can be retried") {
        close_blocked = true;
    }
    SECTION("Delayed close") {
        config.event_cb = device_event;
    }
    SECTION("Close from disconnect callback") {
        config.event_cb = device_event;
        state.close_in_callback = true;
    }
    SECTION("Close from data callback") {
        config.data_cb = receive_and_close;
    }
    SECTION("Overflow preserves cached UART state") {
        config.event_cb = device_event;
    }
    SECTION("Disconnect callback closes while open is still publishing") {
        config.event_cb = device_event;
        config.data_cb = receive_and_close;
        state.close_in_callback = true;
        disconnect_during_open = true;
    }

    cdc_acm_dev_hdl_t dev = nullptr;
    if (disconnect_during_open) {
        cdc_host_common_close_async_ExpectAndReturn(common_port, ESP_OK);
        REQUIRE(ESP_ERR_INVALID_STATE == cdc_acm_host_open(1, 2, 0, &config, &dev));
        REQUIRE(dev == nullptr);
        const unsigned events_before = state.events;
        cdc_host_common_port_event_data_t late_event = {};
        late_event.type = CDC_HOST_COMMON_PORT_EVENT_DISCONNECTED;
        common_config.event_cb(common_port, &late_event, common_config.user_arg);
        REQUIRE(state.events == events_before);
        REQUIRE(common_config.data_cb(common_port, nullptr, 0, common_config.user_arg));
    } else {
        REQUIRE(ESP_OK == cdc_acm_host_open(1, 2, 0, &config, &dev));
        dev->intf_func.user_data = &state;
        dev->intf_func.del = delete_context;
        REQUIRE(dev->data.intf_desc != &interface_desc);
        interface_desc.bInterfaceNumber = 0;
        REQUIRE(dev->data.intf_desc->bInterfaceNumber == 7);

        // A recycled ACM address must not accept an old common port's callback.
        int other_port_storage;
        const auto other_port = reinterpret_cast<cdc_host_common_port_handle_t>(&other_port_storage);
        cdc_host_common_port_event_data_t stale_event = {};
        stale_event.type = CDC_HOST_COMMON_PORT_EVENT_DISCONNECTED;
        common_config.event_cb(other_port, &stale_event, common_config.user_arg);
        REQUIRE(state.events == 0);
        REQUIRE_FALSE(dev->disconnected);

        if (config.event_cb && !state.close_in_callback) {
            uint8_t notification[sizeof(cdc_notification_t) + sizeof(uint16_t)] = {};
            auto *header = reinterpret_cast<cdc_notification_t *>(notification);
            header->bNotificationCode = USB_CDC_NOTIF_SERIAL_STATE;
            header->wLength = sizeof(uint16_t);
            const uint16_t uart_state = 3;
            memcpy(header->Data, &uart_state, sizeof(uart_state));
            cdc_host_common_port_event_data_t event = {};
            event.type = CDC_HOST_COMMON_PORT_EVENT_NOTIFICATION;
            event.data.notification.data = notification;
            event.data.notification.data_len = sizeof(notification);
            common_config.event_cb(common_port, &event, common_config.user_arg);
            REQUIRE(dev->serial_state.val == uart_state);
            event.type = CDC_HOST_COMMON_PORT_EVENT_RX_OVERFLOW;
            for (unsigned i = 0; i < 2; ++i) {
                common_config.event_cb(common_port, &event, common_config.user_arg);
                REQUIRE(state.serial_state.bOverRun == 1);
                REQUIRE(dev->serial_state.val == uart_state);
            }
        }

        if (close_blocked) {
            cdc_host_common_close_ExpectAndReturn(common_port, ESP_ERR_TIMEOUT);
        }
        cdc_host_common_close_ExpectAndReturn(common_port, ESP_OK);
        if (config.data_cb) {
            const uint8_t byte = 0;
            REQUIRE(common_config.data_cb(common_port, &byte, sizeof(byte), common_config.user_arg));
        } else {
            cdc_host_common_port_event_data_t event = {};
            event.type = CDC_HOST_COMMON_PORT_EVENT_DISCONNECTED;
            common_config.event_cb(common_port, &event, common_config.user_arg);
            if (!state.close_in_callback) {
                REQUIRE(state.deletes == 0);
                REQUIRE(ESP_ERR_INVALID_STATE == cdc_acm_host_uninstall());
                uint8_t data = 0;
                cdc_acm_line_coding_t coding = {};
                REQUIRE(ESP_ERR_INVALID_STATE == cdc_acm_host_data_tx_blocking(dev, &data, sizeof(data), 10));
                REQUIRE(ESP_ERR_INVALID_STATE == cdc_acm_host_send_custom_request(dev, 0x21, 1, 0, 0, 0, nullptr));
                REQUIRE(ESP_ERR_INVALID_STATE == cdc_acm_host_line_coding_get(dev, &coding));
                REQUIRE(ESP_ERR_INVALID_STATE == cdc_acm_host_protocols_get(dev, nullptr, nullptr));
                if (close_blocked) {
                    REQUIRE(ESP_ERR_TIMEOUT == cdc_acm_host_close(dev));
                    REQUIRE(state.deletes == 0);
                }
                REQUIRE(ESP_OK == cdc_acm_host_close(dev));
            }
        }
    }
    REQUIRE(state.deletes == 1);
    if (release_blocked) {
        cdc_host_common_unregister_dev_event_cb_ExpectAnyArgsAndReturn(ESP_OK);
        cdc_host_common_release_ExpectAnyArgsAndReturn(ESP_ERR_INVALID_STATE);
        REQUIRE(ESP_ERR_INVALID_STATE == cdc_acm_host_uninstall());
    }
    cdc_host_common_unregister_dev_event_cb_ExpectAnyArgsAndReturn(ESP_OK);
    cdc_host_common_release_ExpectAnyArgsAndReturn(ESP_OK);
    REQUIRE(ESP_OK == cdc_acm_host_uninstall());
    cdc_host_common_port_event_data_t late_event = {};
    late_event.type = CDC_HOST_COMMON_PORT_EVENT_DISCONNECTED;
    const unsigned events_before = state.events;
    common_config.event_cb(common_port, &late_event, common_config.user_arg);
    REQUIRE(state.events == events_before);
    Mockcdc_host_common_Verify();
    Mockcdc_host_common_Destroy();
    Mockqueue_Destroy();
    Mockportmacro_Destroy();
}

TEST_CASE("ACM validates notification lengths and translates supported notifications", "[acm_lifecycle]")
{
    install_mock_driver();
    callback_state state;
    cdc_acm_host_device_config_t config = {};
    config.event_cb = device_event;
    config.user_arg = &state;
    cdc_acm_dev_hdl_t dev = nullptr;
    REQUIRE(cdc_acm_host_open(1, 2, 0, &config, &dev) == ESP_OK);
    dev->serial_state.val = 3;

    uint8_t notification[sizeof(cdc_notification_t) + sizeof(uint16_t)] = {};
    auto *header = reinterpret_cast<cdc_notification_t *>(notification);
    header->bNotificationCode = USB_CDC_NOTIF_SERIAL_STATE;
    header->wLength = sizeof(uint16_t);
    const uint16_t uart_state = 0x42;
    memcpy(header->Data, &uart_state, sizeof(uart_state));
    cdc_host_common_port_event_data_t event = {};
    event.type = CDC_HOST_COMMON_PORT_EVENT_NOTIFICATION;
    event.data.notification.data = notification;
    event.data.notification.data_len = sizeof(notification);

    SECTION("Every truncated header or bitmap is ignored") {
        for (size_t length = 0; length < sizeof(notification); ++length) {
            CAPTURE(length);
            event.data.notification.data_len = length;
            common_config.event_cb(common_port, &event, common_config.user_arg);
            CHECK(state.events == 0);
            CHECK(dev->serial_state.val == 3);
        }
    }
    SECTION("A serial bitmap requires a matching declared payload length") {
        for (uint16_t length : {
                    0, 1, 3
                }) {
            CAPTURE(length);
            header->wLength = length;
            common_config.event_cb(common_port, &event, common_config.user_arg);
            CHECK(state.events == 0);
            CHECK(dev->serial_state.val == 3);
        }
    }
    SECTION("Network status is delivered without a payload") {
        header->bNotificationCode = USB_CDC_NOTIF_NETWORK_CONNECTION;
        header->wLength = 0;
        event.data.notification.data_len = sizeof(cdc_notification_t);
        for (uint16_t connected : {
                    0, 1
                }) {
            header->wValue = connected;
            common_config.event_cb(common_port, &event, common_config.user_arg);
            CHECK(state.events == connected + 1);
            CHECK(state.last_event == CDC_ACM_HOST_NETWORK_CONNECTION);
            CHECK(state.network_connected == static_cast<bool>(connected));
            CHECK(dev->serial_state.val == 3);
        }
    }
    SECTION("Unsupported notifications preserve the cached UART state") {
        for (auto code : {
                    USB_CDC_NOTIF_RESPONSE_AVAILABLE, USB_CDC_NOTIF_CONNECTION_SPEED_CHANGE
                }) {
            CAPTURE(code);
            header->bNotificationCode = code;
            common_config.event_cb(common_port, &event, common_config.user_arg);
            CHECK(state.events == 0);
            CHECK(dev->serial_state.val == 3);
        }
    }

    // A rejected packet must not disable the next complete notification.
    const unsigned previous_events = state.events;
    header->bNotificationCode = USB_CDC_NOTIF_SERIAL_STATE;
    header->wLength = sizeof(uint16_t);
    event.data.notification.data_len = sizeof(notification);
    common_config.event_cb(common_port, &event, common_config.user_arg);
    REQUIRE(state.events == previous_events + 1);
    REQUIRE(state.last_event == CDC_ACM_HOST_SERIAL_STATE);
    REQUIRE(state.serial_state.val == uart_state);
    REQUIRE(dev->serial_state.val == uart_state);
    cdc_host_common_close_ExpectAndReturn(common_port, ESP_OK);
    REQUIRE(cdc_acm_host_close(dev) == ESP_OK);
    uninstall_mock_driver();
}

TEST_CASE("ACM accepts events without optional callbacks and leaves RX disabled", "[acm_lifecycle]")
{
    install_mock_driver();
    callback_state state;
    cdc_acm_host_device_config_t config = {};
    config.user_arg = &state;
    cdc_acm_dev_hdl_t dev = nullptr;
    REQUIRE(cdc_acm_host_open(1, 2, 0, &config, &dev) == ESP_OK);
    dev->intf_func.user_data = &state;
    dev->intf_func.del = delete_context;
    REQUIRE(common_config.data_cb == nullptr);
    REQUIRE(common_config.in_buffer_size == 0);

    cdc_host_common_port_event_data_t event = {};
    for (auto type : {
                CDC_HOST_COMMON_PORT_EVENT_ERROR, CDC_HOST_COMMON_PORT_EVENT_NOTIFICATION, CDC_HOST_COMMON_PORT_EVENT_RX_OVERFLOW
            }) {
        event.type = type;
        common_config.event_cb(common_port, &event, common_config.user_arg);
        REQUIRE_FALSE(dev->disconnected);
        REQUIRE(state.events == 0);
    }
    event.type = CDC_HOST_COMMON_PORT_EVENT_DISCONNECTED;
    common_config.event_cb(common_port, &event, common_config.user_arg);
    REQUIRE(dev->disconnected);
    REQUIRE(state.deletes == 0);
    cdc_host_common_close_ExpectAndReturn(common_port, ESP_OK);
    REQUIRE(cdc_acm_host_close(dev) == ESP_OK);
    REQUIRE(state.deletes == 1);
    uninstall_mock_driver();
}

TEST_CASE("ACM remains usable after an online close fails", "[acm_lifecycle]")
{
    install_mock_driver();
    callback_state state;
    cdc_acm_host_device_config_t config = {};
    config.event_cb = device_event;
    config.user_arg = &state;
    cdc_acm_dev_hdl_t dev = nullptr;
    REQUIRE(cdc_acm_host_open(1, 2, 0, &config, &dev) == ESP_OK);
    dev->intf_func.user_data = &state;
    dev->intf_func.del = delete_context;
    esp_err_t close_error = ESP_ERR_TIMEOUT;

    SECTION("Close timed out") {}
    SECTION("Common close rejected its current state") {
        close_error = ESP_ERR_INVALID_STATE;
    }

    cdc_host_common_close_ExpectAndReturn(common_port, close_error);
    REQUIRE(cdc_acm_host_close(dev) == close_error);
    REQUIRE_FALSE(dev->closing);
    REQUIRE_FALSE(dev->disconnected);
    REQUIRE(state.deletes == 0);
    REQUIRE(cdc_acm_host_uninstall() == ESP_ERR_INVALID_STATE);
    const uint8_t data[] = {0x12, 0x34};
    cdc_host_common_write_bytes_ExpectAndReturn(common_port, data, sizeof(data), pdMS_TO_TICKS(10), ESP_OK);
    REQUIRE(cdc_acm_host_data_tx_blocking(dev, data, sizeof(data), 10) == ESP_OK);
    cdc_host_common_port_event_data_t event = {};
    event.type = CDC_HOST_COMMON_PORT_EVENT_ERROR;
    event.data.error = ESP_FAIL;
    common_config.event_cb(common_port, &event, common_config.user_arg);
    REQUIRE(state.events == 1);
    REQUIRE(state.last_event == CDC_ACM_HOST_ERROR);
    cdc_host_common_close_ExpectAndReturn(common_port, ESP_OK);
    REQUIRE(cdc_acm_host_close(dev) == ESP_OK);
    REQUIRE(state.deletes == 1);
    uninstall_mock_driver();
}
