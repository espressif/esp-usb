/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <catch2/catch_test_macros.hpp>
#include "usb_host_layer_test_fixture.hpp"

SCENARIO("USB Host enumeration event routing")
{
    GIVEN("An installed USB Host library with enum callbacks registered") {
        usb_host_layer_test::UsbHostLayerFixture host(usb_host_layer_test::MOCK_CB_ENUM);
        host.install();

        SECTION("ENUM_EVENT_STARTED is accepted without hub side effects") {
            constexpr unsigned kNodeUid = 7;
            usb_device_handle_t dev_hdl = usb_host_layer_test::mock_device_handle();
            REQUIRE(ESP_OK == enum_mock_event_started(kNodeUid, dev_hdl));
        }

        SECTION("ENUM_EVENT_RESET_REQUIRED resets the hub node") {
            constexpr unsigned kNodeUid = 8;
            usb_device_handle_t dev_hdl = usb_host_layer_test::mock_device_handle(1);
            hub_node_reset_ExpectAndReturn(kNodeUid, ESP_OK);
            REQUIRE(ESP_OK == enum_mock_event_reset_required(kNodeUid, dev_hdl));
        }

        SECTION("ENUM_EVENT_COMPLETED marks the node active and triggers a new device event") {
            constexpr unsigned kNodeUid = 9;
            usb_device_handle_t dev_hdl = usb_host_layer_test::mock_device_handle(2);
            hub_node_active_ExpectAndReturn(kNodeUid, ESP_OK);
            usbh_devs_new_dev_event_ExpectAndReturn(dev_hdl, ESP_OK);
            REQUIRE(ESP_OK == enum_mock_event_completed(kNodeUid, dev_hdl));
        }

        SECTION("ENUM_EVENT_CANCELED disables the hub node") {
            constexpr unsigned kNodeUid = 10;
            usb_device_handle_t dev_hdl = usb_host_layer_test::mock_device_handle(3);
            hub_node_disable_ExpectAndReturn(kNodeUid, ESP_OK);
            REQUIRE(ESP_OK == enum_mock_event_canceled(kNodeUid, dev_hdl));
        }

        uint32_t lib_flags = usb_host_layer_test::drain_lib_events_flags();
        REQUIRE(lib_flags == 0);
    }
}
