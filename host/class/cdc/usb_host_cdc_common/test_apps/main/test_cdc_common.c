/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "unity.h"
#include "usb/cdc_host_common.h"
#include "usb/usb_host.h"
#include "usb/usb_types_ch9.h"
#include "test_cdc_common_private.h"

static const char *TAG = "cdc_common_test";

ssize_t TEST_MEMORY_LEAK_THRESHOLD = 0;

cdc_host_common_driver_handle_t test_driver = NULL;
static cdc_host_common_dev_event_cb_handle_t test_dev_event_cb = NULL;
cdc_host_common_port_handle_t test_port1 = NULL;
cdc_host_common_port_handle_t test_port2 = NULL;
EventGroupHandle_t test_event_group = NULL;
static TaskHandle_t usb_lib_task_handle = NULL;
static esp_err_t usb_lib_task_result = ESP_OK;
uint8_t connected_dev_addr = 0;
int device_connect_count = 0;
int device_disconnect_count = 0;
static size_t before_free_8bit;
static size_t before_free_32bit;

static void usb_lib_task(void *arg)
{
    TaskHandle_t notify_task = (TaskHandle_t)arg;
    usb_host_lib_info_t info;
    esp_err_t ret = usb_host_lib_info(&info);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "USB Host library installed by others, devices: %d, clients: %d", info.num_devices, info.num_clients);
    } else if (ret == ESP_ERR_INVALID_STATE) {
        // Install USB Host driver once for the test case.
        const usb_host_config_t host_config = {
            .skip_phy_setup = false,
            .intr_flags = ESP_INTR_FLAG_LEVEL1,
        };
        ret = usb_host_install(&host_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "USB Host install failed: %s", esp_err_to_name(ret));
            usb_lib_task_result = ret;
            xTaskNotifyGive(notify_task);
            usb_lib_task_handle = NULL;
            vTaskDelete(NULL);
            return;
        }
    } else {
        ESP_LOGE(TAG, "USB Host library state query failed: %s", esp_err_to_name(ret));
        usb_lib_task_result = ret;
        xTaskNotifyGive(notify_task);
        usb_lib_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    usb_lib_task_result = ESP_OK;
    xTaskNotifyGive(notify_task);

    bool has_clients = true;
    bool has_devices = false;
    while (has_clients) {
        uint32_t event_flags = 0;
        ret = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "USB Host event handling failed: %s", esp_err_to_name(ret));
            break;
        }
#ifdef USB_HOST_LIB_EVENT_FLAGS_AUTO_SUSPEND
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_AUTO_SUSPEND) {
            ret = usb_host_lib_root_port_suspend();
            if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND && ret != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "Root port auto suspend failed: %s", esp_err_to_name(ret));
            }
        }
#endif
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            if (usb_host_device_free_all() == ESP_OK) {
                has_clients = false;
            } else {
                has_devices = true;
            }
        }
        if (has_devices && (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE)) {
            has_clients = false;
        }
    }

    vTaskDelay(pdMS_TO_TICKS(100));
    ret = usb_host_uninstall();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "USB Host uninstall failed: %s", esp_err_to_name(ret));
    }
    usb_lib_task_result = ret;
    xTaskNotifyGive(notify_task);
    usb_lib_task_handle = NULL;
    vTaskDelete(NULL);
}

static esp_err_t test_usb_host_install(void)
{
    if (usb_lib_task_handle) {
        return ESP_OK;
    }

    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    usb_lib_task_result = ESP_OK;
    BaseType_t task_created = xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, current_task, 5, &usb_lib_task_handle, 0);
    ESP_RETURN_ON_FALSE(task_created == pdPASS, ESP_FAIL, TAG, "Failed to create USB Host library task");
    uint32_t notify_value = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    ESP_RETURN_ON_FALSE(notify_value != 0, ESP_ERR_TIMEOUT, TAG, "USB Host library install timeout");
    return usb_lib_task_result;
}

static esp_err_t test_usb_host_uninstall(void)
{
    if (!usb_lib_task_handle) {
        return ESP_OK;
    }

    usb_host_lib_unblock();
    uint32_t notify_value = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3000));
    ESP_RETURN_ON_FALSE(notify_value != 0, ESP_ERR_TIMEOUT, TAG, "USB Host library uninstall timeout");
    vTaskDelay(pdMS_TO_TICKS(1000));
    return usb_lib_task_result;
}

static bool common_data_cb(cdc_host_common_port_handle_t port, const uint8_t *data, size_t data_len, void *user_arg)
{
    (void)user_arg;
    ESP_LOGI(TAG, "CDC data received on port %p, len: %u", port, (unsigned)data_len);
    if (test_event_group) {
        xEventGroupSetBits(test_event_group, EVENT_DATA);
    }

    const uint8_t *rx_data = NULL;
    size_t rx_len = 0;
    esp_err_t ret = cdc_host_common_get_rx_data(port, &rx_data, &rx_len);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Current RX data: %p, len: %u", rx_data, (unsigned)rx_len);
        ESP_LOG_BUFFER_HEXDUMP("Current RX data:", rx_data, rx_len, ESP_LOG_INFO);
    }
    return true;
}

