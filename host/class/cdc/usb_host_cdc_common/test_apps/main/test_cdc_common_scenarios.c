/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "unity.h"
#include "test_cdc_common_private.h"

#define SCENARIO_OVERFLOW BIT8
#define SCENARIO_GONE BIT9

typedef struct {
    cdc_host_common_port_handle_t port;
    SemaphoreHandle_t started;
    SemaphoreHandle_t done;
    esp_err_t result;
    size_t length;
} scenario_reader_t;

static void scenario_reader_task(void *arg);

static void scenario_event(cdc_host_common_port_handle_t port, const cdc_host_common_port_event_data_t *event, void *arg)
{
    if (event->type == CDC_HOST_COMMON_PORT_EVENT_RX_OVERFLOW) {
        xEventGroupSetBits(test_event_group, SCENARIO_OVERFLOW);
    } else if (event->type == CDC_HOST_COMMON_PORT_EVENT_ERROR) {
        printf("CDC transfer error on %p: %d\n", port, event->data.error);
    } else if (event->type == CDC_HOST_COMMON_PORT_EVENT_DISCONNECTED) {
        // The test owns the disconnected handle until explicit close.
        xEventGroupSetBits(test_event_group, SCENARIO_GONE);
    }
}

static void scenario_start(void)
{
    TEST_MEMORY_LEAK_THRESHOLD = -40;
    connected_dev_addr = 0;
    test_event_group = xEventGroupCreate();
    TEST_ASSERT_NOT_NULL(test_event_group);
    TEST_ASSERT_EQUAL(ESP_OK, install_common_driver(common_dev_event_cb, NULL));
    TEST_ASSERT_TRUE_MESSAGE(wait_for_cdc_device(5000), "A dual-CDC echo device is required");
}

static cdc_host_common_open_config_t scenario_config(uint8_t interface, size_t rx_size, size_t tx_size)
{
    cdc_host_common_open_config_t config = CDC_COMMON_OPEN_DEFAULT_CONFIG(get_test_dev_addr(), interface, NULL, scenario_event, NULL);
    config.in_buffer_size = 64;
    config.out_buffer_size = 64;
    config.ctrl_buffer_size = 64;
    config.rx_ringbuf_size = rx_size;
    config.tx_ringbuf_size = tx_size;
    return config;
}

static void scenario_open(size_t rx_size, size_t tx_size)
{
    cdc_host_common_open_config_t config = scenario_config(0, rx_size, tx_size);
    TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_open(test_driver, &config, &test_port1));
}

static void scenario_finish(void)
{
    if (test_port2) {
        TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_close(test_port2));
        test_port2 = NULL;
    }
    if (test_port1) {
        TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_close(test_port1));
        test_port1 = NULL;
    }
    TEST_ASSERT_EQUAL(ESP_OK, uninstall_common_driver());
    vEventGroupDelete(test_event_group);
    test_event_group = NULL;
}

static void pattern(uint8_t *data, size_t length, uint8_t seed)
{
    for (size_t i = 0; i < length; i++) {
        data[i] = (uint8_t)(i * 37 + 13 * (i >> 8) + seed);
    }
}

static void read_exact(cdc_host_common_port_handle_t port, uint8_t *data, size_t length)
{
    size_t received = 0;
    while (received < length) {
        size_t chunk = length - received;
        esp_err_t ret = cdc_host_common_read_bytes(port, data + received, &chunk, pdMS_TO_TICKS(2000));
        if (ret != ESP_OK) {
            printf("RX failed: received=%u, expected=%u, error=%s\n", (unsigned)received, (unsigned)length, esp_err_to_name(ret));
        }
        TEST_ASSERT_EQUAL(ESP_OK, ret);
        TEST_ASSERT_GREATER_THAN(0, chunk);
        received += chunk;
    }
    TEST_ASSERT_EQUAL_UINT(length, received);
}

static void echo(cdc_host_common_port_handle_t port, size_t length, uint8_t seed)
{
    uint8_t tx[1024], rx[1024];
    TEST_ASSERT_LESS_OR_EQUAL(sizeof(tx), length);
    pattern(tx, length, seed);
    TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_write_bytes(port, tx, length, pdMS_TO_TICKS(2000)));
    read_exact(port, rx, length);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(tx, rx, length);
}

