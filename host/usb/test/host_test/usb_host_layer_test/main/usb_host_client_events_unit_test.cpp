/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <catch2/catch_test_macros.hpp>
#include "usb_host_layer_test_fixture.hpp"

extern "C" {
#include "Mockusbh.h"
#include "Mockhub.h"
}

namespace {

struct ClientEventCtx {
    int new_dev_count = 0;
    int dev_gone_count = 0;
    int dev_removed_count = 0;
    int dev_suspended_count = 0;
    int dev_resumed_count = 0;
    uint8_t last_new_dev_addr = 0;
    usb_device_handle_t last_dev_gone_hdl = nullptr;
    uint8_t last_dev_removed_addr = 0;
    usb_device_handle_t last_suspend_resume_hdl = nullptr;
};

void drain_client_events(usb_host_client_handle_t client_hdl)
{
    REQUIRE(ESP_OK == usb_host_client_handle_events(client_hdl, 0));
}

void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    auto *ctx = static_cast<ClientEventCtx *>(arg);

    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        ctx->new_dev_count++;
        ctx->last_new_dev_addr = event_msg->new_dev.address;
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        ctx->dev_gone_count++;
        ctx->last_dev_gone_hdl = event_msg->dev_gone.dev_hdl;
        break;
    case USB_HOST_CLIENT_EVENT_DEV_REMOVED:
        ctx->dev_removed_count++;
        ctx->last_dev_removed_addr = event_msg->dev_removed.address;
        break;
    case USB_HOST_CLIENT_EVENT_DEV_SUSPENDED:
        ctx->dev_suspended_count++;
        ctx->last_suspend_resume_hdl = event_msg->dev_suspend_resume.dev_hdl;
        break;
    case USB_HOST_CLIENT_EVENT_DEV_RESUMED:
        ctx->dev_resumed_count++;
        ctx->last_suspend_resume_hdl = event_msg->dev_suspend_resume.dev_hdl;
        break;
    default:
        break;
    }
}

usb_host_client_handle_t register_client(ClientEventCtx *ctx, bool notify_dev_removed = false, int max_num_event_msg = 5)
{
    usb_host_client_handle_t client_hdl = nullptr;
    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = max_num_event_msg,
        .flags = {
            .notify_dev_removed = notify_dev_removed ? 1U : 0U,
            .reserved31 = 0,
        },
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = ctx,
        },
    };

    REQUIRE(ESP_OK == usb_host_client_register(&client_config, &client_hdl));
    return client_hdl;
}

void deregister_client(usb_host_client_handle_t client_hdl)
{
    REQUIRE(ESP_OK == usb_host_client_deregister(client_hdl));
}

void inject_new_dev(uint8_t dev_addr)
{
    hub_dev_new_ExpectAndReturn(dev_addr, ESP_ERR_NOT_SUPPORTED);
    REQUIRE(ESP_OK == usbh_mock_event_new_dev(dev_addr));
}

void inject_dev_gone(uint8_t dev_addr, usb_device_handle_t dev_hdl)
{
    hub_dev_gone_ExpectAndReturn(dev_addr, ESP_ERR_NOT_SUPPORTED);
    REQUIRE(ESP_OK == usbh_mock_event_dev_gone(dev_addr, dev_hdl));
}

void inject_dev_removed(uint8_t dev_addr)
{
    REQUIRE(ESP_OK == usbh_mock_event_dev_removed(dev_addr));
}

void inject_dev_suspend(uint8_t dev_addr, usb_device_handle_t dev_hdl)
{
    REQUIRE(ESP_OK == usbh_mock_event_dev_suspend(dev_addr, dev_hdl));
}

void inject_dev_resume(uint8_t dev_addr, usb_device_handle_t dev_hdl)
{
    REQUIRE(ESP_OK == usbh_mock_event_dev_resume(dev_addr, dev_hdl));
}

void expect_device_open(uint8_t dev_addr, usb_device_handle_t *dev_hdl_storage)
{
    usbh_devs_open_ExpectAndReturn(dev_addr, dev_hdl_storage, ESP_OK);
    usbh_devs_open_IgnoreArg_dev_hdl();
    usbh_devs_open_ReturnThruPtr_dev_hdl(dev_hdl_storage);
}

