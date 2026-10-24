/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "unity.h"
#include "usb/usb_host.h"
#include "test_cdc_common_private.h"

static const char *TAG = "cdc_common_manual";

#define EVENT_RW_STOPPED    BIT4

#define HOTPLUG_CYCLE_COUNT 5
#define HOTPLUG_POWER_OFF_MIN_MS 100
#define HOTPLUG_POWER_OFF_MAX_MS 2000
#define HOTPLUG_POWER_ON_MIN_MS 100
#define HOTPLUG_POWER_ON_MAX_MS 3000
#define HOTPLUG_RW_IO_TIMEOUT_MS 100
#define HOTPLUG_RW_INTERVAL_MS 200
#define HOTPLUG_RW_STOP_TIMEOUT_MS 1000
#define HOTPLUG_RW_TASK_STACK_SIZE 4096
#define HOTPLUG_RW_TASK_PRIORITY 4

static int hotplug_open_count = 0;
static int hotplug_open_fail_count = 0;
static bool hotplug_allow_open = false;
static TaskHandle_t hotplug_rw_task_handle = NULL;
static volatile bool hotplug_rw_stop = false;
static int hotplug_rw_start_count = 0;
static int hotplug_rw_stop_fail_count = 0;
static int hotplug_rw_write_count = 0;
static int hotplug_rw_read_attempt_count = 0;
static int hotplug_rw_read_count = 0;

static uint32_t random_range(uint32_t min, uint32_t max)
{
    if (min > max) {
        return min;
    }

    uint32_t range = max - min + 1;
    uint32_t limit = UINT32_MAX - (UINT32_MAX % range);
    uint32_t value;
    do {
        value = esp_random();
    } while (value >= limit);
    return min + (value % range);
}

static esp_err_t set_root_port_power(bool enable)
{
    esp_err_t ret = usb_host_lib_set_root_port_power(enable);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Root port power %s failed: %s", enable ? "on" : "off", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Root port power %s", enable ? "on" : "off");
    return ESP_OK;
}

static void hotplug_rw_task(void *arg)
{
    cdc_host_common_port_handle_t port = (cdc_host_common_port_handle_t)arg;
    const uint8_t tx_data[] = "CDC_COMMON_HOTPLUG\r\n";
    uint8_t rx_data[256];

    ESP_LOGI(TAG, "Hot-plug CDC read/write task started, port: %p", port);
    while (!hotplug_rw_stop) {
        esp_err_t ret = cdc_host_common_write_bytes(port, tx_data, sizeof(tx_data) - 1, pdMS_TO_TICKS(HOTPLUG_RW_IO_TIMEOUT_MS));
        if (ret == ESP_OK) {
            hotplug_rw_write_count++;
        } else if (!hotplug_rw_stop) {
            ESP_LOGW(TAG, "Hot-plug CDC write returned: %s", esp_err_to_name(ret));
        }

        hotplug_rw_read_attempt_count++;
        size_t rx_buf_size = 0;
        ret = cdc_host_common_get_rx_buffer_size(port, &rx_buf_size);
        if (ret == ESP_OK && rx_buf_size > 0) {
            size_t rx_len = rx_buf_size < sizeof(rx_data) ? rx_buf_size : sizeof(rx_data);
            ret = cdc_host_common_read_bytes(port, rx_data, &rx_len, 0);
            if (ret == ESP_OK && rx_len > 0) {
                hotplug_rw_read_count++;
                ESP_LOGI(TAG, "Hot-plug CDC read returned len: %u", (unsigned)rx_len);
                if (hotplug_rw_read_count <= 3) {
                    ESP_LOG_BUFFER_HEXDUMP("Hot-plug CDC read data", rx_data, rx_len, ESP_LOG_INFO);
                }
            }
        } else if (!hotplug_rw_stop && ret != ESP_OK && ret != ESP_ERR_INVALID_STATE && ret != ESP_ERR_INVALID_ARG) {
            ESP_LOGW(TAG, "Hot-plug CDC RX size query returned: %s", esp_err_to_name(ret));
        }

        vTaskDelay(pdMS_TO_TICKS(HOTPLUG_RW_INTERVAL_MS));
    }

    ESP_LOGI(TAG, "Hot-plug CDC read/write task stopped, port: %p", port);
    if (test_event_group) {
        xEventGroupSetBits(test_event_group, EVENT_RW_STOPPED);
    }
    vTaskDelete(NULL);
}