static void wait_rx_size(size_t expected)
{
    const TickType_t start = xTaskGetTickCount();
    size_t size = 0;
    do {
        TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_get_rx_buffer_size(test_port1, &size));
        if (size == expected) {
            return;
        }
        vTaskDelay(1);
    } while (xTaskGetTickCount() - start < pdMS_TO_TICKS(2000));
    TEST_ASSERT_EQUAL_UINT(expected, size);
}

static void payload_boundaries(size_t tx_ringbuf_size)
{
    scenario_start();
    scenario_open(4096, tx_ringbuf_size);
    const size_t lengths[] = {1, 2, 31, 63, 64, 65, 127, 128, 129, 255, 256, 257, 511, 512, 513, 1023, 1024};
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        echo(test_port1, lengths[i], (uint8_t)i);
    }
    scenario_finish();
}

TEST_CASE("scenario_binary_payload_boundaries", "[cdc_scenarios]")
{
    payload_boundaries(0);
}

TEST_CASE("scenario_buffered_tx_payload_boundaries", "[cdc_scenarios]")
{
    payload_boundaries(4096);
}

TEST_CASE("scenario_partial_reads_and_ring_wrap", "[cdc_scenarios]")
{
    scenario_start();
    scenario_open(128, 0);
    for (unsigned round = 0; round < 20; round++) {
        uint8_t tx[63], rx[63];
        pattern(tx, sizeof(tx), (uint8_t)round);
        TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_write_bytes(test_port1, tx, sizeof(tx), pdMS_TO_TICKS(1000)));
        read_exact(test_port1, rx, 17);
        read_exact(test_port1, rx + 17, sizeof(rx) - 17);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(tx, rx, sizeof(tx));
    }
    scenario_finish();
}

TEST_CASE("scenario_empty_read_timeout_then_recovery", "[cdc_scenarios]")
{
    scenario_start();
    scenario_open(128, 0);
    uint8_t data[8];
    size_t length = sizeof(data);
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, cdc_host_common_read_bytes(test_port1, data, &length, 0));
    TEST_ASSERT_EQUAL_UINT(0, length);
    length = sizeof(data);
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, cdc_host_common_read_bytes(test_port1, data, &length, pdMS_TO_TICKS(20)));
    TEST_ASSERT_EQUAL_UINT(0, length);
    echo(test_port1, 65, 0x55);
    scenario_finish();
}

TEST_CASE("scenario_rx_flush_discards_only_old_data", "[cdc_scenarios]")
{
    scenario_start();
    scenario_open(128, 0);
    uint8_t data[64];
    pattern(data, sizeof(data), 0x42);
    TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_write_bytes(test_port1, data, sizeof(data), pdMS_TO_TICKS(1000)));
    wait_rx_size(sizeof(data));
    TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_flush_rx_buffer(test_port1));
    size_t length = sizeof(data);
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, cdc_host_common_read_bytes(test_port1, data, &length, 0));
    TEST_ASSERT_EQUAL_UINT(0, length);
    echo(test_port1, 63, 0xAA);
    scenario_finish();
}

TEST_CASE("scenario_exact_rx_capacity_overflow_and_recovery", "[cdc_scenarios]")
{
    scenario_start();
    scenario_open(64, 0);
    uint8_t tx[64], rx[64];
    pattern(tx, sizeof(tx), 0x81);
    TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_write_bytes(test_port1, tx, sizeof(tx), pdMS_TO_TICKS(1000)));
    wait_rx_size(64);
    TEST_ASSERT_FALSE(xEventGroupGetBits(test_event_group) & SCENARIO_OVERFLOW);
    TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_write_bytes(test_port1, tx, 1, pdMS_TO_TICKS(1000)));
    TEST_ASSERT_TRUE(xEventGroupWaitBits(test_event_group, SCENARIO_OVERFLOW, pdTRUE, pdTRUE, pdMS_TO_TICKS(2000)) & SCENARIO_OVERFLOW);
    read_exact(test_port1, rx, sizeof(rx));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(tx, rx, sizeof(rx));
    echo(test_port1, 16, 0x12);
    scenario_finish();
}

