/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <catch2/catch_test_macros.hpp>

#include "usb/msc_host_vfs.h"
#include "mock_add_usb_device.h"

extern "C" {
#include "Mockusb_host.h"
}

SCENARIO("MSC Host pre-uninstall")
{
    // No MSC Driver installed
    GIVEN("NO MSC Host previously installed") {

        // Uninstall not-installed MAC Host driver
        SECTION("Uninstalling not installed MSC Host returns error") {
            // Call the DUT function, expect ESP_ERR_INVALID_STATE
            REQUIRE(ESP_ERR_INVALID_STATE == msc_host_uninstall());
        }
    }
}

SCENARIO("MSC Host install")
{
    msc_host_driver_config_t msc_host_driver_config = {
        .create_backround_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = (reinterpret_cast<msc_host_event_cb_t>(0xdeadbeef)),
        .callback_arg = nullptr,
    };

    // MSC Host driver config set to nullptr
    GIVEN("NO MSC Host driver config, driver not already installed") {

        SECTION("Config is nullptr") {
            // Call the DUT function msc_host_install with msc_host_driver_config set to nullptr
            REQUIRE(ESP_ERR_INVALID_ARG == msc_host_install(nullptr));
        }
    }

    // MSC Host driver config set to config from MSC Host example,
    // but with various various changes causing MSC Host driver installation to fail
    GIVEN("Minimal MSC Host driver config, driver not already installed") {

        SECTION("Config error: no callback") {
            msc_host_driver_config.callback = nullptr;
            // Call the DUT function, expect ESP_ERR_INVALID_ARG
            REQUIRE(ESP_ERR_INVALID_ARG == msc_host_install(&msc_host_driver_config));
        }

        SECTION("Config error: stack size is 0") {
            msc_host_driver_config.stack_size = 0;
            // Call the DUT function, expect ESP_ERR_INVALID_ARG
            REQUIRE(ESP_ERR_INVALID_ARG == msc_host_install(&msc_host_driver_config));
        }

        SECTION("Config error: task priority is 0") {
            msc_host_driver_config.task_priority = 0;
            // Call the DUT function, expect ESP_ERR_INVALID_ARG
            REQUIRE(ESP_ERR_INVALID_ARG == msc_host_install(&msc_host_driver_config));
        }
    }

    // MSC Host driver config set to config from MSC Host example
    GIVEN("Full MSC Host config, driver not already installed") {

        // Unable to register client: Invalid state, goto fail
        SECTION("Client register not successful: goto fail") {
            // Register a client, return ESP_ERR_INVALID_STATE, so the client is not registered successfully
            usb_host_client_register_ExpectAnyArgsAndReturn(ESP_ERR_INVALID_STATE);

            // goto fail: delete the semaphore and deregister client
            usb_host_client_deregister_ExpectAnyArgsAndReturn(ESP_OK);

            // Call the DUT function, expect ESP_ERR_INVALID_STATE
            REQUIRE(ESP_ERR_INVALID_STATE == msc_host_install(&msc_host_driver_config));
        }

        // Call msc_host_install and expect successful installation
        SECTION("Client register successful: msc_host_install successful") {

            // Register a client, return ESP_OK, so the client is registered successfully
            usb_host_client_register_ExpectAnyArgsAndReturn(ESP_OK);
            usb_host_client_register_AddCallback(usb_host_client_register_mock_callback);

            // create USB Host Library client processing function
            usb_host_client_handle_events_ExpectAnyArgsAndReturn(ESP_OK);
            usb_host_client_handle_events_AddCallback(usb_host_client_handle_events_mock_callback);

            // Call the DUT function, expect ESP_OK
            REQUIRE(ESP_OK == msc_host_install(&msc_host_driver_config));
        }
    }

    // MSC Host driver config set to config from MSC Host example
    GIVEN("Full MSC Host config, driver already installed") {

        // Driver is already installed
        SECTION("Install error: driver already installed") {
            // Call the DUT function, expect ESP_ERR_INVALID_STATE
            REQUIRE(ESP_ERR_INVALID_STATE == msc_host_install(&msc_host_driver_config));
        }
    }
}

SCENARIO("MSC Host post-uninstall")
{
    // MSC Host driver successfully installed
    GIVEN("MSC Host previously installed") {

        // MSC Driver is installed, uninstall the driver
        SECTION("MSC Host driver uninstall successful") {

            // Unblock client, return ESP_OK, register callback for unblocking the USB Host Library client processing function
            usb_host_client_unblock_ExpectAnyArgsAndReturn(ESP_OK);
            usb_host_client_unblock_AddCallback(usb_host_client_unblock_mock_callback);

            // Deregister the client
            usb_host_client_deregister_ExpectAnyArgsAndReturn(ESP_OK);
            usb_host_client_deregister_AddCallback(usb_host_client_deregister_mock_callback);

            // Call the DUT function, expect ESP_OK
            REQUIRE(ESP_OK == msc_host_uninstall());
        }
    }

    // MSC Host driver successfully uninstalled
    GIVEN("MSC Host successfully uninstalled") {

        // MSC Driver already successfully uninstalled, uninstall it again
        SECTION("Uninstall already uninstalled MSC Host driver") {
            // Call the DUT function, expect error
            REQUIRE(ESP_ERR_INVALID_STATE == msc_host_uninstall());
        }
    }
}
