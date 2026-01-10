/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "unity.h"
#include "esp_private/usb_phy.h"
#include "usb/usb_host.h"
#include "usb/uac_host.h"
#include "test_host_uac_common.h"

#define BIT0_USB_HOST_DRIVER_REMOVED      (0x01 << 0)

const static char *TAG = "UAC_TEST";

static EventGroupHandle_t s_evt_handle = NULL;
QueueHandle_t transfer_event_queue = NULL;
QueueHandle_t client_event_queue = NULL;


// usb_host_lib_set_root_port_power is used to force toggle connection, primary developed for esp32p4
// esp32p4 is supported from IDF 5.3
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 3, 0)
static usb_phy_handle_t phy_hdl = NULL;

void force_conn_state(bool connected, TickType_t delay_ticks)
{
    TEST_ASSERT_NOT_EQUAL(NULL, phy_hdl);
    if (delay_ticks > 0) {
        // Delay of 0 ticks causes a yield. So skip if delay_ticks is 0.
        vTaskDelay(delay_ticks);
    }
    ESP_ERROR_CHECK(usb_phy_action(phy_hdl, (connected) ? USB_PHY_ACTION_HOST_ALLOW_CONN : USB_PHY_ACTION_HOST_FORCE_DISCONN));
}

// Initialize the internal USB PHY to connect to the USB OTG peripheral. We manually install the USB PHY for testing
static bool install_phy(void)
{
    usb_phy_config_t phy_config = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_HOST,
        .otg_speed = USB_PHY_SPEED_UNDEFINED,   // In Host mode, the speed is determined by the connected device
    };
    TEST_ASSERT_EQUAL(ESP_OK, usb_new_phy(&phy_config, &phy_hdl));
    // Return true, to skip_phy_setup during the usb_host_install()
    return true;
}

static void delete_phy(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, usb_del_phy(phy_hdl)); // Tear down USB PHY
    phy_hdl = NULL;
}
#else // ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 3, 0)

// Force connection/disconnection using root port power
void force_conn_state(bool connected, TickType_t delay_ticks)
{
    if (delay_ticks > 0) {
        // Delay of 0 ticks causes a yield. So skip if delay_ticks is 0.
        vTaskDelay(delay_ticks);
    }
    ESP_ERROR_CHECK(usb_host_lib_set_root_port_power(connected));
}

static bool install_phy(void)
{
    // Return false, NOT to skip_phy_setup during the usb_host_install()
    return false;
}

static void delete_phy(void) {}
#endif // ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 3, 0)


void uac_device_callback(uac_host_device_handle_t uac_device_handle, const uac_host_device_event_t event, void *arg)
{
    // Send uac device event to the event queue
    event_queue_t evt_queue = {
        .event_group = UAC_DEVICE_EVENT,
        .device_evt.handle = uac_device_handle,
        .device_evt.event = event,
        .device_evt.arg = arg
    };

    bool evt_type_transfer = false;
    bool evt_type_client = false;

    switch (event) {
    case UAC_HOST_DRIVER_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "UAC Device disconnected");
        TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_close(uac_device_handle));
        evt_type_client = true;
        break;
#ifdef UAC_HOST_SUSPEND_RESUME_API_SUPPORTED
    case UAC_HOST_DEVICE_EVENT_SUSPENDED:
        ESP_LOGI(TAG, "Device suspended");
        evt_type_client = true;
        break;
    case UAC_HOST_DEVICE_EVENT_RESUMED:
        ESP_LOGI(TAG, "Device resumed");
        evt_type_client = true;
        break;
#endif // UAC_HOST_SUSPEND_RESUME_API_SUPPORTED
    case UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR:
    case UAC_HOST_DEVICE_EVENT_RX_DONE:
    case UAC_HOST_DEVICE_EVENT_TX_DONE:
        evt_type_transfer = true;
        break;
    default:
        TEST_FAIL_MESSAGE("Unrecognized device event");
        break;
    }

    // Sanity check, logical XOR
    assert(evt_type_transfer != evt_type_client);

    if (evt_type_transfer) {
        xQueueSend(transfer_event_queue, &evt_queue, 0);
    } else {
        xQueueSend(client_event_queue, &evt_queue, 0);
    }
}