static esp_err_t hotplug_start_rw_task(cdc_host_common_port_handle_t port)
{
    if (hotplug_rw_task_handle) {
        ESP_LOGE(TAG, "Hot-plug CDC read/write task is already running");
        return ESP_ERR_INVALID_STATE;
    }

    hotplug_rw_stop = false;
    if (test_event_group) {
        xEventGroupClearBits(test_event_group, EVENT_RW_STOPPED);
    }

    BaseType_t task_created = xTaskCreatePinnedToCore(hotplug_rw_task, "cdc_hotplug_rw", HOTPLUG_RW_TASK_STACK_SIZE,
                                                      port, HOTPLUG_RW_TASK_PRIORITY, &hotplug_rw_task_handle, 0);
    if (task_created != pdPASS) {
        hotplug_rw_task_handle = NULL;
        ESP_LOGE(TAG, "Failed to create hot-plug CDC read/write task");
        return ESP_FAIL;
    }

    hotplug_rw_start_count++;
    return ESP_OK;
}

static bool hotplug_stop_rw_task(void)
{
    if (!hotplug_rw_task_handle) {
        hotplug_rw_stop = false;
        return true;
    }
    if (!test_event_group) {
        ESP_LOGE(TAG, "Hot-plug CDC read/write task stop failed: event group is NULL");
        return false;
    }

    // Stop the read/write task before the common layer frees the disconnected port.
    hotplug_rw_stop = true;
    EventBits_t bits = xEventGroupWaitBits(test_event_group, EVENT_RW_STOPPED, pdTRUE, pdFALSE, pdMS_TO_TICKS(HOTPLUG_RW_STOP_TIMEOUT_MS));
    if (!(bits & EVENT_RW_STOPPED)) {
        ESP_LOGE(TAG, "Hot-plug CDC read/write task stop timeout");
        return false;
    }

    hotplug_rw_task_handle = NULL;
    hotplug_rw_stop = false;
    return true;
}

static void hotplug_dev_event_cb(const cdc_host_common_dev_event_data_t *event, void *user_arg)
{
    (void)user_arg;

    switch (event->type) {
    case CDC_HOST_COMMON_DEV_EVENT_NEW: {
        connected_dev_addr = event->data.new_dev.dev_addr;
        device_connect_count++;
        ESP_LOGI(TAG, "Hot-plug CDC device connected, addr: %u", connected_dev_addr);

        if (hotplug_allow_open && !test_port1) {
            cdc_host_common_open_config_t config = CDC_COMMON_OPEN_RINGBUF_CONFIG(connected_dev_addr, TEST_INTERFACE_0,
                                                                                  NULL, common_port_event_cb, NULL);
            // Open synchronously while the temporary NEW_DEV device handle is available in common.
            esp_err_t ret = cdc_host_common_open(test_driver, &config, &test_port1);
            if (ret == ESP_OK) {
                hotplug_open_count++;
                ESP_LOGI(TAG, "Hot-plug CDC port opened: %p", test_port1);
                ret = hotplug_start_rw_task(test_port1);
                if (ret != ESP_OK) {
                    hotplug_open_fail_count++;
                    ESP_LOGE(TAG, "Failed to start hot-plug CDC read/write task: %s", esp_err_to_name(ret));
                }
            } else {
                hotplug_open_fail_count++;
                ESP_LOGW(TAG, "Hot-plug CDC port open failed: %s", esp_err_to_name(ret));
            }
        }

        if (test_event_group) {
            xEventGroupSetBits(test_event_group, EVENT_CONNECT);
            xEventGroupClearBits(test_event_group, EVENT_DISCONNECT);
        }
        break;
    }
    case CDC_HOST_COMMON_DEV_EVENT_GONE:
        device_disconnect_count++;
        ESP_LOGI(TAG, "Hot-plug CDC device disconnected, addr: %u", event->data.dev_gone.dev_addr);
        if (!hotplug_stop_rw_task()) {
            hotplug_rw_stop_fail_count++;
        }
        if (test_event_group) {
            xEventGroupSetBits(test_event_group, EVENT_DISCONNECT);
            xEventGroupClearBits(test_event_group, EVENT_CONNECT);
        }
        break;
    default:
        ESP_LOGW(TAG, "Unsupported hot-plug CDC device event");
        break;
    }
}

void cdc_common_manual_reset_state(void)
{
    if (hotplug_rw_task_handle) {
        cdc_common_manual_cleanup();
    }
    if (hotplug_rw_task_handle) {
        ESP_LOGE(TAG, "Manual state reset skipped because read/write task is still running");
        return;
    }

    hotplug_open_count = 0;
    hotplug_open_fail_count = 0;
    hotplug_allow_open = false;
    hotplug_rw_task_handle = NULL;
    hotplug_rw_stop = false;
    hotplug_rw_start_count = 0;
    hotplug_rw_stop_fail_count = 0;
    hotplug_rw_write_count = 0;
    hotplug_rw_read_attempt_count = 0;
    hotplug_rw_read_count = 0;
}

