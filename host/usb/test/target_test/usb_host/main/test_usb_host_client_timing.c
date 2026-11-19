/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "usb/usb_host.h"
#include "unity.h"

static const char *TAG = "USB Host timing";

#define TEST_CLIENT_TIMING_TIMEOUT_MS       2000
#define TEST_CLIENT_TIMING_NO_EVENT_MS      250
#define TEST_CLIENT_TIMING_POLL_MS          10
#define TEST_CLIENT_TIMING_MAX_CLIENTS      3

typedef struct {
    uint8_t last_new_dev_addr;
    int new_dev_count;
    int dev_gone_count;
    int dev_removed_count;
} client_timing_ctx_t;

static void client_timing_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    client_timing_ctx_t *ctx = (client_timing_ctx_t *)arg;

    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        ESP_LOGI(TAG, "Client event -> New device, address: %d", event_msg->new_dev.address);
        ctx->last_new_dev_addr = event_msg->new_dev.address;
        ctx->new_dev_count++;
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        ESP_LOGI(TAG, "Client event -> Device gone");
        ctx->dev_gone_count++;
        break;
    case USB_HOST_CLIENT_EVENT_DEV_REMOVED:
        ESP_LOGI(TAG, "Client event -> Device removed, address: %d", event_msg->dev_removed.address);
        ctx->dev_removed_count++;
        break;
    default:
        break;
    }
}

static void handle_client_events(usb_host_client_handle_t *client_hdl, int client_count)
{
    for (int i = 0; i < client_count; i++) {
        if (client_hdl[i]) {
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_client_handle_events(client_hdl[i], 0));
        }
    }
}

static void wait_for_device_count(int expected_count)
{
    TickType_t timeout_ticks = pdMS_TO_TICKS(10000);
    TimeOut_t timeout;
    vTaskSetTimeOutState(&timeout);

    while (1) {
        uint32_t event_flags = 0;
        usb_host_lib_handle_events(pdMS_TO_TICKS(TEST_CLIENT_TIMING_POLL_MS), &event_flags);

        usb_host_lib_info_t lib_info;
        TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_info(&lib_info));
        if (lib_info.num_devices == expected_count) {
            return;
        }

        if (xTaskCheckForTimeOut(&timeout, &timeout_ticks) == pdTRUE) {
            TEST_FAIL_MESSAGE("Timed out waiting for expected USB device count");
        }
    }
}

static void wait_for_all_devices_free(void)
{
    TickType_t timeout_ticks = pdMS_TO_TICKS(TEST_CLIENT_TIMING_TIMEOUT_MS);
    TimeOut_t timeout;
    vTaskSetTimeOutState(&timeout);

    while (1) {
        uint32_t event_flags = 0;
        usb_host_lib_handle_events(pdMS_TO_TICKS(TEST_CLIENT_TIMING_POLL_MS), &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            return;
        }

        if (xTaskCheckForTimeOut(&timeout, &timeout_ticks) == pdTRUE) {
            TEST_FAIL_MESSAGE("Timed out waiting for all USB devices to be freed");
        }
    }
}

static void wait_for_new_dev_events(usb_host_client_handle_t *client_hdl, client_timing_ctx_t *ctx, int client_count, int expected_count)
{
    TickType_t timeout_ticks = pdMS_TO_TICKS(TEST_CLIENT_TIMING_TIMEOUT_MS);
    TimeOut_t timeout;
    vTaskSetTimeOutState(&timeout);

    while (1) {
        bool all_seen = true;
        usb_host_lib_handle_events(pdMS_TO_TICKS(TEST_CLIENT_TIMING_POLL_MS), NULL);
        handle_client_events(client_hdl, client_count);

        for (int i = 0; i < client_count; i++) {
            if (ctx[i].new_dev_count != expected_count) {
                all_seen = false;
                break;
            }
        }

        if (all_seen) {
            return;
        }

        if (xTaskCheckForTimeOut(&timeout, &timeout_ticks) == pdTRUE) {
            TEST_FAIL_MESSAGE("Timed out waiting for NEW_DEV events");
        }
    }
}

