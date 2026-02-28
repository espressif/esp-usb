/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <catch2/catch_test_macros.hpp>

#include "descriptors/cdc_descriptors.hpp"
#include "usb/cdc_acm_host.h"
#include "mock_add_usb_device.h"
#include "common_test_fixtures.hpp"

extern "C" {
#include "Mockusb_host.h"
}

SCENARIO("Test mocked device opening and closing")
{
    usb_host_mock_dev_list_init();
    SECTION("Fail to open CDC-ACM Device: CDC-ACM Host is not installed") {
        REQUIRE(ESP_ERR_INVALID_STATE == cdc_acm_host_open(0, 0, 0, nullptr, nullptr));
    }

    GIVEN("CDC driver is installed") {
        // Install CDC-ACM driver
        REQUIRE(ESP_OK == test_cdc_acm_host_install(nullptr));

        // Setup mock fixtures
        usb_host_device_addr_list_fill_Stub(usb_host_device_addr_list_fill_mock_callback);
        usb_host_device_open_Stub(usb_host_device_open_mock_callback);
        usb_host_get_device_descriptor_Stub(usb_host_get_device_descriptor_mock_callback);
        usb_host_device_close_Stub(usb_host_device_close_mock_callback);
        usb_host_get_active_config_descriptor_Stub(usb_host_get_active_config_descriptor_mock_callback);
        usb_host_transfer_alloc_Stub(usb_host_transfer_alloc_mock_callback);
        usb_host_transfer_free_Stub(usb_host_transfer_free_mock_callback);

        cdc_acm_dev_hdl_t dev = nullptr;
        // Define input parameters for cdc_acm_host_open
        // VID, PID, interface index and IN EP of the added cp120x device
        const uint16_t vid = 0x10C4, pid = 0xEA60;
        const uint8_t interface_index = 0;
        const uint8_t in_ep = 0x82;
        cdc_acm_host_device_config_t dev_config = {
            .connection_timeout_ms = 1,   // Set small connection timeout, so the usb_host_device_addr_list_fill() is called only once
            .out_buffer_size = 100,
            .in_buffer_size = 100,
            .event_cb = nullptr,
            .data_cb = nullptr,
            .user_arg = nullptr,
        };

        SECTION("Fail to open CDC-ACM Device: dev_config is nullptr") {
            REQUIRE(ESP_ERR_INVALID_ARG == cdc_acm_host_open(vid, pid, interface_index, nullptr, &dev));
        }

        SECTION("Fail to open CDC-ACM Device: No devices found") {
            REQUIRE(ESP_ERR_NOT_FOUND == cdc_acm_host_open(vid, pid, interface_index, &dev_config, &dev));
        }

        AND_GIVEN("Mocked device is added to the device list") {
            REQUIRE(ESP_OK == usb_host_mock_add_device(5, (const usb_device_desc_t *)cp210x_device_desc,
                                                       (const usb_config_desc_t *)cp210x_config_desc));

            SECTION("Incorrect VID") {
                // USB device present, but incorrect VID is given
                const uint16_t incorrect_vid = 0x1234;
                REQUIRE(ESP_ERR_NOT_FOUND == cdc_acm_host_open(incorrect_vid, pid, interface_index, &dev_config, &dev));
            }

            SECTION("Incorrect PID") {
                // USB device present, but incorrect PID is given
                const uint16_t incorrect_pid = 0x1234;
                REQUIRE(ESP_ERR_NOT_FOUND == cdc_acm_host_open(vid, incorrect_pid, interface_index, &dev_config, &dev));
            }

            SECTION("Incorrect Interface parsed") {
                // USB device present, but incorrect interface is given
                const uint8_t incorrect_interface_index = 2;
                REQUIRE(ESP_ERR_NOT_FOUND == cdc_acm_host_open(vid, pid, incorrect_interface_index, &dev_config, &dev));
            }

            SECTION("Incorrect VID or PID with any PID or VID") {
                REQUIRE(ESP_ERR_NOT_FOUND == cdc_acm_host_open(0x1234, CDC_HOST_ANY_PID, interface_index, &dev_config, &dev));
                REQUIRE(ESP_ERR_NOT_FOUND == cdc_acm_host_open(CDC_HOST_ANY_VID, 0x1234, interface_index, &dev_config, &dev));
            }

            SECTION("Successfully open and close the device") {
                // Call cdc_acm_start
                // Claim data interface, make sure that the interface_idx has been claimed
                test_usb_host_interface_claim(interface_index);

                SECTION("Specific VID and specific PID") {
                    REQUIRE(ESP_OK == cdc_acm_host_open(vid, pid, interface_index, &dev_config, &dev));
                    REQUIRE(nullptr != dev);
                }

                SECTION("Any VID and any PID") {
                    REQUIRE(ESP_OK == cdc_acm_host_open(CDC_HOST_ANY_VID, CDC_HOST_ANY_PID, interface_index, &dev_config, &dev));
                    REQUIRE(nullptr != dev);
                }

                SECTION("Any VID and specific PID") {
                    REQUIRE(ESP_OK == cdc_acm_host_open(CDC_HOST_ANY_VID, pid, interface_index, &dev_config, &dev));
                    REQUIRE(nullptr != dev);
                }

                SECTION("Specific VID and any PID") {
                    REQUIRE(ESP_OK == cdc_acm_host_open(vid, CDC_HOST_ANY_PID, interface_index, &dev_config, &dev));
                    REQUIRE(nullptr != dev);
                }

                // Device is opened, now close it
                // Cancel pooling of IN endpoint -> halt, flush, clear
                test_cdc_acm_reset_transfer_endpoint(in_ep);

                // Release data interface
                usb_host_interface_release_ExpectAndReturn(nullptr, nullptr, interface_index, ESP_OK);
                usb_host_interface_release_IgnoreArg_client_hdl();  // Ignore all function parameters, except interface_idx
                usb_host_interface_release_IgnoreArg_dev_hdl();

                // Call the real function cdc_acm_host_close
                REQUIRE(ESP_OK == cdc_acm_host_close(dev));
            }
        }

        // Uninstall CDC-ACM driver
        REQUIRE(ESP_OK == test_cdc_acm_host_uninstall());
    }
}