void common_port_event_cb(cdc_host_common_port_handle_t port, const cdc_host_common_port_event_data_t *event, void *user_arg)
{
    (void)user_arg;

    switch (event->type) {
    case CDC_HOST_COMMON_PORT_EVENT_ERROR:
        ESP_LOGW(TAG, "CDC transfer error on port %p: %d", port, event->data.error);
        break;
    case CDC_HOST_COMMON_PORT_EVENT_NOTIFICATION:
        ESP_LOGI(TAG, "CDC notification on port %p, len: %u", port, (unsigned)event->data.notification.data_len);
        if (test_event_group) {
            xEventGroupSetBits(test_event_group, EVENT_NOTIFICATION);
        }
        ESP_LOG_BUFFER_HEXDUMP("CDC notification data:", event->data.notification.data, event->data.notification.data_len, ESP_LOG_INFO);
        break;
    case CDC_HOST_COMMON_PORT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "CDC port disconnected: %p", port);
        if (test_port1 == port) {
            test_port1 = NULL;
        }
        if (test_port2 == port) {
            test_port2 = NULL;
        }
        if (test_event_group) {
            xEventGroupSetBits(test_event_group, EVENT_DISCONNECT);
            xEventGroupClearBits(test_event_group, EVENT_CONNECT);
        }
        break;
#ifdef CDC_HOST_COMMON_SUSPEND_RESUME_API_SUPPORTED
    case CDC_HOST_COMMON_PORT_EVENT_SUSPENDED:
        ESP_LOGI(TAG, "CDC port suspended: %p", port);
        break;
    case CDC_HOST_COMMON_PORT_EVENT_RESUMED:
        ESP_LOGI(TAG, "CDC port resumed: %p", port);
        break;
#endif
    default:
        ESP_LOGW(TAG, "Unsupported common CDC port event");
        break;
    }
}

void common_dev_event_cb(const cdc_host_common_dev_event_data_t *event, void *user_arg)
{
    (void)user_arg;

    switch (event->type) {
    case CDC_HOST_COMMON_DEV_EVENT_NEW:
        connected_dev_addr = event->data.new_dev.dev_addr;
        device_connect_count++;
        ESP_LOGI(TAG, "CDC common device connected, addr: %u", connected_dev_addr);
        if (test_event_group) {
            xEventGroupSetBits(test_event_group, EVENT_CONNECT);
            xEventGroupClearBits(test_event_group, EVENT_DISCONNECT);
        }
        break;
    case CDC_HOST_COMMON_DEV_EVENT_GONE:
        ESP_LOGI(TAG, "CDC common device disconnected, addr: %u", event->data.dev_gone.dev_addr);
        device_disconnect_count++;
        if (test_event_group) {
            xEventGroupSetBits(test_event_group, EVENT_DISCONNECT);
            xEventGroupClearBits(test_event_group, EVENT_CONNECT);
        }
        break;
    default:
        ESP_LOGW(TAG, "Unsupported common CDC device event");
        break;
    }
}

esp_err_t install_common_driver(cdc_host_common_dev_event_cb_t event_cb, void *user_arg)
{
    esp_err_t ret = test_usb_host_install();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install USB Host for common test: %s", esp_err_to_name(ret));
        return ret;
    }

    const cdc_host_common_driver_config_t config = {
        .task_stack_size = 4096,
        .task_priority = 5,
        .task_coreid = 0,
    };
    ret = cdc_host_common_acquire(&config, &test_driver);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to acquire CDC common driver: %s", esp_err_to_name(ret));
        test_usb_host_uninstall();
        return ret;
    }

    if (event_cb) {
        ret = cdc_host_common_register_dev_event_cb(test_driver, event_cb, user_arg, &test_dev_event_cb);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register CDC common device event callback: %s", esp_err_to_name(ret));
            cdc_host_common_release(test_driver);
            test_driver = NULL;
            test_usb_host_uninstall();
        }
    }
    return ret;
}

esp_err_t uninstall_common_driver(void)
{
    esp_err_t ret = ESP_OK;
    if (test_dev_event_cb) {
        ret = cdc_host_common_unregister_dev_event_cb(test_dev_event_cb);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to unregister CDC common event callback: %s", esp_err_to_name(ret));
        }
        test_dev_event_cb = NULL;
    }
    if (test_driver) {
        ret = cdc_host_common_release(test_driver);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to release CDC common driver: %s", esp_err_to_name(ret));
        }
        test_driver = NULL;
    }

    esp_err_t host_ret = test_usb_host_uninstall();
    if (ret == ESP_OK) {
        ret = host_ret;
    }
    vTaskDelay(pdMS_TO_TICKS(300));
    return ret;
}