void cdc_common_manual_cleanup(void)
{
    hotplug_allow_open = false;
    if (hotplug_rw_task_handle && !hotplug_stop_rw_task()) {
        hotplug_rw_stop_fail_count++;
    }
}

TEST_CASE("cdc_common_single_port_random_hot_plugging", "[cdc_common][hot-plugging][functional]")
{
    UPDATE_LEAK_THRESHOLD(-40);
    TEST_ASSERT_NOT_NULL((test_event_group = xEventGroupCreate()));
    hotplug_allow_open = true;
    TEST_ASSERT_EQUAL(ESP_OK, install_common_driver(hotplug_dev_event_cb, NULL));

    vTaskDelay(pdMS_TO_TICKS(100));
    bool test_ok = true;
    bool root_power_is_off = false;
    esp_err_t ret = ESP_OK;

    if (!wait_for_cdc_device(TEST_WAIT_DEV_MS)) {
        ESP_LOGE(TAG, "No CDC device connected for hot-plugging test");
        test_ok = false;
        goto cleanup;
    }

    if (!test_port1) {
        ESP_LOGE(TAG, "CDC hot-plug port was not opened after initial connection");
        test_ok = false;
        goto cleanup;
    }

    vTaskDelay(pdMS_TO_TICKS(random_range(HOTPLUG_POWER_ON_MIN_MS, HOTPLUG_POWER_ON_MAX_MS)));

    for (int i = 0; i < HOTPLUG_CYCLE_COUNT; i++) {
        ESP_LOGI(TAG, "Hot-plug cycle %d/%d: disconnect", i + 1, HOTPLUG_CYCLE_COUNT);
        xEventGroupClearBits(test_event_group, EVENT_DISCONNECT);
        ret = set_root_port_power(false);
        if (ret != ESP_OK) {
            test_ok = false;
            break;
        }
        root_power_is_off = true;

        EventBits_t bits = xEventGroupWaitBits(test_event_group, EVENT_DISCONNECT, pdFALSE, pdFALSE, pdMS_TO_TICKS(TEST_WAIT_DEV_MS));
        if (!(bits & EVENT_DISCONNECT)) {
            ESP_LOGE(TAG, "CDC hot-plug disconnect timeout");
            test_ok = false;
            break;
        }
        ESP_LOGI(TAG, "Hot-plug disconnect event handled");
        // The device GONE callback can arrive before the port DISCONNECTED callback clears the test handle.
        for (int wait_ms = 0; test_port1 && wait_ms < TEST_WAIT_DEV_MS; wait_ms += 20) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (test_port1) {
            ESP_LOGE(TAG, "CDC hot-plug port handle was not cleared on disconnect");
            test_ok = false;
            break;
        }

        printf("Hot-plug stats: connect=%d, disconnect=%d, open=%d, open_fail=%d, rw_start=%d, rw_stop_fail=%d, write=%d, read_try=%d, read=%d\n",
               device_connect_count, device_disconnect_count, hotplug_open_count, hotplug_open_fail_count, hotplug_rw_start_count, hotplug_rw_stop_fail_count, hotplug_rw_write_count, hotplug_rw_read_attempt_count, hotplug_rw_read_count);
        printf("=================================================\r\n\r\n");

        vTaskDelay(pdMS_TO_TICKS(random_range(HOTPLUG_POWER_OFF_MIN_MS, HOTPLUG_POWER_OFF_MAX_MS)));
        ESP_LOGI(TAG, "Hot-plug cycle %d/%d: reconnect", i + 1, HOTPLUG_CYCLE_COUNT);
        xEventGroupClearBits(test_event_group, EVENT_CONNECT);
        ret = set_root_port_power(true);
        if (ret != ESP_OK) {
            test_ok = false;
            break;
        }
        root_power_is_off = false;

        bits = xEventGroupWaitBits(test_event_group, EVENT_CONNECT, pdFALSE, pdFALSE, pdMS_TO_TICKS(TEST_WAIT_DEV_MS));
        if (!(bits & EVENT_CONNECT)) {
            ESP_LOGE(TAG, "CDC hot-plug reconnect timeout");
            test_ok = false;
            break;
        }
        if (!test_port1) {
            ESP_LOGE(TAG, "CDC hot-plug port was not opened after reconnect");
            test_ok = false;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(random_range(HOTPLUG_POWER_ON_MIN_MS, HOTPLUG_POWER_ON_MAX_MS)));
    }

cleanup:
    // Prevent late NEW_DEV callbacks from opening a new test port during cleanup.
    hotplug_allow_open = false;
    if (!hotplug_stop_rw_task()) {
        hotplug_rw_stop_fail_count++;
        test_ok = false;
    }
    if (root_power_is_off) {
        ret = set_root_port_power(true);
        if (ret != ESP_OK) {
            test_ok = false;
        }
        root_power_is_off = false;
    }

    vTaskDelay(pdMS_TO_TICKS(500));
    if (test_port1) {
        esp_err_t close_ret = cdc_host_common_close(test_port1);
        if (close_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to close hot-plug CDC port: %s", esp_err_to_name(close_ret));
            test_ok = false;
        }
        test_port1 = NULL;
    }

    ESP_LOGI(TAG, "Hot-plug stats: connect=%d, disconnect=%d, open=%d, open_fail=%d, rw_start=%d, rw_stop_fail=%d, write=%d, read_try=%d, read=%d",
             device_connect_count, device_disconnect_count, hotplug_open_count, hotplug_open_fail_count, hotplug_rw_start_count,
             hotplug_rw_stop_fail_count, hotplug_rw_write_count, hotplug_rw_read_attempt_count, hotplug_rw_read_count);

    vEventGroupDelete(test_event_group);
    test_event_group = NULL;
    esp_err_t uninstall_ret = uninstall_common_driver();

    TEST_ASSERT_TRUE(test_ok);
    TEST_ASSERT_EQUAL(0, hotplug_open_fail_count);
    TEST_ASSERT_EQUAL(0, hotplug_rw_stop_fail_count);
    TEST_ASSERT_TRUE(device_connect_count >= HOTPLUG_CYCLE_COUNT + 1);
    TEST_ASSERT_TRUE(device_disconnect_count >= HOTPLUG_CYCLE_COUNT);
    TEST_ASSERT_TRUE(hotplug_open_count >= HOTPLUG_CYCLE_COUNT + 1);
    TEST_ASSERT_TRUE(hotplug_rw_start_count >= HOTPLUG_CYCLE_COUNT + 1);
    TEST_ASSERT_TRUE(hotplug_rw_write_count > 0);
    TEST_ASSERT_TRUE(hotplug_rw_read_attempt_count > 0);
    TEST_ASSERT_EQUAL(ESP_OK, uninstall_ret);
}

