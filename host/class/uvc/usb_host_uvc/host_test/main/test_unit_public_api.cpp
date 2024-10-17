
/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <catch2/catch_test_macros.hpp>

#include "libuvc/libuvc.h"
#include "libuvc_helper.h"
#include "libuvc_adapter.h"

extern "C" {
#include "Mockusb_host.h"
}

SCENARIO("UVC Host Handle events")
{
    GIVEN("Libusb has not been initialized") {

        // Call libuvc_adapter_handle_events and expect ESP_ERR_INVALID_STATE
        SECTION("s_uvc_driver is NULL") {
            REQUIRE(ESP_ERR_INVALID_STATE == libuvc_adapter_handle_events(0));
        }
    }
}
