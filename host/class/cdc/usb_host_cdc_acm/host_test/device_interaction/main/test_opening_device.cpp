/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
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

/**
 * @brief Add mocked devices
 *
 * Mocked devices are defined by a device descriptor, a configuration descriptor and a device address
 */
static void _add_mocked_devices(void)
{
    // Init mocked devices list at the beginning of the test
    usb_host_mock_dev_list_init();

    // Fill mocked devices list
    // CP210x (FS descriptor)
    REQUIRE(ESP_OK == usb_host_mock_add_device(5, (const usb_device_desc_t *)cp210x_device_desc,
                                               (const usb_config_desc_t *)cp210x_config_desc));
}


SCENARIO("Test mocked device opening and closing")
{
    // We will put device adding to the SECTION, to run it just once, not repeatedly for all the following SECTIONs
    // (if multiple sections are present)
    SECTION("Add mocked device") {
        _add_mocked_devices();

        // Optionally, print all the devices
        // usb_host_mock_print_mocked_devices(0xFF);
    }

    SECTION("Fail to open CDC-ACM Device: CDC-ACM Host is not installed") {
        REQUIRE(ESP_ERR_INVALID_STATE == cdc_acm_host_open(0, 0, 0, nullptr, nullptr));
    }

    GIVEN("Mocked device is added to the device list") {
        // Install CDC-ACM driver
        REQUIRE(ESP_OK == test_cdc_acm_host_install(nullptr));

        cdc_acm_dev_hdl_t dev = nullptr;
        cdc_acm_host_device_config_t dev_config = {
            .connection_timeout_ms = 1000,
            .out_buffer_size = 100,
            .in_buffer_size = 100,
            .event_cb = nullptr,
            .data_cb = nullptr,
            .user_arg = nullptr,
        };

        // Define input parameters for cdc_acm_host_open
        // VID, PID, interface index and IN EP of the added cp120x device
        const uint16_t vid = 0x10C4, pid = 0xEA60;
        const uint8_t interface_index = 0;
        const uint8_t in_ep = 0x82;

        SECTION("Fail to open CDC-ACM Device: dev_config is nullptr") {
            REQUIRE(ESP_ERR_INVALID_ARG == cdc_acm_host_open(vid, pid, interface_index, nullptr, &dev));
        }

        SECTION("Fail to open CDC-ACM Device: No devices found") {
            dev_config.connection_timeout_ms = 1;   // Set small connection timeout, so the usb_host_device_addr_list_fill() is called only once
            int num_of_devices = 0;
            usb_host_device_addr_list_fill_ExpectAnyArgsAndReturn(ESP_OK);
            // Unregister callback to usb_host_device_addr_list_fill(), we want to use mocked function here
            usb_host_device_addr_list_fill_Stub(nullptr);
            usb_host_device_addr_list_fill_ReturnThruPtr_num_dev_ret(&num_of_devices);
            REQUIRE(ESP_ERR_NOT_FOUND == cdc_acm_host_open(vid, pid, interface_index, &dev_config, &dev));
        }

        AND_GIVEN("Fail to open CDC-ACM Device: No device with specified PID, VID, interface found") {
            dev_config.connection_timeout_ms = 1;   // Set small connection timeout, so the usb_host_device_addr_list_fill() is called only once

            // Fill address list with callback function
            usb_host_device_addr_list_fill_ExpectAnyArgsAndReturn(ESP_OK);
            usb_host_device_addr_list_fill_AddCallback(usb_host_device_addr_list_fill_mock_callback);

            // Open device with callback function
            usb_host_device_open_ExpectAnyArgsAndReturn(ESP_OK);
            usb_host_device_open_AddCallback(usb_host_device_open_mock_callback);

            usb_host_get_device_descriptor_ExpectAnyArgsAndReturn(ESP_OK);
            usb_host_get_device_descriptor_AddCallback(usb_host_get_device_descriptor_mock_callback);

            usb_host_device_close_ExpectAnyArgsAndReturn(ESP_OK);
            usb_host_device_close_AddCallback(usb_host_device_close_mock_callback);

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
                usb_host_get_device_descriptor_ExpectAnyArgsAndReturn(ESP_OK);
                usb_host_get_active_config_descriptor_ExpectAnyArgsAndReturn(ESP_OK);
                usb_host_get_active_config_descriptor_AddCallback(usb_host_get_active_config_descriptor_mock_callback);

                REQUIRE(ESP_ERR_NOT_FOUND == cdc_acm_host_open(vid, pid, incorrect_interface_index, &dev_config, &dev));
            }
        }

        SECTION("Successfully open and close the device") {
            // We must open and close a device in one section, otherwise the test_cdc_acm_host_uninstall() would fail

            // Fill address list with callback function
            usb_host_device_addr_list_fill_ExpectAnyArgsAndReturn(ESP_OK);
            usb_host_device_addr_list_fill_AddCallback(usb_host_device_addr_list_fill_mock_callback);

            // Open device with callback function
            usb_host_device_open_ExpectAnyArgsAndReturn(ESP_OK);
            usb_host_device_open_AddCallback(usb_host_device_open_mock_callback);

            usb_host_get_device_descriptor_ExpectAnyArgsAndReturn(ESP_OK);
            usb_host_get_device_descriptor_AddCallback(usb_host_get_device_descriptor_mock_callback);

            usb_host_device_close_ExpectAnyArgsAndReturn(ESP_OK);
            usb_host_device_close_AddCallback(usb_host_device_close_mock_callback);

            // We have found the device by specified PID VID
            // Get Device and Configuration descriptors of the correct device
            usb_host_get_device_descriptor_ExpectAnyArgsAndReturn(ESP_OK);
            usb_host_get_active_config_descriptor_ExpectAnyArgsAndReturn(ESP_OK);
            usb_host_get_active_config_descriptor_AddCallback(usb_host_get_active_config_descriptor_mock_callback);

            // Setup control transfer
            usb_host_transfer_alloc_ExpectAnyArgsAndReturn(ESP_OK);
            //  Setup IN data transfer
            usb_host_transfer_alloc_ExpectAnyArgsAndReturn(ESP_OK);
            // Setup OUT bulk transfer
            usb_host_transfer_alloc_ExpectAnyArgsAndReturn(ESP_OK);
            // Register callback
            usb_host_transfer_alloc_AddCallback(usb_host_transfer_alloc_mock_callback);

            // Call cdc_acm_start

            // Claim data interface, make sure that the interface_idx has been claimed
            test_usb_host_interface_claim(interface_index);

            // Call the real function cdc_acm_host_open
            // Expect ESP_OK and dev_hdl non nullptr
            REQUIRE(ESP_OK == cdc_acm_host_open(vid, pid, interface_index, &dev_config, &dev));
            REQUIRE(nullptr != dev);

            // Device is opened, now close it

            // Cancel pooling of IN endpoint -> halt, flush, clear
            test_cdc_acm_reset_transfer_endpoint(in_ep);

            // Release data interface
            usb_host_interface_release_ExpectAndReturn(nullptr, nullptr, interface_index, ESP_OK);
            usb_host_interface_release_IgnoreArg_client_hdl();  // Ignore all function parameters, except interface_idx
            usb_host_interface_release_IgnoreArg_dev_hdl();

            // Free in transfer
            usb_host_transfer_free_ExpectAnyArgsAndReturn(ESP_OK);
            // Free out transfer
            usb_host_transfer_free_ExpectAnyArgsAndReturn(ESP_OK);

            // Call cdc_acm_device_remove, free ctrl transfer
            usb_host_transfer_free_ExpectAnyArgsAndReturn(ESP_OK);
            usb_host_transfer_free_AddCallback(usb_host_transfer_free_mock_callback);

            usb_host_device_close_ExpectAnyArgsAndReturn(ESP_OK);

            // Call the real function cdc_acm_host_close
            REQUIRE(ESP_OK == cdc_acm_host_close(dev));
        }

        // Uninstall CDC-ACM driver
        REQUIRE(ESP_OK == test_cdc_acm_host_uninstall());
    }
}
