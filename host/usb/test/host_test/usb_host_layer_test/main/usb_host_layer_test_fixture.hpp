/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file usb_host_layer_test_fixture.hpp
 *
 * Shared helpers for USB Host layer Linux host tests.
 *
 * Wraps usb_host_install()/usb_host_uninstall() with the CMock expectations
 * required by the mocked PHY, USBH, Enum, and Hub layers. Mock event injectors
 * only work after the corresponding layer callback is registered at install
 * time, so tests should pass only the callback flags they need:
 *
 *   - Enum event tests  -> MOCK_CB_ENUM
 *   - Hub event tests   -> MOCK_CB_HUB
 *   - USBH/client tests -> MOCK_CB_USBH
 *   - Lib-only tests    -> MOCK_CB_NONE
 *
 * Typical usage in a Catch2 GIVEN block:
 *
 *   usb_host_layer_test::UsbHostLayerFixture host(usb_host_layer_test::MOCK_CB_USBH);
 *   host.install();  // expectations derived from default_host_config()
 *   // ... test sections ...
 *   usb_host_layer_test::drain_lib_events();
 *
 * Pass a custom usb_host_config_t as the second constructor argument when testing
 * skip_phy_setup, root_port_unpowered, or multi-port peripheral_map.
 */

#pragma once

#include <stdint.h>
#include <catch2/catch_test_macros.hpp>
#include "esp_bit_defs.h"
#include "usb_host.h"
#include "mock_usb_host_layer_test_helpers.h"

extern "C" {
#include "Mockusb_phy.h"
#include "Mockusbh.h"
#include "Mockenum.h"
#include "Mockhub.h"
}