bool wait_for_cdc_device(uint32_t timeout_ms)
{
    if (!test_event_group) {
        test_event_group = xEventGroupCreate();
        TEST_ASSERT_NOT_NULL(test_event_group);
    }

    EventBits_t bits = xEventGroupWaitBits(test_event_group, EVENT_CONNECT, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (!(bits & EVENT_CONNECT)) {
        ESP_LOGW(TAG, "No CDC-capable USB device connected, skip device-dependent part");
        return false;
    }
    return true;
}

uint8_t get_test_dev_addr(void)
{
    return connected_dev_addr ? connected_dev_addr : TEST_DEV_ADDR;
}

TEST_CASE("cdc_common_acquire_null_handle", "[cdc_common][auto][acquire][boundary]")
{
    UPDATE_LEAK_THRESHOLD(-40);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_acquire(NULL, NULL));
}

TEST_CASE("cdc_common_acquire_release_basic", "[cdc_common][auto][acquire-release]")
{
    UPDATE_LEAK_THRESHOLD(-40);
    TEST_ASSERT_EQUAL(ESP_OK, test_usb_host_install());

    const cdc_host_common_driver_config_t config = {
        .task_stack_size = 4096,
        .task_priority = 5,
        .task_coreid = 0,
    };
    TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_acquire(&config, &test_driver));
    TEST_ASSERT_NOT_NULL(test_driver);
    TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_release(test_driver));
    test_driver = NULL;

    TEST_ASSERT_EQUAL(ESP_OK, test_usb_host_uninstall());
}

TEST_CASE("cdc_common_acquire_duplicate", "[cdc_common][auto][acquire][boundary]")
{
    UPDATE_LEAK_THRESHOLD(-40);
    TEST_ASSERT_EQUAL(ESP_OK, test_usb_host_install());

    cdc_host_common_driver_handle_t driver1 = NULL;
    cdc_host_common_driver_handle_t driver2 = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_acquire(NULL, &driver1));
    TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_acquire(NULL, &driver2));
    TEST_ASSERT_EQUAL_PTR(driver1, driver2);
    TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_release(driver1));
    driver1 = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_release(driver2));
    driver2 = NULL;

    TEST_ASSERT_EQUAL(ESP_OK, test_usb_host_uninstall());
}

TEST_CASE("cdc_common_release_invalid_args", "[cdc_common][auto][release][boundary]")
{
    UPDATE_LEAK_THRESHOLD(-40);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_release(NULL));

    TEST_ASSERT_EQUAL(ESP_OK, install_common_driver(NULL, NULL));
    cdc_host_common_driver_handle_t invalid_driver = (cdc_host_common_driver_handle_t)0x12345678;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_release(invalid_driver));
    TEST_ASSERT_EQUAL(ESP_OK, uninstall_common_driver());
}

TEST_CASE("cdc_common_register_unregister_dev_event_cb", "[cdc_common][auto][callback][boundary]")
{
    UPDATE_LEAK_THRESHOLD(-40);
    TEST_ASSERT_EQUAL(ESP_OK, install_common_driver(NULL, NULL));

    cdc_host_common_dev_event_cb_handle_t cb_handle = NULL;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_register_dev_event_cb(NULL, common_dev_event_cb, NULL, &cb_handle));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_register_dev_event_cb(test_driver, NULL, NULL, &cb_handle));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_register_dev_event_cb(test_driver, common_dev_event_cb, NULL, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_register_dev_event_cb(test_driver, common_dev_event_cb, NULL, &cb_handle));
    TEST_ASSERT_NOT_NULL(cb_handle);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_unregister_dev_event_cb(NULL));
    TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_unregister_dev_event_cb(cb_handle));
    cb_handle = NULL;
    cdc_host_common_dev_event_cb_handle_t invalid_cb_handle = (cdc_host_common_dev_event_cb_handle_t)0xDEADBEEF;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, cdc_host_common_unregister_dev_event_cb(invalid_cb_handle));

    TEST_ASSERT_EQUAL(ESP_OK, uninstall_common_driver());
}

TEST_CASE("cdc_common_open_null_args", "[cdc_common][auto][open][boundary]")
{
    UPDATE_LEAK_THRESHOLD(-40);
    TEST_ASSERT_EQUAL(ESP_OK, install_common_driver(NULL, NULL));

    cdc_host_common_open_config_t config = CDC_COMMON_OPEN_DEFAULT_CONFIG(TEST_DEV_ADDR, TEST_INTERFACE_0, common_data_cb, common_port_event_cb, NULL);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_open(NULL, &config, &test_port1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_open(test_driver, NULL, &test_port1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_open(test_driver, &config, NULL));

    TEST_ASSERT_EQUAL(ESP_OK, uninstall_common_driver());
}

TEST_CASE("cdc_common_open_device_not_found", "[cdc_common][auto][open][boundary]")
{
    UPDATE_LEAK_THRESHOLD(-40);
    TEST_ASSERT_EQUAL(ESP_OK, install_common_driver(NULL, NULL));

    cdc_host_common_open_config_t config = CDC_COMMON_OPEN_DEFAULT_CONFIG(0xFF, TEST_INTERFACE_0, common_data_cb, common_port_event_cb, NULL);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, cdc_host_common_open(test_driver, &config, &test_port1));
    TEST_ASSERT_NULL(test_port1);

    TEST_ASSERT_EQUAL(ESP_OK, uninstall_common_driver());
}

