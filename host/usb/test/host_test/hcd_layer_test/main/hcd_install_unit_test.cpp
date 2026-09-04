/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <catch2/catch_test_macros.hpp>

#include "hcd.h"   // Real implementation of hcd.h

#include "hcd_test_helpers.h"

// Test all the mocked headers defined for this mock
extern "C" {
#include "Mockusb_private.h"
#include "Mockesp_intr_alloc.h"
}

SCENARIO("HCD Port init - invalid arguments")
{
    hcd_port_config_t port_config = {};
    hcd_port_handle_t port_hdl = nullptr;

    GIVEN("Invalid arguments, HCD port not initialized") {

        // Try to init the HCD port with hcd_port_config set to nullptr
        SECTION("HCD port config is NULL, HCD port handle is NULL") {

            // Call the DUT function, expect ESP_ERR_INVALID_ARG
            REQUIRE(ESP_ERR_INVALID_ARG == hcd_port_init(1, nullptr, nullptr));
        }

        // Try to init the HCD port with port handle set to nullptr
        SECTION("HCD port handle is NULL") {

            // Call the DUT function, expect ESP_ERR_INVALID_ARG
            REQUIRE(ESP_ERR_INVALID_ARG == hcd_port_init(1, &port_config, nullptr));
        }

        // Try to init the HCD port with a negative port number
        SECTION("Negative port number") {

            // Call the DUT function, expect ESP_ERR_INVALID_ARG
            REQUIRE(ESP_ERR_INVALID_ARG == hcd_port_init(-1, &port_config, &port_hdl));
        }

        // Try to init the HCD port with a port number that does not exist
        SECTION("Port number out of range") {

            // Call the DUT function, expect ESP_ERR_NOT_FOUND
            REQUIRE(ESP_ERR_NOT_FOUND == hcd_port_init(100, &port_config, &port_hdl));
        }

        // Try to init the HCD port with an invalid custom FIFO config (RX and NPTX FIFO must be > 0)
        SECTION("Invalid custom FIFO config") {
            hcd_fifo_settings_t fifo_config = {
                .nptx_fifo_lines = 0,
                .ptx_fifo_lines = 128,
                .rx_fifo_lines = 0,
            };
            hcd_port_config_t port_config_fifo = {};
            port_config_fifo.fifo_config = &fifo_config;

            // Call the DUT function, expect ESP_ERR_INVALID_SIZE
            REQUIRE(ESP_ERR_INVALID_SIZE == hcd_port_init(1, &port_config_fifo, &port_hdl));
        }
    }
}

SCENARIO("HCD Port init - interrupt allocation failure")
{
    hcd_port_config_t port_config = {};
    hcd_port_handle_t port_hdl = nullptr;

    // HCD port config is valid, HCD port is not initialized
    GIVEN("HCD port config, HCD port not initialized") {

        // Make the interrupt allocation fail
        esp_intr_alloc_ExpectAnyArgsAndReturn(ESP_ERR_NOT_FOUND);

        SECTION("Fail to init the HCD port") {

            // Call the DUT function, expect ESP_ERR_NOT_FOUND
            REQUIRE(ESP_ERR_NOT_FOUND == hcd_port_init(1, &port_config, &port_hdl));
        }
    }
}

SCENARIO("HCD Port init and deinit")
{
    int context = 0;
    hcd_port_config_t port_config = {};
    port_config.context = &context;
    hcd_port_handle_t port_hdl = nullptr;

    // HCD port config is valid, HCD port is not initialized
    GIVEN("HCD port config, HCD port not initialized") {

        // Register the HAL mock callbacks (usb_dwc_hal_init/deinit)
        hcd_mock_register_hal_callbacks();

        // Expectations for a successful hcd_port_init()
        esp_intr_alloc_ExpectAnyArgsAndReturn(ESP_OK);
        esp_intr_enable_ExpectAnyArgsAndReturn(ESP_OK);

        // Successfully init the HCD port
        SECTION("Successfully init the HCD port") {

            // Call the DUT function, expect ESP_OK
            REQUIRE(ESP_OK == hcd_port_init(1, &port_config, &port_hdl));
            REQUIRE(port_hdl != nullptr);

            // After init, the port must be in the NOT_POWERED state
            SECTION("Port is in NOT_POWERED state after init") {
                REQUIRE(HCD_PORT_STATE_NOT_POWERED == hcd_port_get_state(port_hdl));
            }

            // The context variable set in the config must be associated with the port
            SECTION("Port context can be retrieved") {
                REQUIRE(&context == hcd_port_get_context(port_hdl));
            }

            // Try to init the already initialized port
            SECTION("Init the same port again") {

                // Call the DUT function, expect ESP_ERR_INVALID_STATE
                REQUIRE(ESP_ERR_INVALID_STATE == hcd_port_init(1, &port_config, &port_hdl));
            }
        }

        // Expectations for a successful hcd_port_deinit()
        // (usb_dwc_hal_deinit is handled by the registered callback)
        esp_intr_disable_ExpectAnyArgsAndReturn(ESP_OK);
        esp_intr_free_ExpectAnyArgsAndReturn(ESP_OK);

        // Deinit the port, so the following test cases can init it again
        REQUIRE(ESP_OK == hcd_port_deinit(port_hdl));
    }
}

SCENARIO("HCD Port deinit - invalid arguments")
{
    GIVEN("No HCD port initialized") {

        // Try to deinit with port handle set to nullptr
        SECTION("HCD port handle is NULL") {

            // Call the DUT function, expect ESP_ERR_INVALID_ARG
            REQUIRE(ESP_ERR_INVALID_ARG == hcd_port_deinit(nullptr));
        }
    }
}