static void assert_new_dev_count(const client_timing_ctx_t *ctx, int client_count, int expected_count)
{
    for (int i = 0; i < client_count; i++) {
        TEST_ASSERT_EQUAL(expected_count, ctx[i].new_dev_count);
    }
}

static void assert_disconnect_count(const client_timing_ctx_t *ctx, int client_count, const int *expected_dev_gone, const int *expected_dev_removed)
{
    for (int i = 0; i < client_count; i++) {
        TEST_ASSERT_EQUAL(expected_dev_gone[i], ctx[i].dev_gone_count);
        TEST_ASSERT_EQUAL(expected_dev_removed[i], ctx[i].dev_removed_count);
    }
}

static void drain_events_for(usb_host_client_handle_t *client_hdl, int client_count, int duration_ms)
{
    TickType_t timeout_ticks = pdMS_TO_TICKS(duration_ms);
    TimeOut_t timeout;
    vTaskSetTimeOutState(&timeout);

    do {
        usb_host_lib_handle_events(pdMS_TO_TICKS(TEST_CLIENT_TIMING_POLL_MS), NULL);
        handle_client_events(client_hdl, client_count);
        vTaskDelay(pdMS_TO_TICKS(TEST_CLIENT_TIMING_POLL_MS));
    } while (xTaskCheckForTimeOut(&timeout, &timeout_ticks) == pdFALSE);
}

static void wait_for_existing_device_ready(void)
{
    /*
     * num_devices == 1 can become true before the pending USBH_EVENT_NEW_DEV is dispatched by the host lib.
     * Keep running the host lib with no clients so late-client tests do not accidentally catch that real event.
     */
    wait_for_device_count(1);
    drain_events_for(NULL, 0, TEST_CLIENT_TIMING_NO_EVENT_MS);
}

static void register_timing_client(client_timing_ctx_t *ctx, usb_host_client_handle_t *client_hdl)
{
    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = client_timing_cb,
            .callback_arg = ctx,
        },
    };

    TEST_ASSERT_EQUAL(ESP_OK, usb_host_client_register(&client_config, client_hdl));
}

static void register_timing_removed_notify_client(client_timing_ctx_t *ctx, usb_host_client_handle_t *client_hdl)
{
    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .flags = {
            .notify_dev_removed = 1,
        },
        .async = {
            .client_event_callback = client_timing_cb,
            .callback_arg = ctx,
        },
    };

    TEST_ASSERT_EQUAL(ESP_OK, usb_host_client_register(&client_config, client_hdl));
}

static void power_off_root_and_wait_empty(void)
{
    /*
     * The test setup powers the root port immediately. Wait for the already attached test device first,
     * then power the root port off and wait for the host stack to recycle the device object.
     */
    wait_for_device_count(1);
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_root_port_power(false));
    wait_for_all_devices_free();
}

static void power_on_root_and_wait_one_device(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_root_port_power(true));
    wait_for_device_count(1);
}

static void power_off_root_only(void)
{
    wait_for_device_count(1);
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_root_port_power(false));
}

static void cleanup_timing_test(usb_host_client_handle_t *client_hdl, int client_count)
{
    for (int i = 0; i < client_count; i++) {
        if (client_hdl[i]) {
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_client_deregister(client_hdl[i]));
            client_hdl[i] = NULL;
        }
    }

    drain_events_for(NULL, 0, TEST_CLIENT_TIMING_NO_EVENT_MS);

    esp_err_t ret = usb_host_device_free_all();
    TEST_ASSERT_TRUE(ret == ESP_OK || ret == ESP_ERR_NOT_FINISHED);
    if (ret == ESP_ERR_NOT_FINISHED) {
        wait_for_all_devices_free();
    }
}

