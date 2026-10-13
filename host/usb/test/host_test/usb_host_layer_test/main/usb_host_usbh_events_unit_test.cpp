/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <catch2/catch_test_macros.hpp>
#include "usb_host_layer_test_fixture.hpp"

SCENARIO("USB Host USBH event routing")
{
    GIVEN("An installed USB Host library with USBH callbacks registered") {
        usb_host_layer::UsbHostLayerFixture host(usb_host_layer::MOCK_CB_USBH);
        host.install();

        SECTION("USBH_EVENT_CTRL_XFER invokes the submitting client's transfer callback") {
            // Register client
            usb_host_layer::Client client;

            // Allocate a ctrl transfer with a counting callback
            usb_host_layer::Transfer ctrl_xfer(client.hdl);

            // Inject ctrl transfer USBH event USBH_EVENT_CTRL_XFER
            usb_device_handle_t dev_hdl = usb_host_layer::mock_device_handle();
            inject_usbh_event::ctrl_xfer(dev_hdl, ctrl_xfer.urb);
            client.drain();

            // Expect the ctrl transfer callback to fire
            REQUIRE(ctrl_xfer.ctx.callback_count == 1);
        }

        SECTION("USBH_EVENT_NEW_DEV delivers NEW_DEV to registered clients") {
            // Register client
            usb_host_layer::Client client;

            // Inject USBH event USBH_EVENT_NEW_DEV
            constexpr uint8_t kDevAddr = 30;
            inject_usbh_event::new_dev(kDevAddr);
            client.drain();

            // Expect the client to register USB_HOST_CLIENT_EVENT_NEW_DEV event
            REQUIRE(client.ctx.event_count.new_dev == 1);
            REQUIRE(client.ctx.last_new_dev_addr == kDevAddr);
        }

        SECTION("USBH_EVENT_DEV_GONE delivers DEV_GONE to the device owner") {
            // Register client
            usb_host_layer::Client client;

            // Add and open a device
            constexpr uint8_t kDevAddr = 31;
            usb_host_layer::Device dev(client.hdl, kDevAddr);

            // Inject USBH event USBH_EVENT_DEV_GONE
            inject_usbh_event::dev_gone(kDevAddr, dev.hdl);
            client.drain();

            // Expect the client to register USB_HOST_CLIENT_EVENT_DEV_GONE event
            REQUIRE(client.ctx.event_count.dev_gone == 1);
            REQUIRE(client.ctx.last_dev_gone_hdl == dev.hdl);
        }

        SECTION("USBH_EVENT_DEV_REMOVED delivers DEV_REMOVED to monitoring clients") {
            usb_host_layer::client_event_ctx ctx;
            usb_host_client_config_t client_config = usb_host_layer::default_client_config(&ctx);
            client_config.flags.notify_dev_removed = 1;
            usb_host_layer::Client client(client_config);

            constexpr uint8_t kDevAddr = 35;
            inject_usbh_event::dev_removed(kDevAddr);
            client.drain();

            REQUIRE(ctx.event_count.dev_removed == 1);
            REQUIRE(ctx.last_dev_removed_addr == kDevAddr);
        }

        SECTION("USBH_EVENT_DEV_FREE recycles the hub node") {
            constexpr unsigned kDevUid = 32;
            inject_usbh_event::dev_free(kDevUid, usb_host_layer::mock_device_handle(), 1);
        }

        SECTION("USBH_EVENT_ALL_FREE sets the ALL_FREE library event flag") {
            inject_usbh_event::all_free();

            uint32_t lib_flags = host.drain_events_flags();
            REQUIRE((lib_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) != 0);
        }

        SECTION("USBH_EVENT_DEV_SUSPEND delivers DEV_SUSPENDED to the device owner") {
            usb_host_layer::Client client;

            constexpr uint8_t kDevAddr = 33;
            usb_host_layer::Device dev(client.hdl, kDevAddr);

            inject_usbh_event::dev_suspend(kDevAddr, dev.hdl);
            client.drain();

            REQUIRE(client.ctx.event_count.dev_suspended == 1);
            REQUIRE(client.ctx.last_suspend_resume_hdl == dev.hdl);
        }

        SECTION("USBH_EVENT_DEV_RESUME delivers DEV_RESUMED to the device owner") {
            usb_host_layer::Client client;

            constexpr uint8_t kDevAddr = 34;
            usb_host_layer::Device dev(client.hdl, kDevAddr);

            inject_usbh_event::dev_resume(kDevAddr, dev.hdl);
            client.drain();

            REQUIRE(client.ctx.event_count.dev_resumed == 1);
            REQUIRE(client.ctx.last_suspend_resume_hdl == dev.hdl);
        }

        SECTION("USBH_EVENT_ALL_IDLE marks the root hub as ready to suspend") {
            inject_usbh_event::all_idle();
        }

        host.drain_events();
    }
}