TEST_CASE("cdc_common_open_invalid_ringbuf_config", "[cdc_common][auto][open][boundary]")
{
    UPDATE_LEAK_THRESHOLD(-40);
    TEST_ASSERT_EQUAL(ESP_OK, install_common_driver(NULL, NULL));

    cdc_host_common_open_config_t config = CDC_COMMON_OPEN_DEFAULT_CONFIG(CDC_HOST_COMMON_ANY_DEV_ADDR, TEST_INTERFACE_0,
                                                                          common_data_cb, common_port_event_cb, NULL);
    config.out_buffer_size = 0;
    config.tx_ringbuf_size = TEST_RINGBUF_SIZE;
    esp_err_t ret = cdc_host_common_open(test_driver, &config, &test_port1);
    TEST_ASSERT_TRUE(ret == ESP_ERR_INVALID_ARG || ret == ESP_ERR_NOT_FOUND);

    TEST_ASSERT_EQUAL(ESP_OK, uninstall_common_driver());
}

TEST_CASE("cdc_common_close_null_invalid_handle", "[cdc_common][auto][close][boundary]")
{
    UPDATE_LEAK_THRESHOLD(-40);
    TEST_ASSERT_EQUAL(ESP_OK, install_common_driver(NULL, NULL));

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_close(NULL));
    cdc_host_common_port_handle_t invalid_port = (cdc_host_common_port_handle_t)0xDEADBEEF;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_close(invalid_port));

    TEST_ASSERT_EQUAL(ESP_OK, uninstall_common_driver());
}

static esp_err_t idempotent_close_ret = ESP_FAIL;
static bool idempotent_close_seen = false;

static void idempotent_close_event_cb(cdc_host_common_port_handle_t port,
                                      const cdc_host_common_port_event_data_t *event,
                                      void *user_arg)
{
    (void)user_arg;
    // ESP_LOGI(TAG, "idempotent_close_event_cb");
    if (event->type == CDC_HOST_COMMON_PORT_EVENT_DISCONNECTED) {
        // We are inside the DISCONNECTED cb: the port is still registered and
        // flagged `to_close`, so this second close must be idempotent.
        idempotent_close_ret = cdc_host_common_close(port);
        idempotent_close_seen = true;
    }
    common_port_event_cb(port, event, user_arg);
}

TEST_CASE("cdc_common_close_idempotent_during_disconnect", "[cdc_common][auto][close][functional]")
{
    UPDATE_LEAK_THRESHOLD(-40);
    idempotent_close_ret = ESP_FAIL;
    idempotent_close_seen = false;

    TEST_ASSERT_NOT_NULL((test_event_group = xEventGroupCreate()));
    TEST_ASSERT_EQUAL(ESP_OK, install_common_driver(common_dev_event_cb, NULL));

    if (!wait_for_cdc_device(TEST_WAIT_DEV_MS)) {
        ESP_LOGW(TAG, "No CDC device connected; skipping idempotent-close regression");
        vEventGroupDelete(test_event_group);
        test_event_group = NULL;
        TEST_ASSERT_EQUAL(ESP_OK, uninstall_common_driver());
        return;
    }

    cdc_host_common_open_config_t config = CDC_COMMON_OPEN_DEFAULT_CONFIG(get_test_dev_addr(), TEST_INTERFACE_0,
                                                                          common_data_cb, idempotent_close_event_cb, NULL);
    esp_err_t ret = cdc_host_common_open(test_driver, &config, &test_port1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "CDC common open skipped, ret: %s", esp_err_to_name(ret));
        vEventGroupDelete(test_event_group);
        test_event_group = NULL;
        TEST_ASSERT_EQUAL(ESP_OK, uninstall_common_driver());
        return;
    }
    TEST_ASSERT_NOT_NULL(test_port1);

    // Precondition: an unknown pointer must still be rejected even while a
    // legitimately opened port exists.
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      cdc_host_common_close((cdc_host_common_port_handle_t)0xDEADBEEF));

    // Force disconnect via root port power toggle to drive the DISCONNECTED cb.
    ESP_LOGI(TAG, "Forcing root port power OFF to trigger DISCONNECTED callback");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_root_port_power(false));

    EventBits_t bits = xEventGroupWaitBits(test_event_group, EVENT_DISCONNECT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(5000));
    TEST_ASSERT_MESSAGE(bits & EVENT_DISCONNECT, "DISCONNECTED event not delivered");
    TEST_ASSERT_TRUE_MESSAGE(idempotent_close_seen, "DISCONNECTED cb did not run");
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, idempotent_close_ret,
                              "Re-entrant close during DISCONNECTED must be idempotent");
    // common layer freed the port right after cb returned; the callback clears it.
    TEST_ASSERT_NULL(test_port1);

    // Restore power for subsequent tests.
    vTaskDelay(pdMS_TO_TICKS(100));
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_root_port_power(true));

    vEventGroupDelete(test_event_group);
    test_event_group = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, uninstall_common_driver());
}