static void wait_for_client_disconnect_events(usb_host_client_handle_t *client_hdl, client_timing_ctx_t *ctx, int client_count,
                                              const int *expected_dev_gone, const int *expected_dev_removed)
{
    TickType_t timeout_ticks = pdMS_TO_TICKS(TEST_CLIENT_TIMING_TIMEOUT_MS);
    TimeOut_t timeout;
    vTaskSetTimeOutState(&timeout);

    while (1) {
        bool all_seen = true;
        usb_host_lib_handle_events(pdMS_TO_TICKS(TEST_CLIENT_TIMING_POLL_MS), NULL);
        handle_client_events(client_hdl, client_count);

        for (int i = 0; i < client_count; i++) {
            if (ctx[i].dev_gone_count != expected_dev_gone[i] || ctx[i].dev_removed_count != expected_dev_removed[i]) {
                all_seen = false;
                break;
            }
        }

        if (all_seen) {
            return;
        }

        if (xTaskCheckForTimeOut(&timeout, &timeout_ticks) == pdTRUE) {
            TEST_FAIL_MESSAGE("Timed out waiting for disconnect events");
        }
    }
}

TEST_CASE("USB Host clients registered before insertion receive NEW_DEV", "[usb_host][timing][low_speed][full_speed][high_speed]")
{
    client_timing_ctx_t ctx[2] = {};
    usb_host_client_handle_t client_hdl[2] = {};

    power_off_root_and_wait_empty();
    register_timing_client(&ctx[0], &client_hdl[0]);
    register_timing_client(&ctx[1], &client_hdl[1]);

    power_on_root_and_wait_one_device();
    wait_for_new_dev_events(client_hdl, ctx, 2, 1);
    drain_events_for(client_hdl, 2, TEST_CLIENT_TIMING_NO_EVENT_MS);

    assert_new_dev_count(ctx, 2, 1);
    TEST_ASSERT_NOT_EQUAL(0, ctx[0].last_new_dev_addr);
    TEST_ASSERT_EQUAL(ctx[0].last_new_dev_addr, ctx[1].last_new_dev_addr);

    cleanup_timing_test(client_hdl, 2);
}

TEST_CASE("USB Host clients registered after enumeration receive existing NEW_DEV by default", "[usb_host][timing][low_speed][full_speed][high_speed]")
{
    client_timing_ctx_t ctx[2] = {};
    usb_host_client_handle_t client_hdl[2] = {};

    wait_for_existing_device_ready();
    register_timing_client(&ctx[0], &client_hdl[0]);
    register_timing_client(&ctx[1], &client_hdl[1]);
    wait_for_new_dev_events(client_hdl, ctx, 2, 1);
    drain_events_for(client_hdl, 2, TEST_CLIENT_TIMING_NO_EVENT_MS);

    const int client0_new_dev_count = ctx[0].new_dev_count;
    const int client1_new_dev_count = ctx[1].new_dev_count;

    cleanup_timing_test(client_hdl, 2);

    TEST_ASSERT_EQUAL(1, client0_new_dev_count);
    TEST_ASSERT_EQUAL(1, client1_new_dev_count);
}

TEST_CASE("USB Host mixed early and late clients preserve per-client NEW_DEV timing", "[usb_host][timing][low_speed][full_speed][high_speed]")
{
    client_timing_ctx_t ctx[2] = {};
    usb_host_client_handle_t client_hdl[2] = {};

    power_off_root_and_wait_empty();
    register_timing_client(&ctx[0], &client_hdl[0]);
    power_on_root_and_wait_one_device();
    wait_for_new_dev_events(client_hdl, ctx, 1, 1);

    register_timing_client(&ctx[1], &client_hdl[1]);
    wait_for_new_dev_events(&client_hdl[1], &ctx[1], 1, 1);
    drain_events_for(client_hdl, 2, TEST_CLIENT_TIMING_NO_EVENT_MS);

    const int early_client_new_dev_count = ctx[0].new_dev_count;
    const int late_client_new_dev_count = ctx[1].new_dev_count;

    cleanup_timing_test(client_hdl, 2);

    TEST_ASSERT_EQUAL(1, early_client_new_dev_count);
    TEST_ASSERT_EQUAL(1, late_client_new_dev_count);
}

