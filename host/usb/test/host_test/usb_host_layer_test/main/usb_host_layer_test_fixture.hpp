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
 *   usb_host_layer::UsbHostLayerFixture host(usb_host_layer::MOCK_CB_USBH);
 *   host.install();  // expectations derived from default_host_config()
 *   // ... test sections ...
 *   host.drain_events();
 *
 * Pass a custom usb_host_config_t as the second constructor argument when testing
 * skip_phy_setup, root_port_unpowered, or multi-port peripheral_map.
 *
 * Namespace layout: constants -> types -> callbacks -> factories -> classes
 * (each preceded by helpers used only by that class). All event handling is
 * drained through the owning class: Client::drain(), UsbHostLayerFixture::drain_events().
 */

#pragma once

#include <stdint.h>
#include <catch2/catch_test_macros.hpp>
#include "esp_bit_defs.h"
#include "usb_host.h"
#include "usb_private.h"
#include "mock_usb_host_layer_test_helpers.h"

extern "C" {
#include "Mockusb_phy.h"
#include "Mockusbh.h"
#include "Mockenum.h"
#include "Mockhub.h"
}

#include "usbh_event_injectors.hpp"

namespace usb_host_layer {

// ---------------------------------------------- Constants ----------------------------------------------

constexpr unsigned MOCK_CB_NONE = 0;            /** No mock layer callbacks registered at install. */
constexpr unsigned MOCK_CB_USBH = 1U << 0;      /** Register usbh_register_mock_callback / usbh_deregister_mock_callback. */
constexpr unsigned MOCK_CB_ENUM = 1U << 1;      /** Register enum_register_mock_callback / enum_deregister_mock_callback. */
constexpr unsigned MOCK_CB_HUB = 1U << 2;       /** Register hub_register_mock_callback / hub_deregister_mock_callback. */
/** All layer mock callbacks; combine individual flags with bitwise OR. */
constexpr unsigned MOCK_CB_ALL = MOCK_CB_USBH | MOCK_CB_ENUM | MOCK_CB_HUB;

// ------------------------------------------------ Types ------------------------------------------------

/** Records client events delivered to client_event_cb(); passed as the callback arg. */
struct client_event_ctx {
    struct {
        int new_dev = 0;            //** USB_HOST_CLIENT_EVENT_NEW_DEV event count */
        int dev_gone = 0;           //** USB_HOST_CLIENT_EVENT_DEV_GONE event count */
        int dev_removed = 0;        //** USB_HOST_CLIENT_EVENT_DEV_REMOVED event count */
        int dev_suspended = 0;      //** USB_HOST_CLIENT_EVENT_DEV_SUSPENDED event count */
        int dev_resumed = 0;        //** USB_HOST_CLIENT_EVENT_DEV_RESUMED event count */
    } event_count;                  //** Client events count */
    uint8_t last_new_dev_addr = 0;
    uint8_t last_dev_removed_addr = 0;
    usb_device_handle_t last_dev_gone_hdl = nullptr;
    usb_device_handle_t last_suspend_resume_hdl = nullptr;
};

/** Records transfer callback invocations; passed as the transfer context. */
struct transfer_ctx {
    int callback_count = 0;
};

// ----------------------------------------------- Callbacks ----------------------------------------------

/** Client event callback that counts delivered events into the client_event_ctx callback arg. */
inline void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    auto *ctx = static_cast<client_event_ctx *>(arg);

    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        ctx->event_count.new_dev++;
        ctx->last_new_dev_addr = event_msg->new_dev.address;
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        ctx->event_count.dev_gone++;
        ctx->last_dev_gone_hdl = event_msg->dev_gone.dev_hdl;
        break;
    case USB_HOST_CLIENT_EVENT_DEV_REMOVED:
        ctx->event_count.dev_removed++;
        ctx->last_dev_removed_addr = event_msg->dev_removed.address;
        break;
    case USB_HOST_CLIENT_EVENT_DEV_SUSPENDED:
        ctx->event_count.dev_suspended++;
        ctx->last_suspend_resume_hdl = event_msg->dev_suspend_resume.dev_hdl;
        break;
    case USB_HOST_CLIENT_EVENT_DEV_RESUMED:
        ctx->event_count.dev_resumed++;
        ctx->last_suspend_resume_hdl = event_msg->dev_suspend_resume.dev_hdl;
        break;
    default:
        break;
    }
}