TEST_CASE("cdc_common_write_read_invalid_args", "[cdc_common][auto][read-write][boundary]")
{
    UPDATE_LEAK_THRESHOLD(-40);
    TEST_ASSERT_EQUAL(ESP_OK, install_common_driver(NULL, NULL));

    uint8_t data[64] = {0};
    size_t length = sizeof(data);
    cdc_host_common_port_handle_t invalid_port = (cdc_host_common_port_handle_t)0xDEADBEEF;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_write_bytes(NULL, data, length, pdMS_TO_TICKS(1000)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_write_bytes(invalid_port, NULL, length, pdMS_TO_TICKS(1000)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_write_bytes(invalid_port, data, 0, pdMS_TO_TICKS(1000)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_write_bytes(invalid_port, data, length, pdMS_TO_TICKS(1000)));

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_read_bytes(NULL, data, &length, pdMS_TO_TICKS(1000)));
    TEST_ASSERT_EQUAL(0, length);
    length = sizeof(data);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_read_bytes(invalid_port, NULL, &length, pdMS_TO_TICKS(1000)));
    TEST_ASSERT_EQUAL(0, length);
    length = sizeof(data);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_read_bytes(invalid_port, data, NULL, pdMS_TO_TICKS(1000)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_read_bytes(invalid_port, data, &length, pdMS_TO_TICKS(1000)));

    TEST_ASSERT_EQUAL(ESP_OK, uninstall_common_driver());
}

TEST_CASE("cdc_common_buffer_invalid_args", "[cdc_common][auto][buffer][boundary]")
{
    UPDATE_LEAK_THRESHOLD(-40);
    TEST_ASSERT_EQUAL(ESP_OK, install_common_driver(NULL, NULL));

    size_t size = 0;
    cdc_host_common_port_handle_t invalid_port = (cdc_host_common_port_handle_t)0xDEADBEEF;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_get_rx_buffer_size(NULL, &size));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_get_rx_buffer_size(invalid_port, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_get_rx_buffer_size(invalid_port, &size));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_flush_rx_buffer(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_flush_tx_buffer(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_flush_rx_buffer(invalid_port));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_flush_tx_buffer(invalid_port));

    TEST_ASSERT_EQUAL(ESP_OK, uninstall_common_driver());
}

TEST_CASE("cdc_common_descriptor_invalid_args", "[cdc_common][auto][descriptor][boundary]")
{
    UPDATE_LEAK_THRESHOLD(-40);
    TEST_ASSERT_EQUAL(ESP_OK, install_common_driver(NULL, NULL));

    usb_device_handle_t dev_handle = NULL;
    const usb_intf_desc_t *notif_intf = NULL;
    const usb_intf_desc_t *data_intf = NULL;
    cdc_comm_protocol_t comm_protocol;
    cdc_data_protocol_t data_protocol;
    const usb_standard_desc_t *cdc_desc = NULL;
    cdc_host_common_port_handle_t invalid_port = (cdc_host_common_port_handle_t)0xDEADBEEF;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_get_dev_handle(NULL, &dev_handle));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_get_dev_handle(invalid_port, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_get_dev_handle(invalid_port, &dev_handle));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_get_intf_desc(NULL, &notif_intf, &data_intf));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_get_intf_desc(invalid_port, NULL, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_protocols_get(NULL, &comm_protocol, &data_protocol));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_protocols_get(invalid_port, NULL, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_cdc_desc_get(NULL, USB_CDC_DESC_SUBTYPE_HEADER, &cdc_desc));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_cdc_desc_get(invalid_port, USB_CDC_DESC_SUBTYPE_MAX, &cdc_desc));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_cdc_desc_get(invalid_port, USB_CDC_DESC_SUBTYPE_HEADER, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_cdc_desc_get(invalid_port, USB_CDC_DESC_SUBTYPE_HEADER, &cdc_desc));

    TEST_ASSERT_EQUAL(ESP_OK, uninstall_common_driver());
}

TEST_CASE("cdc_common_control_request_invalid_args", "[cdc_common][auto][request][boundary]")
{
    UPDATE_LEAK_THRESHOLD(-40);
    TEST_ASSERT_EQUAL(ESP_OK, install_common_driver(NULL, NULL));

    uint8_t data[64] = {0};
    cdc_host_common_port_handle_t invalid_port = (cdc_host_common_port_handle_t)0xDEADBEEF;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_send_control(NULL, 0, 0, 0, 0, sizeof(data), data));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_send_control(invalid_port, 0, 0, 0, 0, sizeof(data), NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, cdc_host_common_send_control(invalid_port, 0, 0, 0, 0, sizeof(data), data));

    TEST_ASSERT_EQUAL(ESP_OK, uninstall_common_driver());
}

