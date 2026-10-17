/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <catch2/catch_test_macros.hpp>
#include "usb_host_layer_test_fixture.hpp"

SCENARIO("USB Host client NEW_DEV event routing")
{
    GIVEN("An installed USB Host library with USBH callbacks registered") {
        usb_host_layer::UsbHostLayerFixture host(usb_host_layer::MOCK_CB_USBH);
        host.install();

        SECTION("All registered clients receive NEW_DEV") {
            // Register 2 clients
            usb_host_layer::Client clients[2];

            // Make sure no devices are registered by clients
            clients[0].drain();
            clients[1].drain();
            REQUIRE(clients[0].ctx.event_count.new_dev == 0);
            REQUIRE(clients[1].ctx.event_count.new_dev == 0);

            // Inject new device with address 9
            constexpr uint8_t kDevAddr = 9;
            inject_usbh_event::new_dev(kDevAddr);
            clients[0].drain();
            clients[1].drain();

            // Make sure both clients have registered the injected device with the desired address
            for (const auto &client : clients) {
                REQUIRE(client.ctx.event_count.new_dev == 1);
                REQUIRE(client.ctx.last_new_dev_addr == kDevAddr);
            }
        }

        SECTION("A client registered after NEW_DEV does not receive a historical event") {
            // Register early client
            usb_host_layer::Client early_client;

            // Inject new device and expect the client to get NEW_DEV event
            constexpr uint8_t kDevAddr = 10;
            inject_usbh_event::new_dev(kDevAddr);
            early_client.drain();
            REQUIRE(early_client.ctx.event_count.new_dev == 1);

            // Register late client and expect the client NOT to get NEW_DEV event
            usb_host_layer::Client late_client;
            late_client.drain();
            REQUIRE(late_client.ctx.event_count.new_dev == 0);
        }

        SECTION("Client registration rejects max_num_event_msg of zero") {
            usb_host_layer::client_event_ctx ctx;
            usb_host_client_handle_t client_hdl = nullptr;
            const usb_host_client_config_t client_config = {
                .is_synchronous = false,
                .max_num_event_msg = 0,
                .flags = {},
                .async = {
                    .client_event_callback = usb_host_layer::client_event_cb,
                    .callback_arg = &ctx,
                },
            };

            // Expect client NOT to be registered with invalid client config
            REQUIRE(ESP_ERR_INVALID_ARG == usb_host_client_register(&client_config, &client_hdl));
            REQUIRE(client_hdl == nullptr);
        }

        SECTION("Events beyond max_num_event_msg are dropped until the client drains its queue") {
            // Register client with max_num_event_msg equal to 2
            constexpr int kMaxEventMsg = 2;
            usb_host_layer::client_event_ctx ctx;
            usb_host_client_config_t client_config = usb_host_layer::default_client_config(&ctx);
            client_config.max_num_event_msg = kMaxEventMsg;
            usb_host_layer::Client client(client_config);

            // Inject 3 new devices (3 NEW_DEV events to be delivered to the client)
            constexpr uint8_t kQueuedDevAddrs[] = {20, 21, 22};
            for (uint8_t dev_addr : kQueuedDevAddrs) {
                inject_usbh_event::new_dev(dev_addr);
            }

            // Expect the client to register only 2 NEW_DEV events instead of 3 after draining it's events
            client.drain();
            REQUIRE(ctx.event_count.new_dev == kMaxEventMsg);

            // Inject one more device and drain events
            constexpr uint8_t kDeliveredAfterDrain = 23;
            inject_usbh_event::new_dev(kDeliveredAfterDrain);
            client.drain();

            // Expect the client to register 3 NEW_DEV events
            REQUIRE(ctx.event_count.new_dev == kMaxEventMsg + 1);
            REQUIRE(ctx.last_new_dev_addr == kDeliveredAfterDrain);
        }

        host.drain_events();
    }
}