/** Transfer callback that counts invocations into the transfer_ctx context. */
inline void transfer_cb(usb_transfer_t *transfer)
{
    auto *ctx = static_cast<transfer_ctx *>(transfer->context);
    ctx->callback_count++;
}

// ----------------------------------------------- Factories ----------------------------------------------

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

/**
 * Default host config used by most layer event tests.
 */
inline usb_host_config_t default_host_config()
{
    return {
        .skip_phy_setup = false,
        .root_port_unpowered = false,
        .intr_flags = 1,
        .enum_filter_cb = nullptr,
        .fifo_settings_custom = {},
        .peripheral_map = BIT0,
        .fsls_only = false,
    };
}

/**
 * Default async client config used by most layer event tests.
 */
inline usb_host_client_config_t default_client_config(client_event_ctx *ctx)
{
    return {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .flags = {},
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = ctx,
        },
    };
}

// ------------------------------------------------ Classes ------------------------------------------------

/**
 * RAII wrapper for a USB transfer allocated with usb_host_transfer_alloc().
 *
 * Wires the counting transfer_cb() with an owned transfer_ctx by default,
 * derives the private URB for mock event injectors and sets the submitting
 * client required by USBH_EVENT_CTRL_XFER routing. The transfer is freed in
 * the destructor, also when a REQUIRE aborts the test section.
 *
 * Copy and move are deleted: xfer->context aliases the owned ctx member, so
 * relocating the object would leave a dangling context pointer.
 */
class Transfer {
public:
    explicit Transfer(usb_host_client_handle_t client_hdl,
                      size_t data_buffer_size = sizeof(usb_setup_packet_t),
                      int num_isoc_packets = 0,
                      usb_transfer_cb_t cb = transfer_cb,
                      void *context = nullptr)
    {
        REQUIRE(ESP_OK == usb_host_transfer_alloc(data_buffer_size, num_isoc_packets, &xfer));
        REQUIRE(xfer != nullptr);

        urb = __containerof(xfer, urb_t, transfer);
        urb->usb_host_client = client_hdl;

        xfer->callback = cb;
        xfer->context = context ? context : &ctx;
    }

    ~Transfer()
    {
        usb_host_transfer_free(xfer);
    }

    Transfer(const Transfer &) = delete;
    Transfer &operator=(const Transfer &) = delete;

    transfer_ctx ctx;                /** Owned counting ctx; aliased by xfer->context unless overridden. */
    usb_transfer_t *xfer = nullptr;  /** Public transfer view. */
    urb_t *urb = nullptr;            /** Private URB view for mock event injectors. */
};

/**
 * RAII wrapper for a registered USB Host client.
 *
 * Registers the client on construction and deregisters it in the destructor,
 * also when a REQUIRE aborts the test section. The default constructor uses
 * default_client_config() bound to an owned client_event_ctx; pass a custom
 * usb_host_client_config_t to override.
 *
 * deregister() is idempotent: call it explicitly when the deregistration is
 * part of the tested behavior; the destructor skips already deregistered
 * clients.
 *
 * Copy and move are deleted: the default config's callback_arg aliases the
 * owned ctx member, so relocating the object would leave a dangling pointer.
 */
class Client {
public:
    /** Register a client with the default config counting events into the owned ctx */
    Client()
    {
        const usb_host_client_config_t client_config = default_client_config(&ctx);
        REQUIRE(ESP_OK == usb_host_client_register(&client_config, &hdl));
        REQUIRE(hdl != nullptr);
    }

    /** Register a client with a custom config */
    explicit Client(const usb_host_client_config_t &client_config)
    {
        REQUIRE(ESP_OK == usb_host_client_register(&client_config, &hdl));
        REQUIRE(hdl != nullptr);
    }