TEST_CASE("cdc_common_open_close_valid_device", "[cdc_common][auto][port][functional]")
{
    UPDATE_LEAK_THRESHOLD(-40);
    TEST_ASSERT_NOT_NULL((test_event_group = xEventGroupCreate()));
    TEST_ASSERT_EQUAL(ESP_OK, install_common_driver(common_dev_event_cb, NULL));

    if (wait_for_cdc_device(TEST_WAIT_DEV_MS)) {
        cdc_host_common_open_config_t config = CDC_COMMON_OPEN_DEFAULT_CONFIG(get_test_dev_addr(), TEST_INTERFACE_0,
                                                                              common_data_cb, common_port_event_cb, NULL);
        esp_err_t ret = cdc_host_common_open(test_driver, &config, &test_port1);
        if (ret == ESP_OK) {
            TEST_ASSERT_NOT_NULL(test_port1);
            ret = cdc_host_common_open(test_driver, &config, &test_port2);
            TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ret);
            TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_close(test_port1));
            test_port1 = NULL;
        } else {
            ESP_LOGW(TAG, "CDC common open skipped, ret: %s", esp_err_to_name(ret));
        }
    }

    vEventGroupDelete(test_event_group);
    test_event_group = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, uninstall_common_driver());
}

TEST_CASE("cdc_common_read_write_with_ringbuffer", "[cdc_common][auto][read-write][functional]")
{
    UPDATE_LEAK_THRESHOLD(-40);
    TEST_ASSERT_NOT_NULL((test_event_group = xEventGroupCreate()));
    TEST_ASSERT_EQUAL(ESP_OK, install_common_driver(common_dev_event_cb, NULL));

    if (wait_for_cdc_device(TEST_WAIT_DEV_MS)) {
        cdc_host_common_open_config_t config = CDC_COMMON_OPEN_RINGBUF_CONFIG(get_test_dev_addr(), TEST_INTERFACE_1,
                                                                              NULL, common_port_event_cb, NULL);
        if (cdc_host_common_open(test_driver, &config, &test_port1) == ESP_OK) {
            uint8_t tx_data[128] = "AT+GMR\r\n";
            uint8_t rx_data[128] = {0};

            esp_err_t ret = cdc_host_common_write_bytes(test_port1, tx_data, strlen((char *)tx_data), pdMS_TO_TICKS(1000));
            TEST_ASSERT_EQUAL(ESP_OK, ret);
            vTaskDelay(pdMS_TO_TICKS(1000));

            size_t rx_buf_size = 0;
            ret = cdc_host_common_get_rx_buffer_size(test_port1, &rx_buf_size);
            TEST_ASSERT_EQUAL(ESP_OK, ret);
            ESP_LOGI(TAG, "CDC common RX buffer size: %u", (unsigned)rx_buf_size);
            TEST_ASSERT_TRUE(rx_buf_size > 0);
            if (rx_buf_size > 0) {
                size_t rx_len = rx_buf_size < sizeof(rx_data) ? rx_buf_size : sizeof(rx_data);
                ret = cdc_host_common_read_bytes(test_port1, rx_data, &rx_len, pdMS_TO_TICKS(1000));
                ESP_LOGI(TAG, "CDC common ringbuffer read returned: %s, len: %u", esp_err_to_name(ret), (unsigned)rx_len);
                ESP_LOG_BUFFER_HEXDUMP("CDC common ringbuffer read data", rx_data, rx_len, ESP_LOG_INFO);
            }

            TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_flush_rx_buffer(test_port1));
            TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_flush_tx_buffer(test_port1));
            TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_close(test_port1));
            test_port1 = NULL;
        }
    }

    vEventGroupDelete(test_event_group);
    test_event_group = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, uninstall_common_driver());
}

TEST_CASE("cdc_common_read_write_without_ringbuffer", "[cdc_common][auto][read-write][functional]")
{
    UPDATE_LEAK_THRESHOLD(-40);
    TEST_ASSERT_NOT_NULL((test_event_group = xEventGroupCreate()));
    TEST_ASSERT_EQUAL(ESP_OK, install_common_driver(common_dev_event_cb, NULL));

    if (wait_for_cdc_device(TEST_WAIT_DEV_MS)) {
        cdc_host_common_open_config_t config = CDC_COMMON_OPEN_DEFAULT_CONFIG(get_test_dev_addr(), TEST_INTERFACE_1,
                                                                              common_data_cb, common_port_event_cb, NULL);
        if (cdc_host_common_open(test_driver, &config, &test_port1) == ESP_OK) {
            uint8_t tx_data[] = "AT+GMR\r\n";
            esp_err_t ret = cdc_host_common_write_bytes(test_port1, tx_data, strlen((char *)tx_data), pdMS_TO_TICKS(1000));
            TEST_ASSERT_EQUAL(ESP_OK, ret);
            vTaskDelay(pdMS_TO_TICKS(1000));


            TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, cdc_host_common_flush_rx_buffer(test_port1));
            TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, cdc_host_common_flush_tx_buffer(test_port1));
            TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_close(test_port1));
            test_port1 = NULL;
        }
    }

    vEventGroupDelete(test_event_group);
    test_event_group = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, uninstall_common_driver());
}

