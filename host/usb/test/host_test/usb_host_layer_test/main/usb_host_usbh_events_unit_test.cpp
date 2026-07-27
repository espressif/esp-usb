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
#include "usb_private.h"
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

struct CtrlXferCtx {
    int callback_count = 0;
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

void ctrl_xfer_cb(usb_transfer_t *transfer)
{
    auto *ctx = static_cast<CtrlXferCtx *>(transfer->context);
    ctx->callback_count++;
}

usb_host_client_handle_t register_client(ClientEventCtx *ctx, bool notify_dev_removed = false)
{
    usb_host_client_handle_t client_hdl = nullptr;
    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
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

SCENARIO("USB Host USBH mock event injectors require registration")
{
    SECTION("Injectors return ESP_ERR_INVALID_STATE before install") {
        REQUIRE(ESP_ERR_INVALID_STATE == usbh_mock_event_ctrl_xfer(usb_host_layer_test::mock_device_handle(), nullptr));
        REQUIRE(ESP_ERR_INVALID_STATE == usbh_mock_event_new_dev(1));
        REQUIRE(ESP_ERR_INVALID_STATE == usbh_mock_event_dev_gone(1, usb_host_layer_test::mock_device_handle()));
        REQUIRE(ESP_ERR_INVALID_STATE == usbh_mock_event_dev_free(1, nullptr, 1));
        REQUIRE(ESP_ERR_INVALID_STATE == usbh_mock_event_all_free());
        REQUIRE(ESP_ERR_INVALID_STATE == usbh_mock_event_dev_suspend(1, usb_host_layer_test::mock_device_handle()));
        REQUIRE(ESP_ERR_INVALID_STATE == usbh_mock_event_dev_resume(1, usb_host_layer_test::mock_device_handle()));
        REQUIRE(ESP_ERR_INVALID_STATE == usbh_mock_event_all_idle());
        REQUIRE(ESP_ERR_INVALID_STATE == usbh_mock_event_dev_removed(1));
    }
}

SCENARIO("USB Host USBH event routing")
{
    GIVEN("An installed USB Host library with USBH callbacks registered") {
        usb_host_layer_test::UsbHostLayerFixture host(usb_host_layer_test::MOCK_CB_USBH);
        host.install();

        SECTION("USBH_EVENT_CTRL_XFER invokes the submitting client's transfer callback") {
            // Register client
            CtrlXferCtx ctrl_ctx;
            ClientEventCtx unused_ctx;
            usb_host_client_handle_t client_hdl = register_client(&unused_ctx);

            // Allocate and URB for a ctrl transfer
            urb_t *urb = urb_alloc(sizeof(usb_setup_packet_t), 0);
            REQUIRE(urb != nullptr);
            urb->usb_host_client = client_hdl;
            urb->transfer.callback = ctrl_xfer_cb;
            urb->transfer.context = &ctrl_ctx;

            // Inject ctrl transfer USBH event USBH_EVENT_CTRL_XFER
            REQUIRE(ESP_OK == usbh_mock_event_ctrl_xfer(usb_host_layer_test::mock_device_handle(), urb));
            drain_client_events(client_hdl);

            // Expect the ctrl transfer callback to fire
            REQUIRE(ctrl_ctx.callback_count == 1);

            urb_free(urb);
            deregister_client(client_hdl);
        }

        SECTION("USBH_EVENT_NEW_DEV delivers NEW_DEV to registered clients") {
            // Register client
            ClientEventCtx ctx;
            usb_host_client_handle_t client_hdl = register_client(&ctx);

            // Inject USBH event USBH_EVENT_NEW_DEV
            constexpr uint8_t kDevAddr = 30;
            inject_new_dev(kDevAddr);
            drain_client_events(client_hdl);

            // Expect the client to register USB_HOST_CLIENT_EVENT_NEW_DEV event
            REQUIRE(ctx.new_dev_count == 1);
            REQUIRE(ctx.last_new_dev_addr == kDevAddr);

            deregister_client(client_hdl);
        }

        SECTION("USBH_EVENT_DEV_GONE delivers DEV_GONE to the device owner") {
            // Register client
            ClientEventCtx ctx;
            usb_host_client_handle_t client_hdl = register_client(&ctx);

            // Add and open a device
            uint8_t dev_addr = 31;
            usb_device_handle_t dev_hdl = usb_host_layer_test::mock_device_handle();
            expect_device_open(dev_addr, &dev_hdl);
            REQUIRE(ESP_OK == usb_host_device_open(client_hdl, dev_addr, &dev_hdl));

            // Inject USBH event USBH_EVENT_DEV_GONE
            inject_dev_gone(dev_addr, dev_hdl);
            drain_client_events(client_hdl);

            // Expect the client to register USB_HOST_CLIENT_EVENT_DEV_GONE event
            REQUIRE(ctx.dev_gone_count == 1);
            REQUIRE(ctx.last_dev_gone_hdl == dev_hdl);

            expect_device_close(dev_hdl, &dev_addr);
            REQUIRE(ESP_OK == usb_host_device_close(client_hdl, dev_hdl));
            deregister_client(client_hdl);
        }

        SECTION("USBH_EVENT_DEV_REMOVED delivers DEV_REMOVED to monitoring clients") {
            ClientEventCtx ctx;
            usb_host_client_handle_t client_hdl = register_client(&ctx, true);

            constexpr uint8_t kDevAddr = 35;
            inject_dev_removed(kDevAddr);
            drain_client_events(client_hdl);

            REQUIRE(ctx.dev_removed_count == 1);
            REQUIRE(ctx.last_dev_removed_addr == kDevAddr);

            deregister_client(client_hdl);
        }

        SECTION("USBH_EVENT_DEV_FREE recycles the hub node") {
            constexpr unsigned kDevUid = 32;
            hub_node_recycle_ExpectAndReturn(kDevUid, ESP_OK);
            REQUIRE(ESP_OK == usbh_mock_event_dev_free(kDevUid, usb_host_layer_test::mock_device_handle(), 1));
        }

        SECTION("USBH_EVENT_ALL_FREE sets the ALL_FREE library event flag") {
            REQUIRE(ESP_OK == usbh_mock_event_all_free());

            uint32_t lib_flags = usb_host_layer_test::drain_lib_events_flags();
            REQUIRE((lib_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) != 0);
        }

        SECTION("USBH_EVENT_DEV_SUSPEND delivers DEV_SUSPENDED to the device owner") {
            ClientEventCtx ctx;
            usb_host_client_handle_t client_hdl = register_client(&ctx);

            uint8_t dev_addr = 33;
            usb_device_handle_t dev_hdl = usb_host_layer_test::mock_device_handle(1);
            expect_device_open(dev_addr, &dev_hdl);
            REQUIRE(ESP_OK == usb_host_device_open(client_hdl, dev_addr, &dev_hdl));

            inject_dev_suspend(dev_addr, dev_hdl);
            drain_client_events(client_hdl);

            REQUIRE(ctx.dev_suspended_count == 1);
            REQUIRE(ctx.last_suspend_resume_hdl == dev_hdl);

            expect_device_close(dev_hdl, &dev_addr);
            REQUIRE(ESP_OK == usb_host_device_close(client_hdl, dev_hdl));
            deregister_client(client_hdl);
        }

        SECTION("USBH_EVENT_DEV_RESUME delivers DEV_RESUMED to the device owner") {
            ClientEventCtx ctx;
            usb_host_client_handle_t client_hdl = register_client(&ctx);

            uint8_t dev_addr = 34;
            usb_device_handle_t dev_hdl = usb_host_layer_test::mock_device_handle(2);
            expect_device_open(dev_addr, &dev_hdl);
            REQUIRE(ESP_OK == usb_host_device_open(client_hdl, dev_addr, &dev_hdl));

            inject_dev_resume(dev_addr, dev_hdl);
            drain_client_events(client_hdl);

            REQUIRE(ctx.dev_resumed_count == 1);
            REQUIRE(ctx.last_suspend_resume_hdl == dev_hdl);

            expect_device_close(dev_hdl, &dev_addr);
            REQUIRE(ESP_OK == usb_host_device_close(client_hdl, dev_hdl));
            deregister_client(client_hdl);
        }

        SECTION("USBH_EVENT_ALL_IDLE marks the root hub as ready to suspend") {
            hub_root_mark_suspend_ExpectAndReturn(ESP_OK);
            REQUIRE(ESP_OK == usbh_mock_event_all_idle());
        }

        usb_host_layer_test::drain_lib_events();
    }
}