static void uac_host_lib_callback(uint8_t addr, uint8_t iface_num, const uac_host_driver_event_t event, void *arg)
{
    // Send uac driver event to the event queue
    event_queue_t evt_queue = {
        .event_group = UAC_DRIVER_EVENT,
        .driver_evt.addr = addr,
        .driver_evt.iface_num = iface_num,
        .driver_evt.event = event,
        .driver_evt.arg = arg
    };

    switch (event) {
    case UAC_HOST_DRIVER_EVENT_RX_CONNECTED:
        ESP_LOGI(TAG, "RX Device connected");
        break;
    case UAC_HOST_DRIVER_EVENT_TX_CONNECTED:
        ESP_LOGI(TAG, "TX Device connected");
        break;
    default:
        TEST_FAIL_MESSAGE("Unrecognized driver event");
    }

    xQueueSend(client_event_queue, &evt_queue, 0);
}

/**
 * @brief Start USB Host install and handle common USB host library events while app pin not low
 *
 * @param[in] arg  Not used
 */
static void usb_lib_task(void *arg)
{
    const bool skip_phy_setup = install_phy();
    const usb_host_config_t host_config = {
        .skip_phy_setup = skip_phy_setup,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };

    TEST_ASSERT_EQUAL(ESP_OK, usb_host_install(&host_config));
    ESP_LOGI(TAG, "USB Host installed");
    xTaskNotifyGive(arg);

    bool all_clients_gone = false;
    bool all_dev_free = false;
    while (!all_clients_gone || !all_dev_free) {
        // Start handling system events
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            printf("No more clients\n");
            usb_host_device_free_all();
            all_clients_gone = true;
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            printf("All devices freed\n");
            all_dev_free = true;
        }
#ifdef UAC_HOST_SUSPEND_RESUME_API_SUPPORTED
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_AUTO_SUSPEND) {
            printf("Auto suspend timer flag\n");
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());
        }
#endif // #ifdef UAC_HOST_SUSPEND_RESUME_API_SUPPORTED
    }

    ESP_LOGI(TAG, "USB Host shutdown");
    // Clean up USB Host
    vTaskDelay(10); // Short delay to allow clients clean-up
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_uninstall());
    delete_phy(); //Tear down USB PHY
    // set bit BIT0_USB_HOST_DRIVER_REMOVED to notify driver removed
    xEventGroupSetBits(s_evt_handle, BIT0_USB_HOST_DRIVER_REMOVED);
    vTaskDelete(NULL);
}

void test_uac_setup(void)
{
    // create a queue to handle events
    client_event_queue = xQueueCreate(8, sizeof(event_queue_t));
    TEST_ASSERT_NOT_NULL(client_event_queue);
    transfer_event_queue = xQueueCreate(16, sizeof(event_queue_t));
    TEST_ASSERT_NOT_NULL(transfer_event_queue);
    s_evt_handle = xEventGroupCreate();
    TEST_ASSERT_NOT_NULL(s_evt_handle);
    static TaskHandle_t uac_task_handle = NULL;
    // create USB lib task, pass the current task handle to notify when the task is created
    TEST_ASSERT_EQUAL(pdTRUE, xTaskCreatePinnedToCore(usb_lib_task,
                                                      "usb_events",
                                                      4096,
                                                      xTaskGetCurrentTaskHandle(),
                                                      5, &uac_task_handle, 0));

    // install uac host driver
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uac_host_driver_config_t uac_config = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = uac_host_lib_callback,
        .callback_arg = NULL
    };

    TEST_ASSERT_EQUAL(ESP_OK, uac_host_install(&uac_config));
    ESP_LOGI(TAG, "UAC Class Driver installed");
}

void test_uac_queue_reset(void)
{
    xQueueReset(client_event_queue);
    xQueueReset(transfer_event_queue);
}

void test_uac_teardown(bool force)
{
    if (force) {
        force_conn_state(false, pdMS_TO_TICKS(1000));
    }
    vTaskDelay(500);
    // uninstall uac host driver
    ESP_LOGI(TAG, "UAC Driver uninstall");
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_uninstall());
    // Wait for USB lib task to finish
    xEventGroupWaitBits(s_evt_handle, BIT0_USB_HOST_DRIVER_REMOVED, pdTRUE, pdTRUE, portMAX_DELAY);
    // delete event queue and event group
    vQueueDelete(client_event_queue);
    vQueueDelete(transfer_event_queue);
    vEventGroupDelete(s_evt_handle);
    // delay to allow task to delete
    vTaskDelay(100);
}

