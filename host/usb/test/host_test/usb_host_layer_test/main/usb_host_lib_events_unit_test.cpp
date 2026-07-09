/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <catch2/catch_test_macros.hpp>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb_host_layer_test_fixture.hpp"

namespace {

bool wait_for_lib_event_flags(uint32_t expected_flags, TickType_t timeout_ticks)
{
    uint32_t lib_flags = 0;
    if (usb_host_lib_handle_events(timeout_ticks, &lib_flags) != ESP_OK) {
        return false;
    }
    return (lib_flags & expected_flags) == expected_flags;
}

void expect_usbh_devs_num(int *num_devs_storage)
{
    usbh_devs_num_ExpectAnyArgsAndReturn(ESP_OK);
    usbh_devs_num_IgnoreArg_num_devs_ret();
    usbh_devs_num_ReturnThruPtr_num_devs_ret(num_devs_storage);
}

void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    (void)event_msg;
    (void)arg;
}

usb_host_client_handle_t register_client(void)
{
    usb_host_client_handle_t client_hdl = nullptr;
    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .flags = {},
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = nullptr,
        },
    };

    REQUIRE(ESP_OK == usb_host_client_register(&client_config, &client_hdl));
    return client_hdl;
}

void deregister_client(usb_host_client_handle_t client_hdl)
{
    REQUIRE(ESP_OK == usb_host_client_deregister(client_hdl));
}

} // namespace

SCENARIO("USB Host mock callback injectors require registration")
{
    SECTION("Injectors return ESP_ERR_INVALID_STATE before install") {
        REQUIRE(ESP_ERR_INVALID_STATE == hub_mock_event_connected(1));
        REQUIRE(ESP_ERR_INVALID_STATE == enum_mock_event_started(1, usb_host_layer_test::mock_device_handle()));
    }
}

SCENARIO("USB Host library NO_CLIENTS event routing")
{
    GIVEN("An installed USB Host library") {
        usb_host_layer_test::UsbHostLayerFixture host(usb_host_layer_test::MOCK_CB_NONE);
        host.install();

        SECTION("Last client deregistration sets NO_CLIENTS library event flag") {
            // Register and deregister client
            usb_host_client_handle_t client_hdl = register_client();
            deregister_client(client_hdl);

            // Drain USB Host lib events and expect USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS
            uint32_t lib_flags = usb_host_layer_test::drain_lib_events_flags();
            REQUIRE((lib_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) != 0);
        }

        SECTION("NO_CLIENTS is not set while other clients remain registered") {
            // Register 2 clients and deregister one client
            usb_host_client_handle_t client_a = register_client();
            usb_host_client_handle_t client_b = register_client();
            deregister_client(client_a);

            // Drain USB Host lib events and DON'T expect USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS
            uint32_t lib_flags = usb_host_layer_test::drain_lib_events_flags();
            REQUIRE((lib_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) == 0);

            // Deregister the last client, drain USB Host lib events and expect USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS
            deregister_client(client_b);
            lib_flags = usb_host_layer_test::drain_lib_events_flags();
            REQUIRE((lib_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) != 0);
        }

        usb_host_layer_test::drain_lib_events();
    }
}

SCENARIO("USB Host library AUTO_SUSPEND event routing")
{
    GIVEN("An installed USB Host library") {
        usb_host_layer_test::UsbHostLayerFixture host(usb_host_layer_test::MOCK_CB_NONE);
        host.install();

        SECTION("Auto suspend timer sets AUTO_SUSPEND when a device is connected") {
            // Inject device and resumed state state
            int num_devs = 1;
            expect_usbh_devs_num(&num_devs);
            hub_root_is_suspended_ExpectAndReturn(false);

            // Set auto suspend timer and wait for USB_HOST_LIB_EVENT_FLAGS_AUTO_SUSPEND flag
            REQUIRE(ESP_OK == usb_host_lib_set_auto_suspend(USB_HOST_LIB_AUTO_SUSPEND_ONE_SHOT, 50));
            REQUIRE(wait_for_lib_event_flags(USB_HOST_LIB_EVENT_FLAGS_AUTO_SUSPEND, pdMS_TO_TICKS(500)));

            // Stop the auto suspend timer
            REQUIRE(ESP_OK == usb_host_lib_set_auto_suspend(USB_HOST_LIB_AUTO_SUSPEND_ONE_SHOT, 0));
        }

        SECTION("Auto suspend timer does not set AUTO_SUSPEND when no device is connected") {
            // Inject NO device
            int num_devs = 0;
            expect_usbh_devs_num(&num_devs);

            // Set auto suspend timer
            REQUIRE(ESP_OK == usb_host_lib_set_auto_suspend(USB_HOST_LIB_AUTO_SUSPEND_ONE_SHOT, 50));

            // Don't expect USB_HOST_LIB_EVENT_FLAGS_AUTO_SUSPEND to be set
            uint32_t lib_flags = 0;
            REQUIRE(ESP_ERR_TIMEOUT == usb_host_lib_handle_events(pdMS_TO_TICKS(200), &lib_flags));
            REQUIRE((lib_flags & USB_HOST_LIB_EVENT_FLAGS_AUTO_SUSPEND) == 0);

            // Stop the auto suspend timer
            REQUIRE(ESP_OK == usb_host_lib_set_auto_suspend(USB_HOST_LIB_AUTO_SUSPEND_ONE_SHOT, 0));
        }

        usb_host_layer_test::drain_lib_events();
    }
}

SCENARIO("USB Host library ALL_FREE event routing")
{
    GIVEN("An installed USB Host library with USBH callbacks registered") {
        usb_host_layer_test::UsbHostLayerFixture host(usb_host_layer_test::MOCK_CB_USBH);
        host.install();

        SECTION("USBH_EVENT_ALL_FREE sets the ALL_FREE library event flag") {
            REQUIRE(ESP_OK == usbh_mock_event_all_free());

            uint32_t lib_flags = usb_host_layer_test::drain_lib_events_flags();
            REQUIRE((lib_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) != 0);
        }

        usb_host_layer_test::drain_lib_events();
    }
}

SCENARIO("USB Host library NO_CLIENTS and ALL_FREE event routing")
{
    GIVEN("An installed USB Host library with USBH callbacks registered") {
        usb_host_layer_test::UsbHostLayerFixture host(usb_host_layer_test::MOCK_CB_USBH);
        host.install();

        SECTION("Last client deregistration and ALL_FREE deliver both library event flags together") {
            usb_host_client_handle_t client_hdl = register_client();
            deregister_client(client_hdl);

            REQUIRE(ESP_OK == usbh_mock_event_all_free());

            uint32_t lib_flags = usb_host_layer_test::drain_lib_events_flags();
            REQUIRE((lib_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) != 0);
            REQUIRE((lib_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) != 0);
        }

        usb_host_layer_test::drain_lib_events();
    }
}
