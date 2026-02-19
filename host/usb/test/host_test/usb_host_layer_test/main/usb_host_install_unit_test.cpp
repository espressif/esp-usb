/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdio.h>
#include <catch2/catch_test_macros.hpp>

#include "esp_bit_defs.h"
#include "usb_host.h"   // Real implementation of usb_host.h

// Test all the mocked headers defined for this mock
extern "C" {
#include "Mockusb_phy.h"
#include "Mockhcd.h"
#include "Mockusbh.h"
#include "Mockenum.h"
#include "Mockhub.h"
}

namespace {

/** Non-null opaque handle for mocked usb_phy (ReturnThruPtr). Use distinct \a id for multiple handles (e.g. dual port). id is mapped to id+1 so that 0 never becomes nullptr. */
inline usb_phy_handle_t mock_phy_handle(unsigned id = 0)
{
    return reinterpret_cast<usb_phy_handle_t>(reinterpret_cast<void *>(static_cast<uintptr_t>(id + 1)));
}

} // namespace

SCENARIO("USB Host pre-uninstall")
{
    // No USB Host driver previously installed
    GIVEN("No USB Host previously installed") {

        // Uninstall not-installed USB Host
        SECTION("Uninstalling not installed USB Host") {

            // Call the DUT function, expect ESP_ERR_INVALID_STATE
            REQUIRE(ESP_ERR_INVALID_STATE == usb_host_uninstall());
        }
    }
}

SCENARIO("USB Host install")
{
    // USB Host config is not valid, USB Host driver is not installed from previous test case
    GIVEN("No USB Host config, USB Host driver not installed") {

        // Try to install the USB Host driver with usb_host_config set to nullptr
        SECTION("USB Host config is nullptr") {

            // Call the DUT function, expect ESP_ERR_INVALID_ARG
            REQUIRE(ESP_ERR_INVALID_ARG == usb_host_install(nullptr));
        }
    }

    // USB Host config struct
    usb_host_config_t usb_host_config = {
        .skip_phy_setup = false,
        .root_port_unpowered = false,
        .intr_flags = 1,
        .enum_filter_cb = nullptr,
        .fifo_settings_custom = {},
        .peripheral_map = BIT0,
    };

    // USB host config is valid, USB Host driver is not installed from previous test case
    GIVEN("USB Host config, USB Host driver not installed") {

        // peripheral_map exceeding maximum returns ESP_ERR_INVALID_ARG (no PHY calls)
        SECTION("peripheral_map exceeds maximum returns ESP_ERR_INVALID_ARG") {

            // With SOC_USB_OTG_PERIPH_NUM=2 (mock), max valid is (1<<2)-1 = 3. 4 is invalid.
            usb_host_config_t config_invalid = usb_host_config;
            config_invalid.peripheral_map = 4U;

            REQUIRE(ESP_ERR_INVALID_ARG == usb_host_install(&config_invalid));
        }

        // Try to install the USB Host driver, with PHY install error
        SECTION("Fail to install USB Host - PHY Install error") {

            // Make the PHY install to fail
            usb_new_phy_ExpectAnyArgsAndReturn(ESP_ERR_INVALID_STATE);

            // Call the DUT function, expect ESP_ERR_INVALID_STATE
            REQUIRE(ESP_ERR_INVALID_STATE == usb_host_install(&usb_host_config));
        }

        SECTION("Fail to install dual USB Host - 2nd PHY install error") {
            usb_host_config_t config_dual = usb_host_config;
            config_dual.peripheral_map = BIT0 | BIT1;
            // Make 1st PHY install to pass
            usb_phy_handle_t phy_handle_0 = mock_phy_handle(0);
            usb_new_phy_ExpectAnyArgsAndReturn(ESP_OK);
            usb_new_phy_ReturnThruPtr_handle_ret(&phy_handle_0);

            // Make 2nd PHY install to fail
            usb_new_phy_ExpectAnyArgsAndReturn(ESP_ERR_INVALID_STATE);

            // goto phy_err: We must uninstall 1st PHY
            usb_del_phy_ExpectAndReturn(phy_handle_0, ESP_OK);

            // Call the DUT function, expect ESP_ERR_INVALID_STATE
            REQUIRE(ESP_ERR_INVALID_STATE == usb_host_install(&config_dual));
        }

        // Try to install the USB Host driver, with USBH Install error, use internal PHY
        SECTION("Fail to install USB Host - USBH install error") {

            // Make the PHY install to pass
            usb_phy_handle_t phy_handle = mock_phy_handle();
            usb_new_phy_ExpectAnyArgsAndReturn(ESP_OK);
            usb_new_phy_ReturnThruPtr_handle_ret(&phy_handle);

            // Make the USBH install to fail
            usbh_install_ExpectAnyArgsAndReturn(ESP_ERR_INVALID_STATE);

            // goto usbh_err: We must uninstall PHY
            usb_del_phy_ExpectAndReturn(phy_handle, ESP_OK);

            // Call the DUT function, expect ESP_ERR_INVALID_STATE
            REQUIRE(ESP_ERR_INVALID_STATE == usb_host_install(&usb_host_config));
        }

        // Try to install the USB Host driver, with Enum driver Install error, use internal PHY
        SECTION("Fail to install USB Host - Enum driver install error") {

            // Make the PHY install to pass
            usb_phy_handle_t phy_handle = mock_phy_handle();
            usb_new_phy_ExpectAnyArgsAndReturn(ESP_OK);
            usb_new_phy_ReturnThruPtr_handle_ret(&phy_handle);

            // Make the USBH install to pass
            usbh_install_ExpectAnyArgsAndReturn(ESP_OK);

            // Make the ENUM Driver to fail
            enum_install_ExpectAnyArgsAndReturn(ESP_ERR_INVALID_STATE);

            // goto enum_err: We must uninstall USBHand PHY
            usbh_uninstall_ExpectAndReturn(ESP_OK);
            usb_del_phy_ExpectAndReturn(phy_handle, ESP_OK);

            // Call the DUT function, expect ESP_ERR_INVALID_STATE
            REQUIRE(ESP_ERR_INVALID_STATE == usb_host_install(&usb_host_config));
        }

        // Try to install the USB Host driver, with Hub driver Install error, use internal PHY
        SECTION("Fail to install USB Host - Hub driver install error") {

            // Make the PHY install to pass
            usb_phy_handle_t phy_handle = mock_phy_handle();
            usb_new_phy_ExpectAnyArgsAndReturn(ESP_OK);
            usb_new_phy_ReturnThruPtr_handle_ret(&phy_handle);

            // Make the USBH install to pass
            usbh_install_ExpectAnyArgsAndReturn(ESP_OK);

            // Make the ENUM Driver to pass
            enum_install_ExpectAnyArgsAndReturn(ESP_OK);

            // Make the HUB Driver to fail
            hub_install_ExpectAnyArgsAndReturn(ESP_ERR_INVALID_STATE);

            // goto hub_err: We must uninstall Enum driver, USBH and PHY
            enum_uninstall_ExpectAndReturn(ESP_OK);
            usbh_uninstall_ExpectAndReturn(ESP_OK);
            usb_del_phy_ExpectAndReturn(phy_handle, ESP_OK);

            // Call the DUT function, expect ESP_ERR_INVALID_STATE
            REQUIRE(ESP_ERR_INVALID_STATE == usb_host_install(&usb_host_config));
        }
    }
}

