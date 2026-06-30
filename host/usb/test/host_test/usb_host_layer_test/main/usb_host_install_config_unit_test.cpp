/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <catch2/catch_test_macros.hpp>
#include "usb_host_layer_test_fixture.hpp"
#include "usb/usb_types_ch9.h"

extern "C" {
#include "Mockenum.h"
#include "Mockhub.h"
#include "enum.h"       // For ENABLE_ENUM_FILTER_CALLBACK
}

namespace {

constexpr uint16_t kTestVendorId = 0x303a;
constexpr uint16_t kTestProductId = 0x4001;

usb_device_desc_t make_test_device_desc(uint8_t num_configurations = 2)
{
    usb_device_desc_t dev_desc = {};
    dev_desc.bLength = USB_DEVICE_DESC_SIZE;
    dev_desc.bDescriptorType = USB_B_DESCRIPTOR_TYPE_DEVICE;
    dev_desc.bcdUSB = 0x0200;
    dev_desc.bMaxPacketSize0 = 64;
    dev_desc.idVendor = kTestVendorId;
    dev_desc.idProduct = kTestProductId;
    dev_desc.bcdDevice = 0x0100;
    dev_desc.iManufacturer = 1;
    dev_desc.iProduct = 2;
    dev_desc.iSerialNumber = 3;
    dev_desc.bNumConfigurations = num_configurations;
    return dev_desc;
}

bool accept_with_configuration_two(const usb_device_desc_t *dev_desc, uint8_t *bConfigurationValue)
{
    REQUIRE(dev_desc != nullptr);
    REQUIRE(bConfigurationValue != nullptr);
    REQUIRE(dev_desc->idVendor == kTestVendorId);
    *bConfigurationValue = 2;
    return true;
}

bool reject_all_devices(const usb_device_desc_t *dev_desc, uint8_t *bConfigurationValue)
{
    (void)dev_desc;
    (void)bConfigurationValue;
    return false;
}

bool filter_by_vendor_id(const usb_device_desc_t *dev_desc, uint8_t *bConfigurationValue)
{
    REQUIRE(bConfigurationValue != nullptr);
    if (dev_desc->idVendor != kTestVendorId) {
        return false;
    }
    *bConfigurationValue = 1;
    return true;
}

} // namespace

SCENARIO("USB Host install with custom host configuration")
{
    SECTION("root_port_unpowered skips hub_root_start during install") {
        usb_host_config_t host_config = usb_host_layer_test::default_host_config();
        host_config.root_port_unpowered = true;

        usb_host_layer_test::UsbHostLayerFixture host(usb_host_layer_test::MOCK_CB_NONE, host_config);
        host.install();
        usb_host_layer_test::drain_lib_events();
    }

    SECTION("skip_phy_setup installs without creating a PHY") {
        usb_host_config_t host_config = usb_host_layer_test::default_host_config();
        host_config.skip_phy_setup = true;

        usb_host_layer_test::UsbHostLayerFixture host(usb_host_layer_test::MOCK_CB_NONE, host_config);
        host.install();
        usb_host_layer_test::drain_lib_events();
    }
}

SCENARIO("USB Host enumeration filter callback wiring")
{
    GIVEN("An installed USB Host library with an enumeration filter") {
        // Create host config with the enumeration filter enabled and pass a callback pointer to it
        usb_host_config_t host_config = usb_host_layer_test::default_host_config();
        host_config.enum_filter_cb = accept_with_configuration_two;

        // Install usb host with the enum filter enabled
        usb_host_layer_test::UsbHostLayerFixture host(usb_host_layer_test::MOCK_CB_ENUM, host_config);
        host.install();

        SECTION("usb_host_install forwards enum_filter_cb to enum_install") {
            // Check callback pointers
            REQUIRE(mock_usb_host_layer_get_enum_filter_cb() == accept_with_configuration_two);
        }

        SECTION("Installed filter selects a non-default bConfigurationValue") {
            // Create test device descriptor
            const usb_device_desc_t dev_desc = make_test_device_desc();
            uint8_t bConfigurationValue = 0;

            // Expect the configuration value 2 is selected by the enum filter
            REQUIRE(mock_usb_host_layer_get_enum_filter_cb()(&dev_desc, &bConfigurationValue));
            REQUIRE(bConfigurationValue == 2);
        }

        usb_host_layer_test::drain_lib_events();
    }

    GIVEN("An installed USB Host library with a rejecting enumeration filter") {
        // Create host config with the enumeration filter enabled and pass a callback pointer to it
        usb_host_config_t host_config = usb_host_layer_test::default_host_config();
        host_config.enum_filter_cb = reject_all_devices;

        usb_host_layer_test::UsbHostLayerFixture host(usb_host_layer_test::MOCK_CB_ENUM, host_config);
        host.install();

        SECTION("Filter callback rejects the device") {
            const usb_device_desc_t dev_desc = make_test_device_desc();
            uint8_t bConfigurationValue = 1;

            // Expect the enum filter to reject all the devices (by returning false)
            REQUIRE_FALSE(mock_usb_host_layer_get_enum_filter_cb()(&dev_desc, &bConfigurationValue));
            REQUIRE(bConfigurationValue == 1);
        }

        SECTION("Rejecting a device cancels enumeration at the enum layer") {
            constexpr unsigned kNodeUid = 42;
            const usb_device_desc_t dev_desc = make_test_device_desc();
            uint8_t bConfigurationValue = 1;

            REQUIRE_FALSE(mock_usb_host_layer_get_enum_filter_cb()(&dev_desc, &bConfigurationValue));

            enum_cancel_ExpectAndReturn(kNodeUid, ESP_OK);
            REQUIRE(ESP_OK == enum_cancel(kNodeUid));
        }

        usb_host_layer_test::drain_lib_events();
    }

    GIVEN("An installed USB Host library without an enumeration filter") {
        usb_host_config_t host_config = usb_host_layer_test::default_host_config();
        host_config.enum_filter_cb = nullptr;

        usb_host_layer_test::UsbHostLayerFixture host(usb_host_layer_test::MOCK_CB_ENUM, host_config);
        host.install();

        SECTION("enum_install receives a null filter when none is configured") {
            REQUIRE(mock_usb_host_layer_get_enum_filter_cb() == nullptr);
        }

        usb_host_layer_test::drain_lib_events();
    }

    GIVEN("An installed USB Host library with a vendor-based enumeration filter") {
        usb_host_config_t host_config = usb_host_layer_test::default_host_config();
        host_config.enum_filter_cb = filter_by_vendor_id;

        usb_host_layer_test::UsbHostLayerFixture host(usb_host_layer_test::MOCK_CB_ENUM, host_config);
        host.install();

        SECTION("Filter accepts matching vendor ID and keeps default configuration") {
            usb_device_desc_t matching_desc = make_test_device_desc();
            uint8_t matching_config = 0;

            REQUIRE(mock_usb_host_layer_get_enum_filter_cb()(&matching_desc, &matching_config));
            REQUIRE(matching_config == 1);
        }

        SECTION("Filter rejects devices with a different vendor ID") {
            usb_device_desc_t other_desc = make_test_device_desc();
            other_desc.idVendor = 0xffff;
            uint8_t other_config = 0;

            REQUIRE_FALSE(mock_usb_host_layer_get_enum_filter_cb()(&other_desc, &other_config));
        }

        usb_host_layer_test::drain_lib_events();
    }
}