namespace usb_host_layer_test {

/** No mock layer callbacks registered at install. */
constexpr unsigned MOCK_CB_NONE = 0;
/** Register usbh_register_mock_callback / usbh_deregister_mock_callback. */
constexpr unsigned MOCK_CB_USBH = 1U << 0;
/** Register enum_register_mock_callback / enum_deregister_mock_callback. */
constexpr unsigned MOCK_CB_ENUM = 1U << 1;
/** Register hub_register_mock_callback / hub_deregister_mock_callback. */
constexpr unsigned MOCK_CB_HUB = 1U << 2;
/** All layer mock callbacks; combine individual flags with bitwise OR. */
constexpr unsigned MOCK_CB_ALL = MOCK_CB_USBH | MOCK_CB_ENUM | MOCK_CB_HUB;

/** Returns a distinct non-null PHY handle for CMock ReturnThruPtr expectations. */
inline usb_phy_handle_t mock_phy_handle(unsigned id = 0)
{
    return reinterpret_cast<usb_phy_handle_t>(reinterpret_cast<void *>(static_cast<uintptr_t>(id + 1)));
}

/** Returns a distinct non-null device handle for event injector arguments. */
inline usb_device_handle_t mock_device_handle(unsigned id = 0)
{
    return reinterpret_cast<usb_device_handle_t>(reinterpret_cast<void *>(static_cast<uintptr_t>(id + 0x100)));
}

/** Default host config used by most layer event tests (single port, BIT0). */
inline usb_host_config_t default_host_config()
{
    return {
        .skip_phy_setup = false,
        .root_port_unpowered = false,
        .intr_flags = 1,
        .enum_filter_cb = nullptr,
        .fifo_settings_custom = {},
        .peripheral_map = BIT0,
    };
}

/**
 * Set CMock expectations for usb_host_install().
 *
 * Layer install mocks are always expected; AddCallback hooks are added only for
 * flags set in @p mock_callbacks so injectors match what the test registered.
 *
 * PHY and hub_root_start expectations follow @p host_config (skip_phy_setup,
 * peripheral_map, root_port_unpowered) to mirror usb_host_install() behavior.
 */
inline void expect_host_install(unsigned mock_callbacks, const usb_host_config_t &host_config)
{
    const unsigned peripheral_map = host_config.peripheral_map == 0 ? (unsigned)BIT0 : host_config.peripheral_map;

    if (!host_config.skip_phy_setup) {
        usb_phy_handle_t phy_handles[SOC_USB_OTG_PERIPH_NUM];
        for (int i = 0; i < SOC_USB_OTG_PERIPH_NUM; i++) {
            if (!(peripheral_map & BIT(i))) {
                continue;
            }
            phy_handles[i] = mock_phy_handle(i);
            usb_new_phy_ExpectAnyArgsAndReturn(ESP_OK);
            usb_new_phy_ReturnThruPtr_handle_ret(&phy_handles[i]);
        }
    }

    usbh_install_ExpectAnyArgsAndReturn(ESP_OK);
    if (mock_callbacks & MOCK_CB_USBH) {
        usbh_install_AddCallback(usbh_register_mock_callback);
    }

    enum_install_ExpectAnyArgsAndReturn(ESP_OK);
    if (mock_callbacks & MOCK_CB_ENUM) {
        enum_install_AddCallback(enum_register_mock_callback);
    }

    hub_install_ExpectAnyArgsAndReturn(ESP_OK);
    if (mock_callbacks & MOCK_CB_HUB) {
        hub_install_AddCallback(hub_register_mock_callback);
    }

    if (!host_config.root_port_unpowered) {
        hub_root_start_ExpectAndReturn(ESP_OK);
    }
}

/** Set CMock expectations for usb_host_uninstall(); mirrors @p host_config PHY setup from install. */
inline void expect_host_uninstall(unsigned mock_callbacks, const usb_host_config_t &host_config)
{
    hub_root_stop_ExpectAndReturn(ESP_OK);

    hub_uninstall_ExpectAndReturn(ESP_OK);
    if (mock_callbacks & MOCK_CB_HUB) {
        hub_uninstall_AddCallback(hub_deregister_mock_callback);
    }

    enum_uninstall_ExpectAndReturn(ESP_OK);
    if (mock_callbacks & MOCK_CB_ENUM) {
        enum_uninstall_AddCallback(enum_deregister_mock_callback);
    }

    usbh_uninstall_ExpectAndReturn(ESP_OK);
    if (mock_callbacks & MOCK_CB_USBH) {
        usbh_uninstall_AddCallback(usbh_deregister_mock_callback);
    }

    if (!host_config.skip_phy_setup) {
        const unsigned peripheral_map = host_config.peripheral_map == 0 ? (unsigned)BIT0 : host_config.peripheral_map;
        for (int i = 0; i < SOC_USB_OTG_PERIPH_NUM; i++) {
            if (!(peripheral_map & BIT(i))) {
                continue;
            }
            usb_del_phy_ExpectAndReturn(mock_phy_handle(i), ESP_OK);
        }
    }
}

/**
 * RAII fixture for installed USB Host library state in layer event tests.
 *
 * Call install() in GIVEN; destructor uninstalls if still installed. Idempotent
 * install()/uninstall() allow explicit teardown before the GIVEN scope ends.
 */
class UsbHostLayerFixture {
public:
    explicit UsbHostLayerFixture(unsigned mock_callbacks = MOCK_CB_ALL,
                                 usb_host_config_t host_config = default_host_config())
        : mock_callbacks_(mock_callbacks)
        , host_config_(host_config)
    {}

    UsbHostLayerFixture(const UsbHostLayerFixture &) = delete;
    UsbHostLayerFixture &operator=(const UsbHostLayerFixture &) = delete;

    ~UsbHostLayerFixture()
    {
        uninstall();
    }

    void install()
    {
        if (installed_) {
            return;
        }

        expect_host_install(mock_callbacks_, host_config_);
        REQUIRE(ESP_OK == usb_host_install(&host_config_));
        installed_ = true;
    }

    void uninstall()
    {
        if (!installed_) {
            return;
        }

        expect_host_uninstall(mock_callbacks_, host_config_);
        REQUIRE(ESP_OK == usb_host_uninstall());
        installed_ = false;
    }

    unsigned mock_callbacks() const
    {
        return mock_callbacks_;
    }

    const usb_host_config_t &host_config() const
    {
        return host_config_;
    }

private:
    unsigned mock_callbacks_;
    usb_host_config_t host_config_;
    bool installed_ = false;
};

/** Drain pending library events with zero timeout; discards returned flags. */
inline void drain_lib_events(void)
{
    uint32_t lib_flags = 0;
    REQUIRE(ESP_OK == usb_host_lib_handle_events(0, &lib_flags));
}

/** Drain pending library events and return the flags set by usb_host_lib_handle_events(). */
inline uint32_t drain_lib_events_flags(void)
{
    uint32_t lib_flags = 0;
    REQUIRE(ESP_OK == usb_host_lib_handle_events(0, &lib_flags));
    return lib_flags;
}

} // namespace usb_host_layer_test
