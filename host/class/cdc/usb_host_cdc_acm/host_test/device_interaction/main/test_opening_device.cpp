/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
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
        // usb_host_mock_print_mocked_devices(0);
    }

    SECTION("Fail to open CDC-ACM Device: CDC-ACM Host is not installed") {
        REQUIRE(ESP_ERR_INVALID_STATE == cdc_acm_host_open(0, 0, 0, nullptr, nullptr));
    }

    GIVEN("Mocked device is added to the device list") {
        // Install CDC-ACM driver
        REQUIRE(ESP_OK == mock_cdc_acm_host_install(nullptr));

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
        // VID, PID and interface number of the added cp120x device
        const uint16_t vid = 0x10C4, pid = 0xEA60;
        const uint8_t interface_number = 0;

        SECTION("Fail to open CDC-ACM Device: dev_config is nullptr") {
            REQUIRE(ESP_ERR_INVALID_ARG == cdc_acm_host_open(vid, pid, interface_number, nullptr, &dev));
        }

        SECTION("Fail to open CDC-ACM Device: No devices found") {
            dev_config.connection_timeout_ms = 1;   // Set small connection timeout, so the usb_host_device_addr_list_fill() is called only once
            int num_of_devices = 0;
            usb_host_device_addr_list_fill_ExpectAnyArgsAndReturn(ESP_OK);
            // Don't register callback to usb_host_device_addr_list_fill(), we want to use mocked function here
            usb_host_device_addr_list_fill_ReturnThruPtr_num_dev_ret(&num_of_devices);
            REQUIRE(ESP_ERR_NOT_FOUND == cdc_acm_host_open(vid, pid, interface_number, &dev_config, &dev));
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
                // A USB device present, but incorrect VID
                REQUIRE(ESP_ERR_NOT_FOUND == cdc_acm_host_open(0x1234, pid, interface_number, &dev_config, &dev));
            }

            SECTION("Incorrect PID") {
                // A USB device present, but incorrect VPID
                REQUIRE(ESP_ERR_NOT_FOUND == cdc_acm_host_open(vid, 0x1234, interface_number, &dev_config, &dev));
            }

            SECTION("Incorrect Interface") {
                usb_host_get_device_descriptor_ExpectAnyArgsAndReturn(ESP_OK);
                usb_host_get_active_config_descriptor_ExpectAnyArgsAndReturn(ESP_OK);
                usb_host_get_active_config_descriptor_AddCallback(usb_host_get_active_config_descriptor_mock_callback);

                // A USB device present, but incorrect interface
                REQUIRE(ESP_ERR_NOT_FOUND == cdc_acm_host_open(vid, pid, 2, &dev_config, &dev));
            }
        }

        // Uninstall CDC-ACM driver
        REQUIRE(ESP_OK == mock_cdc_acm_host_uninstall());
    }
}