TEST_CASE("cdc_common_descriptor_operations", "[cdc_common][auto][descriptor][functional]")
{
    UPDATE_LEAK_THRESHOLD(-40);
    TEST_ASSERT_NOT_NULL((test_event_group = xEventGroupCreate()));
    TEST_ASSERT_EQUAL(ESP_OK, install_common_driver(common_dev_event_cb, NULL));

    if (wait_for_cdc_device(TEST_WAIT_DEV_MS)) {
        cdc_host_common_open_config_t config = CDC_COMMON_OPEN_DEFAULT_CONFIG(get_test_dev_addr(), TEST_INTERFACE_0,
                                                                              common_data_cb, common_port_event_cb, NULL);
        if (cdc_host_common_open(test_driver, &config, &test_port1) == ESP_OK) {
            usb_device_handle_t dev_handle = NULL;
            const usb_intf_desc_t *notif_intf = NULL;
            const usb_intf_desc_t *data_intf = NULL;
            cdc_comm_protocol_t comm_protocol;
            cdc_data_protocol_t data_protocol;
            const usb_standard_desc_t *cdc_desc = NULL;

            cdc_host_common_desc_print(test_port1);
            TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_get_dev_handle(test_port1, &dev_handle));
            TEST_ASSERT_NOT_NULL(dev_handle);
            TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_get_intf_desc(test_port1, &notif_intf, &data_intf));
            TEST_ASSERT_NOT_NULL(data_intf);
            ESP_LOGI(TAG, "CDC common interfaces, notif: %p, data: %u", notif_intf, data_intf->bInterfaceNumber);
            TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_protocols_get(test_port1, &comm_protocol, &data_protocol));
            ESP_LOGI(TAG, "CDC common protocols, comm: %d, data: %d", comm_protocol, data_protocol);

            esp_err_t ret = cdc_host_common_cdc_desc_get(test_port1, USB_CDC_DESC_SUBTYPE_HEADER, &cdc_desc);
            ESP_LOGI(TAG, "CDC header descriptor get returned: %s, desc: %p", esp_err_to_name(ret), cdc_desc);
            TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_close(test_port1));
            test_port1 = NULL;
        }
    }

    vEventGroupDelete(test_event_group);
    test_event_group = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, uninstall_common_driver());
}

TEST_CASE("cdc_common_custom_request", "[cdc_common][auto][request][functional]")
{
    UPDATE_LEAK_THRESHOLD(-40);
    TEST_ASSERT_NOT_NULL((test_event_group = xEventGroupCreate()));
    TEST_ASSERT_EQUAL(ESP_OK, install_common_driver(common_dev_event_cb, NULL));

    if (wait_for_cdc_device(TEST_WAIT_DEV_MS)) {
        cdc_host_common_open_config_t config = CDC_COMMON_OPEN_DEFAULT_CONFIG(get_test_dev_addr(), TEST_INTERFACE_0,
                                                                              common_data_cb, common_port_event_cb, NULL);
        if (cdc_host_common_open(test_driver, &config, &test_port1) == ESP_OK) {
            uint8_t str[64] = {0};
            uint8_t bm_request_type = USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_STANDARD | USB_BM_REQUEST_TYPE_RECIP_DEVICE;
            uint16_t w_value = (USB_W_VALUE_DT_STRING << 8) | 1;
            esp_err_t ret = cdc_host_common_send_control(test_port1, bm_request_type, USB_B_REQUEST_GET_DESCRIPTOR, w_value, 0x0409, sizeof(str), str);
            ESP_LOGI(TAG, "CDC common custom request returned: %s", esp_err_to_name(ret));
            if (ret == ESP_OK) {
                ESP_LOG_BUFFER_HEXDUMP(TAG, str, sizeof(str), ESP_LOG_INFO);
            }
            TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_close(test_port1));
            test_port1 = NULL;
        }
    }

    vEventGroupDelete(test_event_group);
    test_event_group = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, uninstall_common_driver());
}

