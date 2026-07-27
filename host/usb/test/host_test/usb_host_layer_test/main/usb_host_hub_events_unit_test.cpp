/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <catch2/catch_test_macros.hpp>
#include "usb_host_layer_test_fixture.hpp"

SCENARIO("USB Host hub event routing")
{
    GIVEN("An installed USB Host library with hub callbacks registered") {
        usb_host_layer_test::UsbHostLayerFixture host(usb_host_layer_test::MOCK_CB_HUB);
        host.install();

        SECTION("HUB_EVENT_CONNECTED starts enumeration") {
            constexpr unsigned kUid = 3;
            enum_start_ExpectAndReturn(kUid, ESP_OK);
            REQUIRE(ESP_OK == hub_mock_event_connected(kUid));
        }

        SECTION("HUB_EVENT_RESET_COMPLETED proceeds with enumeration") {
            constexpr unsigned kUid = 4;
            enum_proceed_ExpectAndReturn(kUid, ESP_OK);
            REQUIRE(ESP_OK == hub_mock_event_reset_completed(kUid));
        }

        SECTION("HUB_EVENT_DISCONNECTED cancels enumeration and removes the device") {
            constexpr unsigned kUid = 5;
            enum_cancel_ExpectAndReturn(kUid, ESP_OK);
            usbh_devs_remove_ExpectAndReturn(kUid, ESP_OK);
            REQUIRE(ESP_OK == hub_mock_event_disconnected(kUid));
        }

        usb_host_layer_test::drain_lib_events();
    }
}