    ~Client()
    {
        deregister();
    }

    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    /** Deregister the client; no-op when already deregistered. */
    void deregister()
    {
        if (!hdl) {
            return;
        }
        REQUIRE(ESP_OK == usb_host_client_deregister(hdl));
        hdl = nullptr;
    }

    /** Drain pending client events with zero timeout. */
    void drain()
    {
        REQUIRE(ESP_OK == usb_host_client_handle_events(hdl, 0));
    }

    client_event_ctx ctx;                   /** Owned event ctx; bound to the default config's callback_arg. */
    usb_host_client_handle_t hdl = nullptr; /** Registered client handle; nullptr after deregister(). */
};

/**
 * RAII wrapper for a device opened with usb_host_device_open().
 *
 * Sets the CMock expectations and opens the device on construction; closes it
 * in the destructor, also when a REQUIRE aborts the test section. The device
 * handle is a distinct mock value (see mock_device_handle()), returned through
 * the usbh_devs_open() expectations.
 *
 * Declare after the owning Client so the device is closed before the client
 * deregisters.
 *
 * close() is idempotent: call it explicitly when closing the device is part of
 * the tested behavior; the destructor skips already closed devices.
 */
class Device {
public:
    /** Open device @p dev_addr by client @p client_hdl with a mock device handle */
    Device(usb_host_client_handle_t client_hdl, uint8_t dev_addr, unsigned mock_hdl_id = 0)
        : client_hdl(client_hdl)
        , addr(dev_addr)
        , hdl(mock_device_handle(mock_hdl_id))
    {
        usbh_devs_open_ExpectAndReturn(dev_addr, &hdl, ESP_OK);
        usbh_devs_open_IgnoreArg_dev_hdl();
        usbh_devs_open_ReturnThruPtr_dev_hdl(&hdl);
        REQUIRE(ESP_OK == usb_host_device_open(client_hdl, dev_addr, &hdl));
    }

    ~Device()
    {
        close();
    }

    Device(const Device &) = delete;
    Device &operator=(const Device &) = delete;

    /** Close the device; no-op when already closed. */
    void close()
    {
        if (!hdl) {
            return;
        }
        usbh_dev_get_addr_ExpectAndReturn(hdl, &addr, ESP_OK);
        usbh_dev_get_addr_IgnoreArg_dev_addr();
        usbh_dev_get_addr_ReturnThruPtr_dev_addr(&addr);
        usbh_dev_close_ExpectAndReturn(hdl, ESP_OK);
        REQUIRE(ESP_OK == usb_host_device_close(client_hdl, hdl));
        hdl = nullptr;
    }

    usb_host_client_handle_t client_hdl; /** Handle of the client that opened the device. */
    uint8_t addr;                        /** Device address. */
    usb_device_handle_t hdl = nullptr;   /** Opened device handle; nullptr after close(). */
};

// -------------------------------- UsbHostLayerFixture private helpers -----------------------------------

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

    /** Install USB Host */
    void install()
    {
        if (installed_) {
            return;
        }

        // Set CMOCK expectations
        expect_host_install(mock_callbacks_, host_config_);
        // Call the real DUT function
        REQUIRE(ESP_OK == usb_host_install(&host_config_));
        installed_ = true;
    }

    /** Uninstall USB Host */
    void uninstall()
    {
        if (!installed_) {
            return;
        }

        expect_host_uninstall(mock_callbacks_, host_config_);
        REQUIRE(ESP_OK == usb_host_uninstall());
        installed_ = false;
    }

    /** Drain pending library events with zero timeout; discards returned flags. */
    void drain_events() const
    {
        uint32_t lib_flags = 0;
        REQUIRE(ESP_OK == usb_host_lib_handle_events(0, &lib_flags));
    }

    /** Drain pending library events and return the flags set by usb_host_lib_handle_events(). */
    uint32_t drain_events_flags() const
    {
        uint32_t lib_flags = 0;
        REQUIRE(ESP_OK == usb_host_lib_handle_events(0, &lib_flags));
        return lib_flags;
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

} // namespace usb_host_layer