SCENARIO("USB Host client disconnect event routing")
{
    GIVEN("An installed USB Host library with USBH callbacks registered") {
        usb_host_layer::UsbHostLayerFixture host(usb_host_layer::MOCK_CB_USBH);
        host.install();

        SECTION("DEV_GONE is delivered only to the client that opened the device") {
            // Register 2 clients: owner and bystander
            usb_host_layer::Client owner;
            usb_host_layer::Client bystander;

            // Add a new device and open it by the owner client
            constexpr uint8_t kDevAddr = 11;
            usb_host_layer::Device dev(owner.hdl, kDevAddr);

            // Inject USBH_EVENT_DEV_GONE event and drain both client events
            inject_usbh_event::dev_gone(kDevAddr, dev.hdl);
            owner.drain();
            bystander.drain();

            // Expect the owner client to register DEV_GONE event and bystander client not to register DEV_GONE
            REQUIRE(owner.ctx.event_count.dev_gone == 1);
            REQUIRE(owner.ctx.last_dev_gone_hdl == dev.hdl);
            REQUIRE(bystander.ctx.event_count.dev_gone == 0);
        }

        SECTION("DEV_REMOVED is delivered to subscribers that did not open the device") {
            // Register 2 clients: owner and monitor (monitor with notify_dev_removed flag)
            usb_host_layer::Client owner;
            usb_host_layer::client_event_ctx monitor_ctx;
            usb_host_client_config_t monitor_config = usb_host_layer::default_client_config(&monitor_ctx);
            monitor_config.flags.notify_dev_removed = 1;
            usb_host_layer::Client monitor(monitor_config);

            // Add a new device and open it by the owner client
            constexpr uint8_t kDevAddr = 12;
            usb_host_layer::Device dev(owner.hdl, kDevAddr);

            // Inject USBH_EVENT_DEV_REMOVED event and drain both client events
            inject_usbh_event::dev_removed(kDevAddr);
            owner.drain();
            monitor.drain();

            REQUIRE(owner.ctx.event_count.dev_gone == 0);
            REQUIRE(owner.ctx.event_count.dev_removed == 0);
            REQUIRE(monitor_ctx.event_count.dev_removed == 1);
            REQUIRE(monitor_ctx.last_dev_removed_addr == kDevAddr);
        }

        SECTION("DEV_GONE and DEV_REMOVED are routed correctly when the device disconnects") {
            // Register 2 clients: owner and monitor (monitor with notify_dev_removed flag)
            usb_host_layer::Client owner;
            usb_host_layer::client_event_ctx monitor_ctx;
            usb_host_client_config_t monitor_config = usb_host_layer::default_client_config(&monitor_ctx);
            monitor_config.flags.notify_dev_removed = 1;
            usb_host_layer::Client monitor(monitor_config);

            // Add a new device and open it by the owner client
            constexpr uint8_t kDevAddr = 13;
            usb_host_layer::Device dev(owner.hdl, kDevAddr);

            // Inject USBH_EVENT_DEV_GONE event and drain both client events
            inject_usbh_event::dev_gone(kDevAddr, dev.hdl);
            owner.drain();
            monitor.drain();

            REQUIRE(owner.ctx.event_count.dev_gone == 1);
            REQUIRE(owner.ctx.event_count.dev_removed == 0);
            REQUIRE(monitor_ctx.event_count.dev_gone == 0);
            REQUIRE(monitor_ctx.event_count.dev_removed == 1);
            REQUIRE(monitor_ctx.last_dev_removed_addr == kDevAddr);
        }

        host.drain_events();
    }
}

SCENARIO("USB Host client suspend and resume event routing")
{
    GIVEN("An installed USB Host library with USBH callbacks registered") {
        usb_host_layer::UsbHostLayerFixture host(usb_host_layer::MOCK_CB_USBH);
        host.install();

        SECTION("DEV_SUSPENDED and DEV_RESUMED are delivered only to the client that opened the device") {
            // Register 2 clients: owner and bystander
            usb_host_layer::Client owner;
            usb_host_layer::Client bystander;

            // Add a new device and open it by the owner client
            constexpr uint8_t kDevAddr = 14;
            usb_host_layer::Device dev(owner.hdl, kDevAddr);

            // Inject USBH_EVENT_DEV_SUSPEND event and drain both client events
            inject_usbh_event::dev_suspend(kDevAddr, dev.hdl);
            owner.drain();
            bystander.drain();

            // Expect the owner client to register USB_HOST_CLIENT_EVENT_DEV_SUSPENDED event and the bystander client not to
            REQUIRE(owner.ctx.event_count.dev_suspended == 1);
            REQUIRE(owner.ctx.last_suspend_resume_hdl == dev.hdl);
            REQUIRE(bystander.ctx.event_count.dev_suspended == 0);

            // Inject USBH_EVENT_DEV_RESUME event and drain both client events
            inject_usbh_event::dev_resume(kDevAddr, dev.hdl);
            owner.drain();
            bystander.drain();

            // Expect the owner client to register USB_HOST_CLIENT_EVENT_DEV_RESUMED event and the bystander client not to
            REQUIRE(owner.ctx.event_count.dev_resumed == 1);
            REQUIRE(owner.ctx.last_suspend_resume_hdl == dev.hdl);
            REQUIRE(bystander.ctx.event_count.dev_resumed == 0);
        }

        host.drain_events();
    }
}