TEST_CASE("scenario_dual_port_stream_isolation", "[cdc_scenarios]")
{
    scenario_start();
    scenario_open(512, 512);
    cdc_host_common_open_config_t config = scenario_config(2, 512, 512);
    TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_open(test_driver, &config, &test_port2));
    for (unsigned round = 0; round < 20; round++) {
        uint8_t tx1[129], tx2[127], rx[129];
        pattern(tx1, sizeof(tx1), (uint8_t)round);
        pattern(tx2, sizeof(tx2), (uint8_t)(round + 128));
        TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_write_bytes(test_port1, tx1, sizeof(tx1), pdMS_TO_TICKS(1000)));
        TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_write_bytes(test_port2, tx2, sizeof(tx2), pdMS_TO_TICKS(1000)));
        read_exact(test_port2, rx, sizeof(tx2));
        TEST_ASSERT_EQUAL_HEX8_ARRAY(tx2, rx, sizeof(tx2));
        read_exact(test_port1, rx, sizeof(tx1));
        TEST_ASSERT_EQUAL_HEX8_ARRAY(tx1, rx, sizeof(tx1));
    }
    TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_close(test_port1));
    test_port1 = NULL;
    echo(test_port2, 65, 0x7E);
    scenario_finish();
}

TEST_CASE("scenario_reopen_preserves_other_port", "[cdc_scenarios]")
{
    scenario_start();
    cdc_host_common_open_config_t config = scenario_config(2, 256, 0);
    TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_open(test_driver, &config, &test_port2));
    for (unsigned round = 0; round < 10; round++) {
        // Keep the other interface's toggles nonzero across this interface's open.
        echo(test_port2, 1, (uint8_t)round);
        scenario_open(256, 0);
        echo(test_port1, 1, (uint8_t)(round + 32));
        echo(test_port2, 1, (uint8_t)(round + 64));
        TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_close(test_port1));
        test_port1 = NULL;
    }
    scenario_finish();
}

TEST_CASE("scenario_reopen_same_interface_25_times", "[cdc_scenarios]")
{
    scenario_start();
    for (unsigned round = 0; round < 25; round++) {
        scenario_open(256, 0);
        cdc_host_common_open_config_t config = scenario_config(0, 256, 0);
        cdc_host_common_port_handle_t duplicate = NULL;
        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, cdc_host_common_open(test_driver, &config, &duplicate));
        TEST_ASSERT_NULL(duplicate);
        echo(test_port1, 65, (uint8_t)round);
        TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_close(test_port1));
        test_port1 = NULL;
    }
    scenario_finish();
}

static void disconnect_and_recover(bool event_callback)
{
    scenario_start();
    cdc_host_common_open_config_t config = scenario_config(0, 256, 0);
    config.event_cb = event_callback ? scenario_event : NULL;
    TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_open(test_driver, &config, &test_port1));
    echo(test_port1, 65, 0x3A);
    xEventGroupClearBits(test_event_group, EVENT_CONNECT | EVENT_DISCONNECT | SCENARIO_GONE);
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_root_port_power(false));
    TEST_ASSERT_TRUE(xEventGroupWaitBits(test_event_group, EVENT_DISCONNECT, pdTRUE, pdTRUE, pdMS_TO_TICKS(2000)) & EVENT_DISCONNECT);
    if (event_callback) {
        TEST_ASSERT_TRUE(xEventGroupWaitBits(test_event_group, SCENARIO_GONE, pdTRUE, pdTRUE, pdMS_TO_TICKS(2000)) & SCENARIO_GONE);
    }
    // The global removal notification precedes per-port state updates.
    const TickType_t start = xTaskGetTickCount();
    size_t size;
    esp_err_t ret;
    do {
        ret = cdc_host_common_get_rx_buffer_size(test_port1, &size);
        if (ret == ESP_ERR_INVALID_STATE) {
            break;
        }
        vTaskDelay(1);
    } while (xTaskGetTickCount() - start < pdMS_TO_TICKS(1000));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ret);
    uint8_t data = 1;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, cdc_host_common_write_bytes(test_port1, &data, 1, 0));
    TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_close(test_port1));
    test_port1 = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_root_port_power(true));
    TEST_ASSERT_TRUE_MESSAGE(wait_for_cdc_device(5000), "Device did not reconnect");
    scenario_open(256, 0);
    echo(test_port1, 65, 0xB4);
    scenario_finish();
}

TEST_CASE("scenario_disconnect_without_user_callback", "[cdc_scenarios]")
{
    disconnect_and_recover(false);
}