SCENARIO("USB Dual Host install - valid config")
{
    usb_phy_handle_t phy_handle_0 = mock_phy_handle(0);
    usb_phy_handle_t phy_handle_1 = mock_phy_handle(1);
    usb_host_config_t usb_host_config = {
        .skip_phy_setup = false,
        .root_port_unpowered = false,
        .intr_flags = 1,
        .enum_filter_cb = nullptr,
        .fifo_settings_custom = {},
        .peripheral_map = BIT0 | BIT1,
    };

    usbh_install_ExpectAnyArgsAndReturn(ESP_OK);
    enum_install_ExpectAnyArgsAndReturn(ESP_OK);
    hub_install_ExpectAnyArgsAndReturn(ESP_OK);
    hub_root_start_ExpectAndReturn(ESP_OK);

    GIVEN("Dual port USB Host config with PHY") {
        // Make the PHY install to pass (2 ports)
        usb_new_phy_ExpectAnyArgsAndReturn(ESP_OK);
        usb_new_phy_ReturnThruPtr_handle_ret(&phy_handle_0);
        usb_new_phy_ExpectAnyArgsAndReturn(ESP_OK);
        usb_new_phy_ReturnThruPtr_handle_ret(&phy_handle_1);

        // Dual port install success
        SECTION("Dual port install and uninstall") {
            REQUIRE(ESP_OK == usb_host_install(&usb_host_config));
        }

        // Dual port with custom FIFO: install succeeds with default FIFO
        SECTION("Dual port with custom FIFO uses default FIFO and succeeds") {
            usb_host_config_t config_dual_fifo = usb_host_config;
            config_dual_fifo.fifo_settings_custom.rx_fifo_lines = 4;
            config_dual_fifo.fifo_settings_custom.nptx_fifo_lines = 4;
            config_dual_fifo.fifo_settings_custom.ptx_fifo_lines = 2;

            // Call the DUT function, expect ESP_OK
            REQUIRE(ESP_OK == usb_host_install(&config_dual_fifo));
        }

        // Common cleanup
        usb_del_phy_ExpectAndReturn(phy_handle_0, ESP_OK);
        usb_del_phy_ExpectAndReturn(phy_handle_1, ESP_OK);
    }

    GIVEN("Dual port USB Host config without PHY") {
        usb_host_config_t config_no_phy = usb_host_config;
        config_no_phy.skip_phy_setup = true;

        SECTION("Install with skip_phy_setup") {
            // Call the DUT function, expect ESP_OK
            REQUIRE(ESP_OK == usb_host_install(&config_no_phy));
        }
    }

    // Common cleanup
    hub_root_stop_ExpectAndReturn(ESP_OK);
    hub_uninstall_ExpectAndReturn(ESP_OK);
    enum_uninstall_ExpectAndReturn(ESP_OK);
    usbh_uninstall_ExpectAndReturn(ESP_OK);
    REQUIRE(ESP_OK == usb_host_uninstall());
}