TEST_CASE("USB Host late clients receive NEW_DEV for later reconnect", "[usb_host][timing][low_speed][full_speed][high_speed]")
{
    client_timing_ctx_t ctx[2] = {};
    usb_host_client_handle_t client_hdl[2] = {};

    wait_for_existing_device_ready();
    register_timing_client(&ctx[0], &client_hdl[0]);
    register_timing_client(&ctx[1], &client_hdl[1]);
    wait_for_new_dev_events(client_hdl, ctx, 2, 1);
    drain_events_for(client_hdl, 2, TEST_CLIENT_TIMING_NO_EVENT_MS);

    const int client0_historical_count = ctx[0].new_dev_count;
    const int client1_historical_count = ctx[1].new_dev_count;

    power_off_root_and_wait_empty();
    power_on_root_and_wait_one_device();
    wait_for_new_dev_events(client_hdl, ctx, 2, 2);
    drain_events_for(client_hdl, 2, TEST_CLIENT_TIMING_NO_EVENT_MS);

    TEST_ASSERT_NOT_EQUAL(0, ctx[0].last_new_dev_addr);
    TEST_ASSERT_EQUAL(ctx[0].last_new_dev_addr, ctx[1].last_new_dev_addr);

    cleanup_timing_test(client_hdl, 2);

    TEST_ASSERT_EQUAL(1, client0_historical_count);
    TEST_ASSERT_EQUAL(1, client1_historical_count);
}

TEST_CASE("USB Host multiple late clients receive one existing NEW_DEV by default", "[usb_host][timing][low_speed][full_speed][high_speed]")
{
    client_timing_ctx_t ctx[TEST_CLIENT_TIMING_MAX_CLIENTS] = {};
    usb_host_client_handle_t client_hdl[TEST_CLIENT_TIMING_MAX_CLIENTS] = {};
    int new_dev_count[TEST_CLIENT_TIMING_MAX_CLIENTS] = {};

    wait_for_existing_device_ready();
    for (int i = 0; i < TEST_CLIENT_TIMING_MAX_CLIENTS; i++) {
        register_timing_client(&ctx[i], &client_hdl[i]);
        wait_for_new_dev_events(&client_hdl[i], &ctx[i], 1, 1);
        drain_events_for(&client_hdl[i], 1, TEST_CLIENT_TIMING_NO_EVENT_MS);
        new_dev_count[i] = ctx[i].new_dev_count;
    }

    cleanup_timing_test(client_hdl, TEST_CLIENT_TIMING_MAX_CLIENTS);

    for (int i = 0; i < TEST_CLIENT_TIMING_MAX_CLIENTS; i++) {
        TEST_ASSERT_EQUAL(1, new_dev_count[i]);
    }
}