TEST_CASE("scenario_disconnect_delayed_close_and_reconnect", "[cdc_scenarios]")
{
    disconnect_and_recover(true);
}

TEST_CASE("scenario_notification_disabled_control_and_echo", "[cdc_scenarios]")
{
    scenario_start();
    cdc_host_common_open_config_t config = scenario_config(0, 256, 0);
    config.flags = CDC_HOST_COMMON_OPEN_FLAG_DISABLE_NOTIFICATION;
    config.ctrl_buffer_size = 7;
    TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_open(test_driver, &config, &test_port1));
    const usb_intf_desc_t *notification = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_get_intf_desc(test_port1, &notification, NULL));
    TEST_ASSERT_NULL(notification);
    uint8_t saved[7], actual[7], requested[7] = {0};
    TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_send_control(test_port1, 0xA1, 0x21, 0, 0, sizeof(saved), saved));
    const uint32_t rates[] = {9600, 115200, 921600};
    for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        memcpy(requested, &rates[i], sizeof(rates[i]));
        requested[6] = 8;
        TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_send_control(test_port1, 0x21, 0x20, 0, 0, sizeof(requested), requested));
        TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_send_control(test_port1, 0xA1, 0x21, 0, 0, sizeof(actual), actual));
        TEST_ASSERT_EQUAL_HEX8_ARRAY(requested, actual, sizeof(actual));
    }
    uint8_t oversized[8];
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, cdc_host_common_send_control(test_port1, 0xA1, 0x21, 0, 0, sizeof(oversized), oversized));
    TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_send_control(test_port1, 0x21, 0x20, 0, 0, sizeof(saved), saved));
    echo(test_port1, 129, 0x5A);
    scenario_finish();
}

TEST_CASE("scenario_close_interrupts_waiting_reader", "[cdc_scenarios]")
{
    scenario_start();
    scenario_open(256, 0);
    // Static storage remains valid if a failed assertion leaves the reader running.
    static scenario_reader_t reader;
    reader = (scenario_reader_t) {
        test_port1, xSemaphoreCreateBinary(), xSemaphoreCreateBinary(), ESP_FAIL, 1
    };
    TEST_ASSERT_NOT_NULL(reader.started);
    TEST_ASSERT_NOT_NULL(reader.done);
    // On this core the higher-priority reader blocks in read before the test resumes.
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreatePinnedToCore(scenario_reader_task, "scenario_reader", 3072, &reader, uxTaskPriorityGet(NULL) + 1, NULL, xPortGetCoreID()));
    BaseType_t started = xSemaphoreTake(reader.started, pdMS_TO_TICKS(1000));
    esp_err_t closed = cdc_host_common_close(test_port1);
    if (closed == ESP_OK) {
        test_port1 = NULL;
    }
    BaseType_t done = xSemaphoreTake(reader.done, pdMS_TO_TICKS(1000));
    if (done == pdTRUE) {
        vSemaphoreDelete(reader.started);
        vSemaphoreDelete(reader.done);
    }
    TEST_ASSERT_EQUAL(pdTRUE, started);
    TEST_ASSERT_EQUAL(ESP_OK, closed);
    TEST_ASSERT_EQUAL(pdTRUE, done);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, reader.result);
    TEST_ASSERT_EQUAL_UINT(0, reader.length);
    scenario_finish();
}

static void scenario_reader_task(void *arg)
{
    scenario_reader_t *reader = arg;
    uint8_t data;
    xSemaphoreGive(reader->started);
    reader->result = cdc_host_common_read_bytes(reader->port, &data, &reader->length, portMAX_DELAY);
    xSemaphoreGive(reader->done);
    vTaskDelete(NULL);
}

static void reopen_short_packets(unsigned exchanges)
{
    scenario_start();
    for (unsigned round = 0; round < 10; round++) {
        scenario_open(64, 0);
        for (unsigned i = 0; i < exchanges; i++) {
            echo(test_port1, 1, (uint8_t)(round * 2 + i));
        }
        TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_close(test_port1));
        test_port1 = NULL;
    }
    scenario_finish();
}

TEST_CASE("scenario_reopen_after_one_short_packet", "[cdc_scenarios]")
{
    reopen_short_packets(1);
}

TEST_CASE("scenario_reopen_after_two_short_packets", "[cdc_scenarios]")
{
    reopen_short_packets(2);
}