TEST_CASE("cdc_common_write_speed_test", "[cdc_common][write][speed]")
{
    UPDATE_LEAK_THRESHOLD(-40);
    TEST_ASSERT_NOT_NULL((test_event_group = xEventGroupCreate()));
    TEST_ASSERT_EQUAL(ESP_OK, install_common_driver(common_dev_event_cb, NULL));

    if (wait_for_cdc_device(TEST_WAIT_DEV_MS)) {
        cdc_host_common_open_config_t config = CDC_COMMON_OPEN_DEFAULT_CONFIG(get_test_dev_addr(), TEST_INTERFACE_0,
                                                                              NULL, common_port_event_cb, NULL);
        if (cdc_host_common_open(test_driver, &config, &test_port1) == ESP_OK) {
            const size_t data_size = 4096;
            const size_t total_bytes = 1000 * 1024;
            uint8_t *tx_data = malloc(data_size);
            TEST_ASSERT_NOT_NULL(tx_data);
            for (size_t i = 0; i < data_size; i++) {
                tx_data[i] = (uint8_t)(i & 0xFF);
            }

            int64_t start_us = esp_timer_get_time();
            size_t bytes_written = 0;
            for (size_t i = 0; i < total_bytes / data_size; i++) {
                if (cdc_host_common_write_bytes(test_port1, tx_data, data_size, pdMS_TO_TICKS(5000)) == ESP_OK) {
                    bytes_written += data_size;
                } else {
                    break;
                }
                printf("Written %u bytes...\r", (unsigned)bytes_written);
            }
            printf("\n");

            int64_t elapsed_us = esp_timer_get_time() - start_us;
            float speed_kbps = elapsed_us > 0 ? (bytes_written * 1000000.0f) / (elapsed_us * 1024.0f) : 0.0f;
            ESP_LOGI(TAG, "CDC common write: %u bytes, %.2f ms, %.2f KB/s", (unsigned)bytes_written, elapsed_us / 1000.0f, speed_kbps);
            free(tx_data);
            TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_close(test_port1));
            test_port1 = NULL;
        }
    }

    vEventGroupDelete(test_event_group);
    test_event_group = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, uninstall_common_driver());
}