TEST_CASE("USB Host DEV_GONE is delivered only to clients that opened the device", "[usb_host][timing][low_speed][full_speed][high_speed]")
{
    client_timing_ctx_t ctx[3] = {};
    usb_host_client_handle_t client_hdl[3] = {};
    usb_device_handle_t opened_dev[2] = {};

    power_off_root_and_wait_empty();
    register_timing_client(&ctx[0], &client_hdl[0]);
    register_timing_client(&ctx[1], &client_hdl[1]);
    register_timing_client(&ctx[2], &client_hdl[2]);

    power_on_root_and_wait_one_device();
    wait_for_new_dev_events(client_hdl, ctx, 3, 1);
    drain_events_for(client_hdl, 3, TEST_CLIENT_TIMING_NO_EVENT_MS);
    assert_new_dev_count(ctx, 3, 1);

    TEST_ASSERT_EQUAL(ESP_OK, usb_host_device_open(client_hdl[0], ctx[0].last_new_dev_addr, &opened_dev[0]));
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_device_open(client_hdl[1], ctx[1].last_new_dev_addr, &opened_dev[1]));

    power_off_root_only();

    const int expected_dev_gone[3] = {1, 1, 0};
    const int expected_dev_removed[3] = {0, 0, 0};
    wait_for_client_disconnect_events(client_hdl, ctx, 3, expected_dev_gone, expected_dev_removed);
    drain_events_for(client_hdl, 3, TEST_CLIENT_TIMING_NO_EVENT_MS);

    const int final_dev_gone_count[3] = {ctx[0].dev_gone_count, ctx[1].dev_gone_count, ctx[2].dev_gone_count};
    const int final_dev_removed_count[3] = {ctx[0].dev_removed_count, ctx[1].dev_removed_count, ctx[2].dev_removed_count};

    TEST_ASSERT_EQUAL(ESP_OK, usb_host_device_close(client_hdl[0], opened_dev[0]));
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_device_close(client_hdl[1], opened_dev[1]));
    cleanup_timing_test(client_hdl, 3);

    assert_disconnect_count((const client_timing_ctx_t[]) {
        {.dev_gone_count = final_dev_gone_count[0], .dev_removed_count = final_dev_removed_count[0]},
        {.dev_gone_count = final_dev_gone_count[1], .dev_removed_count = final_dev_removed_count[1]},
        {.dev_gone_count = final_dev_gone_count[2], .dev_removed_count = final_dev_removed_count[2]},
    }, 3, expected_dev_gone, expected_dev_removed);
}

TEST_CASE("USB Host DEV_REMOVED is delivered to unopened clients that subscribe", "[usb_host][timing][low_speed][full_speed][high_speed]")
{
    client_timing_ctx_t ctx[2] = {};
    usb_host_client_handle_t client_hdl[2] = {};
    usb_device_handle_t opened_dev = NULL;

    power_off_root_and_wait_empty();
    register_timing_client(&ctx[0], &client_hdl[0]);
    register_timing_removed_notify_client(&ctx[1], &client_hdl[1]);

    power_on_root_and_wait_one_device();
    wait_for_new_dev_events(client_hdl, ctx, 2, 1);
    drain_events_for(client_hdl, 2, TEST_CLIENT_TIMING_NO_EVENT_MS);
    assert_new_dev_count(ctx, 2, 1);

    TEST_ASSERT_EQUAL(ESP_OK, usb_host_device_open(client_hdl[0], ctx[0].last_new_dev_addr, &opened_dev));

    power_off_root_only();

    const int expected_dev_gone[2] = {1, 0};
    const int expected_dev_removed[2] = {0, 1};
    wait_for_client_disconnect_events(client_hdl, ctx, 2, expected_dev_gone, expected_dev_removed);
    drain_events_for(client_hdl, 2, TEST_CLIENT_TIMING_NO_EVENT_MS);

    const int final_dev_gone_count[2] = {ctx[0].dev_gone_count, ctx[1].dev_gone_count};
    const int final_dev_removed_count[2] = {ctx[0].dev_removed_count, ctx[1].dev_removed_count};

    TEST_ASSERT_EQUAL(ESP_OK, usb_host_device_close(client_hdl[0], opened_dev));
    cleanup_timing_test(client_hdl, 2);

    assert_disconnect_count((const client_timing_ctx_t[]) {
        {.dev_gone_count = final_dev_gone_count[0], .dev_removed_count = final_dev_removed_count[0]},
        {.dev_gone_count = final_dev_gone_count[1], .dev_removed_count = final_dev_removed_count[1]},
    }, 2, expected_dev_gone, expected_dev_removed);
}

TEST_CASE("USB Host late client receives existing device as NEW_DEV", "[usb_host][timing][low_speed][full_speed][high_speed]")
{
    client_timing_ctx_t ctx = {};
    usb_host_client_handle_t client_hdl = NULL;

    wait_for_existing_device_ready();
    register_timing_client(&ctx, &client_hdl);
    wait_for_new_dev_events(&client_hdl, &ctx, 1, 1);
    drain_events_for(&client_hdl, 1, TEST_CLIENT_TIMING_NO_EVENT_MS);

    TEST_ASSERT_EQUAL(1, ctx.new_dev_count);
    TEST_ASSERT_NOT_EQUAL(0, ctx.last_new_dev_addr);

    cleanup_timing_test(&client_hdl, 1);
}