void test_open_mic_device(uint8_t iface_num, uint32_t buffer_size, uint32_t buffer_threshold, uac_host_device_handle_t *uac_device_handle)
{
    // check if device params as expected
    const uac_host_device_config_t dev_config = {
        .addr = 1,
        .iface_num = iface_num,
        .buffer_size = buffer_size,
        .buffer_threshold = buffer_threshold,
        .callback = uac_device_callback,
        .callback_arg = NULL,
    };
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_open(&dev_config, uac_device_handle));
}

void test_open_spk_device(uint8_t iface_num, uint32_t buffer_size, uint32_t buffer_threshold, uac_host_device_handle_t *uac_device_handle)
{
    // check if device params as expected
    const uac_host_device_config_t dev_config = {
        .addr = 1,
        .iface_num = iface_num,
        .buffer_size = buffer_size,
        .buffer_threshold = buffer_threshold,
        .callback = uac_device_callback,
        .callback_arg = NULL,
    };
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_open(&dev_config, uac_device_handle));
}

void test_close_device(uac_host_device_handle_t uac_device_handle)
{
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_close(uac_device_handle));
}

void test_handle_dev_connection(uint8_t *iface_num, uint8_t *if_rx)
{
    event_queue_t evt_queue = {0};
    // ignore the first connected event
    TEST_ASSERT_EQUAL(pdTRUE, xQueueReceive(client_event_queue, &evt_queue, portMAX_DELAY));
    TEST_ASSERT_EQUAL(UAC_DRIVER_EVENT, evt_queue.event_group);
    TEST_ASSERT_EQUAL(1, evt_queue.driver_evt.addr);
    if (iface_num) {
        *iface_num = evt_queue.driver_evt.iface_num;
    }
    if (if_rx) {
        *if_rx = evt_queue.driver_evt.event == UAC_HOST_DRIVER_EVENT_RX_CONNECTED ? 1 : 0;
    }
}

void expect_client_event(const event_queue_t *expected_client_event, TickType_t ticks)
{
    event_queue_t client_event;

    // Check, that no event is delivered
    if (expected_client_event == NULL) {
        if (pdFALSE == xQueueReceive(client_event_queue, &client_event, ticks)) {
            return;
        } else {
            TEST_FAIL_MESSAGE("Expecting NO client event, but an event delivered");
        }
    }

    // Check that an event is delivered
    if (pdTRUE == xQueueReceive(client_event_queue, &client_event, ticks)) {

        // Check event group
        TEST_ASSERT_EQUAL_MESSAGE(expected_client_event->event_group, client_event.event_group, "Unexpected event group");

        // Check event type according to event group
        if (expected_client_event->event_group == UAC_DRIVER_EVENT) {
            TEST_ASSERT_EQUAL_MESSAGE(expected_client_event->driver_evt.event, client_event.driver_evt.event, "Unexpected driver event");
        } else if (expected_client_event->event_group == UAC_DEVICE_EVENT) {
            TEST_ASSERT_EQUAL_MESSAGE(expected_client_event->device_evt.event, client_event.device_evt.event, "Unexpected device event");
        } else {
            // Event group not set, fail the test
            TEST_FAIL_MESSAGE("Client event group not set");
        }
    } else {
        TEST_FAIL_MESSAGE("Client event not generated on time");
    }
}

size_t expect_transfer_event(const event_queue_t *expected_transfer_event, TickType_t ticks)
{
    event_queue_t transfer_event;
    size_t events_delivered = 0;

    // Check, that no event is delivered
    if (expected_transfer_event == NULL) {
        if (pdFALSE == xQueueReceive(transfer_event_queue, &transfer_event, ticks)) {
            return 0;
        } else {
            TEST_FAIL_MESSAGE("Expecting NO transfer event, but an event delivered");
        }
    }

    TEST_ASSERT_EQUAL_MESSAGE(expected_transfer_event->event_group, UAC_DEVICE_EVENT, "Only device events are allowed");
    TimeOut_t queue_timeout;
    TickType_t remaining_ticks = ticks;
    vTaskSetTimeOutState(&queue_timeout);

    do {
        // Check that an event is delivered
        if (pdTRUE == xQueueReceive(transfer_event_queue, &transfer_event, remaining_ticks)) {
            TEST_ASSERT_EQUAL_MESSAGE(expected_transfer_event->device_evt.event, transfer_event.device_evt.event, "Unexpected transfer event");
            events_delivered++;
        } else {
            // Only fail if no event was delivered
            TEST_ASSERT_MESSAGE(events_delivered, "Transfer event not generated on time");
        }

    } while (xTaskCheckForTimeOut(&queue_timeout, &remaining_ticks) == pdFALSE);

    return events_delivered;
}