TEST_CASE("cdc_common_flags_disable_notification", "[cdc_common][auto][flags][functional]")
{
    UPDATE_LEAK_THRESHOLD(-40);
    TEST_ASSERT_NOT_NULL((test_event_group = xEventGroupCreate()));
    TEST_ASSERT_EQUAL(ESP_OK, install_common_driver(common_dev_event_cb, NULL));

    if (wait_for_cdc_device(TEST_WAIT_DEV_MS)) {
        cdc_host_common_open_config_t config = CDC_COMMON_OPEN_DEFAULT_CONFIG(get_test_dev_addr(), TEST_INTERFACE_0,
                                                                              common_data_cb, common_port_event_cb, NULL);
        config.flags |= CDC_HOST_COMMON_OPEN_FLAG_DISABLE_NOTIFICATION;
        if (cdc_host_common_open(test_driver, &config, &test_port1) == ESP_OK) {
            uint8_t data[] = "TEST";
            ESP_LOGI(TAG, "CDC common port opened with DISABLE_NOTIFICATION flag");
            cdc_host_common_write_bytes(test_port1, data, strlen((char *)data), pdMS_TO_TICKS(1000));
            TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_close(test_port1));
            test_port1 = NULL;
        }
    }

    vEventGroupDelete(test_event_group);
    test_event_group = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, uninstall_common_driver());
}

TEST_CASE("cdc_common_multiple_ports", "[cdc_common][auto][multi-port][functional]")
{
    UPDATE_LEAK_THRESHOLD(-40);
    TEST_ASSERT_NOT_NULL((test_event_group = xEventGroupCreate()));
    TEST_ASSERT_EQUAL(ESP_OK, install_common_driver(common_dev_event_cb, NULL));

    if (wait_for_cdc_device(TEST_WAIT_DEV_MS)) {
        cdc_host_common_open_config_t config1 = CDC_COMMON_OPEN_DEFAULT_CONFIG(get_test_dev_addr(), TEST_INTERFACE_0,
                                                                               common_data_cb, common_port_event_cb, NULL);
        cdc_host_common_open_config_t config2 = CDC_COMMON_OPEN_DEFAULT_CONFIG(get_test_dev_addr(), TEST_INTERFACE_1,
                                                                               common_data_cb, common_port_event_cb, NULL);
        esp_err_t ret1 = cdc_host_common_open(test_driver, &config1, &test_port1);
        esp_err_t ret2 = cdc_host_common_open(test_driver, &config2, &test_port2);

        if (ret1 == ESP_OK) {
            ESP_LOGI(TAG, "CDC common port 1 opened");
            uint8_t data1[] = "PORT1";
            cdc_host_common_write_bytes(test_port1, data1, strlen((char *)data1), pdMS_TO_TICKS(1000));
        }
        if (ret2 == ESP_OK) {
            ESP_LOGI(TAG, "CDC common port 2 opened");
            uint8_t data2[] = "PORT2";
            cdc_host_common_write_bytes(test_port2, data2, strlen((char *)data2), pdMS_TO_TICKS(1000));
        }

        vTaskDelay(pdMS_TO_TICKS(500));
        if (test_port2) {
            TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_close(test_port2));
            test_port2 = NULL;
        }
        if (test_port1) {
            TEST_ASSERT_EQUAL(ESP_OK, cdc_host_common_close(test_port1));
            test_port1 = NULL;
        }
    }

    vEventGroupDelete(test_event_group);
    test_event_group = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, uninstall_common_driver());
}

static void check_leak(size_t before_free, size_t after_free, const char *type)
{
    ssize_t delta = after_free - before_free;
    printf("MALLOC_CAP_%s: Before: %u bytes free, After: %u bytes free (delta:%d)\n", type, before_free, after_free, delta);
    if (!(delta >= TEST_MEMORY_LEAK_THRESHOLD)) {
        ESP_LOGE(TAG, "Memory leak detected, delta: %d bytes, threshold: %d bytes", delta, TEST_MEMORY_LEAK_THRESHOLD);
    }
    TEST_ASSERT_MESSAGE(delta >= TEST_MEMORY_LEAK_THRESHOLD, "memory leak");
}

void setUp(void)
{
    before_free_8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    before_free_32bit = heap_caps_get_free_size(MALLOC_CAP_32BIT);
    test_driver = NULL;
    test_dev_event_cb = NULL;
    test_port1 = NULL;
    test_port2 = NULL;
    test_event_group = NULL;
    connected_dev_addr = 0;
    device_connect_count = 0;
    device_disconnect_count = 0;
    cdc_common_manual_reset_state();
}

void tearDown(void)
{
    cdc_common_manual_cleanup();
    if (test_port2) {
        cdc_host_common_close(test_port2);
        test_port2 = NULL;
    }
    if (test_port1) {
        cdc_host_common_close(test_port1);
        test_port1 = NULL;
    }
    if (test_event_group) {
        vEventGroupDelete(test_event_group);
        test_event_group = NULL;
    }
    if (test_driver || test_dev_event_cb || usb_lib_task_handle) {
        uninstall_common_driver();
    }

    size_t after_free_8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t after_free_32bit = heap_caps_get_free_size(MALLOC_CAP_32BIT);
    check_leak(before_free_8bit, after_free_8bit, "8BIT");
    check_leak(before_free_32bit, after_free_32bit, "32BIT");
}

void app_main(void)
{
    printf("USB HOST CDC COMMON TEST - Comprehensive Test Suite\n");
    unity_run_menu();
}