TEST_CASE("USB Host late client receives one NEW_DEV across pending enumeration boundary", "[usb_host][timing][low_speed][full_speed][high_speed]")
{
    client_timing_ctx_t ctx = {};
    usb_host_client_handle_t client_hdl = NULL;

    power_off_root_and_wait_empty();
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_root_port_power(true));
    wait_for_device_count(1);

    register_timing_client(&ctx, &client_hdl);
    wait_for_new_dev_events(&client_hdl, &ctx, 1, 1);
    drain_events_for(&client_hdl, 1, TEST_CLIENT_TIMING_NO_EVENT_MS);

    const int new_dev_count = ctx.new_dev_count;

    cleanup_timing_test(&client_hdl, 1);

    TEST_ASSERT_EQUAL(1, new_dev_count);
}

TEST_CASE("USB Host client does not receive NEW_DEV after all devices are freed", "[usb_host][timing][low_speed][full_speed][high_speed]")
{
    client_timing_ctx_t ctx = {};
    usb_host_client_handle_t client_hdl = NULL;

    wait_for_existing_device_ready();
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, usb_host_device_free_all());
    wait_for_all_devices_free();

    register_timing_client(&ctx, &client_hdl);
    drain_events_for(&client_hdl, 1, TEST_CLIENT_TIMING_NO_EVENT_MS);

    const int new_dev_count = ctx.new_dev_count;

    cleanup_timing_test(&client_hdl, 1);

    TEST_ASSERT_EQUAL(0, new_dev_count);
}

TEST_CASE("USB Host client registration tolerates device removal in flight", "[usb_host][timing][low_speed][full_speed][high_speed]")
{
    client_timing_ctx_t ctx = {};
    usb_host_client_handle_t client_hdl = NULL;

    wait_for_existing_device_ready();
    power_off_root_only();

    register_timing_client(&ctx, &client_hdl);
    drain_events_for(&client_hdl, 1, TEST_CLIENT_TIMING_NO_EVENT_MS);

    TEST_ASSERT_TRUE(ctx.new_dev_count <= 1);
    if (ctx.new_dev_count == 1) {
        TEST_ASSERT_NOT_EQUAL(0, ctx.last_new_dev_addr);
    }

    cleanup_timing_test(&client_hdl, 1);
}

TEST_CASE("USB Host late clients and early clients share consistent NEW_DEV visibility", "[usb_host][timing][low_speed][full_speed][high_speed]")
{
    client_timing_ctx_t ctx[3] = {};
    usb_host_client_handle_t client_hdl[3] = {};

    power_off_root_and_wait_empty();
    register_timing_client(&ctx[0], &client_hdl[0]);
    power_on_root_and_wait_one_device();
    wait_for_new_dev_events(client_hdl, ctx, 1, 1);
    drain_events_for(client_hdl, 1, TEST_CLIENT_TIMING_NO_EVENT_MS);

    register_timing_client(&ctx[1], &client_hdl[1]);
    register_timing_client(&ctx[2], &client_hdl[2]);
    wait_for_new_dev_events(&client_hdl[1], &ctx[1], 2, 1);
    drain_events_for(&client_hdl[1], 2, TEST_CLIENT_TIMING_NO_EVENT_MS);

    TEST_ASSERT_EQUAL(1, ctx[0].new_dev_count);
    TEST_ASSERT_EQUAL(1, ctx[1].new_dev_count);
    TEST_ASSERT_EQUAL(1, ctx[2].new_dev_count);
    TEST_ASSERT_EQUAL(ctx[0].last_new_dev_addr, ctx[1].last_new_dev_addr);
    TEST_ASSERT_EQUAL(ctx[0].last_new_dev_addr, ctx[2].last_new_dev_addr);

    cleanup_timing_test(client_hdl, 3);
}