void expect_device_close(usb_device_handle_t dev_hdl, uint8_t *dev_addr_storage)
{
    usbh_dev_get_addr_ExpectAndReturn(dev_hdl, dev_addr_storage, ESP_OK);
    usbh_dev_get_addr_IgnoreArg_dev_addr();
    usbh_dev_get_addr_ReturnThruPtr_dev_addr(dev_addr_storage);
    usbh_dev_close_ExpectAndReturn(dev_hdl, ESP_OK);
}

} // namespace

SCENARIO("USB Host client NEW_DEV event routing")
{
    GIVEN("An installed USB Host library with USBH callbacks registered") {
        usb_host_layer_test::UsbHostLayerFixture host(usb_host_layer_test::MOCK_CB_USBH);
        host.install();

        SECTION("All registered clients receive NEW_DEV") {
            // Register 2 clients
            ClientEventCtx ctx[2] = {};
            usb_host_client_handle_t client_hdl[2] = {
                register_client(&ctx[0]),
                register_client(&ctx[1]),
            };
            REQUIRE(client_hdl[0] != nullptr);
            REQUIRE(client_hdl[1] != nullptr);

            // Make sure no devices are registered by clients
            drain_client_events(client_hdl[0]);
            drain_client_events(client_hdl[1]);
            REQUIRE(ctx[0].new_dev_count == 0);
            REQUIRE(ctx[1].new_dev_count == 0);

            // Inject new device with address 9
            constexpr uint8_t kDevAddr = 9;
            inject_new_dev(kDevAddr);
            drain_client_events(client_hdl[0]);
            drain_client_events(client_hdl[1]);

            // Make sure both clients have registered the injected device with the desired address
            for (const auto &this_client_ctx : ctx) {
                REQUIRE(this_client_ctx.new_dev_count == 1);
                REQUIRE(this_client_ctx.last_new_dev_addr == kDevAddr);
            }

            // Deregister clients
            deregister_client(client_hdl[0]);
            deregister_client(client_hdl[1]);
        }

        SECTION("A client registered after NEW_DEV does not receive a historical event") {
            // Register early client
            ClientEventCtx early_ctx;
            usb_host_client_handle_t early_client = register_client(&early_ctx);
            REQUIRE(early_client != nullptr);

            // Inject new device and expect the client to get NEW_DEV event
            constexpr uint8_t kDevAddr = 10;
            inject_new_dev(kDevAddr);
            drain_client_events(early_client);
            REQUIRE(early_ctx.new_dev_count == 1);

            // Register late client and expect the client NOT to get NEW_DEV event
            ClientEventCtx late_ctx;
            usb_host_client_handle_t late_client = register_client(&late_ctx);
            REQUIRE(late_client != nullptr);
            drain_client_events(late_client);
            REQUIRE(late_ctx.new_dev_count == 0);

            // Deregister both clients
            deregister_client(early_client);
            deregister_client(late_client);
        }

        SECTION("Client registration rejects max_num_event_msg of zero") {
            ClientEventCtx ctx;
            usb_host_client_handle_t client_hdl = nullptr;
            const usb_host_client_config_t client_config = {
                .is_synchronous = false,
                .max_num_event_msg = 0,
                .flags = {},
                .async = {
                    .client_event_callback = client_event_cb,
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
            ClientEventCtx ctx;
            usb_host_client_handle_t client_hdl = register_client(&ctx, false, kMaxEventMsg);
            REQUIRE(client_hdl != nullptr);

            // Inject 3 new devices (3 NEW_DEV events to be delivered to the client)
            constexpr uint8_t kQueuedDevAddrs[] = {20, 21, 22};
            for (uint8_t dev_addr : kQueuedDevAddrs) {
                inject_new_dev(dev_addr);
            }

            // Expect the client to register only 2 NEW_DEV events instead of 3 after draining it's events
            drain_client_events(client_hdl);
            REQUIRE(ctx.new_dev_count == kMaxEventMsg);

            // Inject one more device and drain events
            constexpr uint8_t kDeliveredAfterDrain = 23;
            inject_new_dev(kDeliveredAfterDrain);
            drain_client_events(client_hdl);

            // Expect the client to register 3 NEW_DEV events
            REQUIRE(ctx.new_dev_count == kMaxEventMsg + 1);
            REQUIRE(ctx.last_new_dev_addr == kDeliveredAfterDrain);

            deregister_client(client_hdl);
        }

        usb_host_layer_test::drain_lib_events();
    }
}

SCENARIO("USB Host client disconnect event routing")
{
    GIVEN("An installed USB Host library with USBH callbacks registered") {
        usb_host_layer_test::UsbHostLayerFixture host(usb_host_layer_test::MOCK_CB_USBH);
        host.install();

        SECTION("DEV_GONE is delivered only to the client that opened the device") {
            // Register 2 clients: owner and bystander
            ClientEventCtx owner_ctx;
            ClientEventCtx bystander_ctx;
            usb_host_client_handle_t owner_client = register_client(&owner_ctx);
            usb_host_client_handle_t bystander_client = register_client(&bystander_ctx);
            REQUIRE(owner_client != nullptr);
            REQUIRE(bystander_client != nullptr);

            // Add a new device and open it by the owner_client
            uint8_t dev_addr = 11;
            usb_device_handle_t dev_hdl = usb_host_layer_test::mock_device_handle();
            expect_device_open(dev_addr, &dev_hdl);
            REQUIRE(ESP_OK == usb_host_device_open(owner_client, dev_addr, &dev_hdl));

            // Inject USBH_EVENT_DEV_GONE event and drain both client events
            inject_dev_gone(dev_addr, dev_hdl);
            drain_client_events(owner_client);
            drain_client_events(bystander_client);

            // Expect the owner_client to register DEV_GONE event and bystander_client not to register DEV_GONE
            REQUIRE(owner_ctx.dev_gone_count == 1);
            REQUIRE(owner_ctx.last_dev_gone_hdl == dev_hdl);
            REQUIRE(bystander_ctx.dev_gone_count == 0);

            // Close the device
            expect_device_close(dev_hdl, &dev_addr);
            REQUIRE(ESP_OK == usb_host_device_close(owner_client, dev_hdl));
            deregister_client(owner_client);
            deregister_client(bystander_client);
        }

        SECTION("DEV_REMOVED is delivered to subscribers that did not open the device") {
            // Register 2 clients: owner and monitor (monitor with notify_dev_removed flag)
            ClientEventCtx owner_ctx;
            ClientEventCtx monitor_ctx;
            usb_host_client_handle_t owner_client = register_client(&owner_ctx);
            usb_host_client_handle_t monitor_client = register_client(&monitor_ctx, true);
            REQUIRE(owner_client != nullptr);
            REQUIRE(monitor_client != nullptr);

            // Add a new device and open it by the owner_client
            uint8_t dev_addr = 12;
            usb_device_handle_t dev_hdl = usb_host_layer_test::mock_device_handle(1);
            expect_device_open(dev_addr, &dev_hdl);
            REQUIRE(ESP_OK == usb_host_device_open(owner_client, dev_addr, &dev_hdl));

            // Inject USBH_EVENT_DEV_REMOVED event and drain both client events
            inject_dev_removed(dev_addr);
            drain_client_events(owner_client);
            drain_client_events(monitor_client);

            REQUIRE(owner_ctx.dev_gone_count == 0);
            REQUIRE(owner_ctx.dev_removed_count == 0);
            REQUIRE(monitor_ctx.dev_removed_count == 1);
            REQUIRE(monitor_ctx.last_dev_removed_addr == dev_addr);

            // Close the device
            expect_device_close(dev_hdl, &dev_addr);
            REQUIRE(ESP_OK == usb_host_device_close(owner_client, dev_hdl));
            deregister_client(owner_client);
            deregister_client(monitor_client);
        }

        SECTION("DEV_GONE and DEV_REMOVED are routed correctly when the device disconnects") {
            // Register 2 clients: owner and monitor (monitor with notify_dev_removed flag)
            ClientEventCtx owner_ctx;
            ClientEventCtx monitor_ctx;
            usb_host_client_handle_t owner_client = register_client(&owner_ctx);
            usb_host_client_handle_t monitor_client = register_client(&monitor_ctx, true);

            // Add a new device and open it by the owner_client
            uint8_t dev_addr = 13;
            usb_device_handle_t dev_hdl = usb_host_layer_test::mock_device_handle(2);
            expect_device_open(dev_addr, &dev_hdl);
            REQUIRE(ESP_OK == usb_host_device_open(owner_client, dev_addr, &dev_hdl));

            // Inject USBH_EVENT_DEV_GONE event and drain both client events
            inject_dev_gone(dev_addr, dev_hdl);
            drain_client_events(owner_client);
            drain_client_events(monitor_client);

            REQUIRE(owner_ctx.dev_gone_count == 1);
            REQUIRE(owner_ctx.dev_removed_count == 0);
            REQUIRE(monitor_ctx.dev_gone_count == 0);
            REQUIRE(monitor_ctx.dev_removed_count == 1);
            REQUIRE(monitor_ctx.last_dev_removed_addr == dev_addr);

            // Close the device
            expect_device_close(dev_hdl, &dev_addr);
            REQUIRE(ESP_OK == usb_host_device_close(owner_client, dev_hdl));
            deregister_client(owner_client);
            deregister_client(monitor_client);
        }

        usb_host_layer_test::drain_lib_events();
    }
}

SCENARIO("USB Host client suspend and resume event routing")
{
    GIVEN("An installed USB Host library with USBH callbacks registered") {
        usb_host_layer_test::UsbHostLayerFixture host(usb_host_layer_test::MOCK_CB_USBH);
        host.install();

        SECTION("DEV_SUSPENDED and DEV_RESUMED are delivered only to the client that opened the device") {
            // Register 2 clients: owner and bystander
            ClientEventCtx owner_ctx;
            ClientEventCtx bystander_ctx;
            usb_host_client_handle_t owner_client = register_client(&owner_ctx);
            usb_host_client_handle_t bystander_client = register_client(&bystander_ctx);

            // Add a new device and open it by the owner_client
            uint8_t dev_addr = 14;
            usb_device_handle_t dev_hdl = usb_host_layer_test::mock_device_handle(3);
            expect_device_open(dev_addr, &dev_hdl);
            REQUIRE(ESP_OK == usb_host_device_open(owner_client, dev_addr, &dev_hdl));

            // Inject USBH_EVENT_DEV_SUSPEND event and drain both client events
            inject_dev_suspend(dev_addr, dev_hdl);
            drain_client_events(owner_client);
            drain_client_events(bystander_client);

            // Expect the owner client to register USB_HOST_CLIENT_EVENT_DEV_SUSPENDED event and the bystander client not to
            REQUIRE(owner_ctx.dev_suspended_count == 1);
            REQUIRE(owner_ctx.last_suspend_resume_hdl == dev_hdl);
            REQUIRE(bystander_ctx.dev_suspended_count == 0);

            // Inject USBH_EVENT_DEV_RESUME event and drain both client events
            inject_dev_resume(dev_addr, dev_hdl);
            drain_client_events(owner_client);
            drain_client_events(bystander_client);

            // Expect the owner client to register USB_HOST_CLIENT_EVENT_DEV_RESUMED event and the bystander client not to
            REQUIRE(owner_ctx.dev_resumed_count == 1);
            REQUIRE(owner_ctx.last_suspend_resume_hdl == dev_hdl);
            REQUIRE(bystander_ctx.dev_resumed_count == 0);

            // Close the device
            expect_device_close(dev_hdl, &dev_addr);
            REQUIRE(ESP_OK == usb_host_device_close(owner_client, dev_hdl));
            deregister_client(owner_client);
            deregister_client(bystander_client);
        }

        usb_host_layer_test::drain_lib_events();
    }
}