SCENARIO("USB Host install - valid config")
{
    usb_phy_handle_t phy_handle = mock_phy_handle();
    usb_host_config_t usb_host_config = {
        .skip_phy_setup = false,
        .root_port_unpowered = false,
        .intr_flags = 1,
        .enum_filter_cb = nullptr,
        .fifo_settings_custom = {},
        .peripheral_map = BIT0,
    };

    GIVEN("Default config with PHY") {
        // Expect for usb_host_install() to pass
        usb_new_phy_ExpectAnyArgsAndReturn(ESP_OK);
        usb_new_phy_ReturnThruPtr_handle_ret(&phy_handle);
        usbh_install_ExpectAnyArgsAndReturn(ESP_OK);
        enum_install_ExpectAnyArgsAndReturn(ESP_OK);
        hub_install_ExpectAnyArgsAndReturn(ESP_OK);

        // Successfully install the USB Host driver
        SECTION("Successfully install the USB Host driver") {
            // with root_port_unpowered = false, hub_root_start() is expected to pass
            hub_root_start_ExpectAndReturn(ESP_OK);

            // Call the DUT function, expect ESP_OK
            REQUIRE(ESP_OK == usb_host_install(&usb_host_config));

            SECTION("Try to install the USB Host driver again") {
                REQUIRE(ESP_ERR_INVALID_STATE == usb_host_install(&usb_host_config));
            }
        }

        SECTION("Start with unpowered root port") {
            usb_host_config_t config_unpowered = usb_host_config;
            config_unpowered.root_port_unpowered = true;

            // Call the DUT function, expect ESP_OK
            REQUIRE(ESP_OK == usb_host_install(&config_unpowered));
        }

        // peripheral_map 0 is treated as BIT0 for backward compatibility
        SECTION("peripheral_map 0 treated as BIT0") {
            usb_host_config_t config_periph_0 = usb_host_config;
            config_periph_0.peripheral_map = 0;

            // with root_port_unpowered = false, hub_root_start() is expected to pass
            hub_root_start_ExpectAndReturn(ESP_OK);

            // Call the DUT function, expect ESP_OK
            REQUIRE(ESP_OK == usb_host_install(&config_periph_0));
        }

        usb_del_phy_ExpectAndReturn(phy_handle, ESP_OK);
    }

    GIVEN("Default config without PHY") {
        usb_host_config_t config_no_phy = usb_host_config;
        config_no_phy.skip_phy_setup = true;

        // Make the USBH install to pass
        usbh_install_ExpectAnyArgsAndReturn(ESP_OK);
        enum_install_ExpectAnyArgsAndReturn(ESP_OK);
        hub_install_ExpectAnyArgsAndReturn(ESP_OK);
        hub_root_start_ExpectAndReturn(ESP_OK);

        SECTION("Install with skip_phy_setup") {
            // Call the DUT function, expect ESP_OK
            REQUIRE(ESP_OK == usb_host_install(&config_no_phy));
        }
    }

    hub_root_stop_ExpectAndReturn(ESP_OK);
    hub_uninstall_ExpectAndReturn(ESP_OK);
    enum_uninstall_ExpectAndReturn(ESP_OK);
    usbh_uninstall_ExpectAndReturn(ESP_OK);
    REQUIRE(ESP_OK == usb_host_uninstall());
}

SCENARIO("USB Host post-uninstall")
{
    // USB Host driver successfully uninstalled from previous test case
    GIVEN("USB Host successfully uninstalled") {

        // USB Host successfully uninstalled, try to uninstall it again
        SECTION("Uninstall already uninstalled USB Host driver") {

            // Call the DUT function, expect ESP_ERR_INVALID_STATE
            REQUIRE(ESP_ERR_INVALID_STATE == usb_host_uninstall());
        }
    }
}
