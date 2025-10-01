/*
 * SPDX-FileCopyrightText: 2015-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_intr_alloc.h"
#include "dev_msc.h"
#include "msc_client.h"
#include "ctrl_client.h"
#include "multiconf_client.h"
#include "usb/usb_host.h"
#include "unity.h"

#define TEST_MSC_NUM_SECTORS_TOTAL          10
#define TEST_MSC_NUM_SECTORS_PER_XFER       2
#define TEST_MSC_SCSI_TAG                   0xDEADBEEF
#define TEST_CTRL_NUM_TRANSFERS             30
#define B_CONFIGURATION_VALUE               1

const char *USB_HOST_ASYNC_TAG = "USB Host async";

// --------------------------------------------------- Test Cases ------------------------------------------------------

/*
Test USB Host Asynchronous API single client

Requires: This test requires an MSC SCSI device to be attached (see the MSC mock class)

Purpose:
    - Test that USB Host Asynchronous API works correctly with a single client
    - Test that a client can be created
    - Test that client can operate concurrently in a separate thread
    - Test that the main thread is able to detect library events (such as USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)

Procedure:
    - Install USB Host Library
    - Create a task to run an MSC client
    - Start the MSC client task. It will execute a bunch of MSC SCSI sector reads
    - Wait for the host library event handler to report a USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS event
    - Free all devices
    - Uninstall USB Host Library
*/

TEST_CASE("Test USB Host async client (single client)", "[usb_host][full_speed][high_speed]")
{
    // Create task to run client that communicates with MSC SCSI interface
    msc_client_test_param_t params = {
        .num_sectors_to_read = TEST_MSC_NUM_SECTORS_TOTAL,
        .num_sectors_per_xfer = TEST_MSC_NUM_SECTORS_PER_XFER,
        .msc_scsi_xfer_tag = TEST_MSC_SCSI_TAG,
    };
    TaskHandle_t task_hdl;
    xTaskCreatePinnedToCore(msc_client_async_seq_task, "async", 4096, (void *)&params, 2, &task_hdl, 0);
    TEST_ASSERT_NOT_NULL_MESSAGE(task_hdl, "Failed to create async task");
    // Start the task
    xTaskNotifyGive(task_hdl);

    while (1) {
        // Start handling system events
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            printf("No more clients\n");
            TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, usb_host_device_free_all());
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            break;
        }
    }
}

/*
Test USB Host Asynchronous API with multiple clients

Requires: This test requires an MSC SCSI device to be attached (see the MSC mock class)

Purpose:
    - Test the USB Host Asynchronous API works correctly with multiple clients
    - Test that multiple clients can be created
    - Test that multiple clients can operate concurrently in separate threads
    - Test that the main thread is able to detect library events (such as USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)

Procedure:
    - Install USB Host Library
    - Create separate tasks to run an MSC client and Ctrl Client
        - MSC Client will execute a bunch of MSC SCSI sector reads
        - Ctrl Client will execute a bunch of control transfers
    - Wait for the host library event handler to report a USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS event
    - Free all devices
    - Uninstall USB Host Library
*/
TEST_CASE("Test USB Host async client (multi client)", "[usb_host][full_speed][high_speed]")
{
    // Create task to run the MSC client
    msc_client_test_param_t msc_params = {
        .num_sectors_to_read = TEST_MSC_NUM_SECTORS_TOTAL,
        .num_sectors_per_xfer = TEST_MSC_NUM_SECTORS_PER_XFER,
        .msc_scsi_xfer_tag = TEST_MSC_SCSI_TAG,
    };
    TaskHandle_t msc_task_hdl;
    xTaskCreatePinnedToCore(msc_client_async_seq_task, "msc", 4096, (void *)&msc_params, 2, &msc_task_hdl, 0);
    TEST_ASSERT_NOT_NULL_MESSAGE(msc_task_hdl, "Failed to create MSC task");

    // Create task a control transfer client
    ctrl_client_test_param_t ctrl_params = {
        .num_ctrl_xfer_to_send = TEST_CTRL_NUM_TRANSFERS,
    };
    TaskHandle_t ctrl_task_hdl;
    xTaskCreatePinnedToCore(ctrl_client_async_seq_task, "ctrl", 4096, (void *)&ctrl_params, 2, &ctrl_task_hdl, 0);
    TEST_ASSERT_NOT_NULL_MESSAGE(ctrl_task_hdl, "Failed to create CTRL task");

    // Start both tasks
    xTaskNotifyGive(msc_task_hdl);
    xTaskNotifyGive(ctrl_task_hdl);

    while (1) {
        // Start handling system events
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            printf("No more clients\n");
            TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, usb_host_device_free_all());
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            break;
        }
    }
}

/*
Test USB Host Asynchronous API Usage

Requires: This test requires an MSC SCSI device to be attached (see the MSC mock class)

Purpose:
    - Test that incorrect usage of USB Host Asynchronous API will returns errors

Procedure:
    - Install USB Host Library
    - Register two clients and all event handler functions from the same loop
    - Wait for each client to detect device connection
    - Check that both clients can open the same device
    - Check that a client cannot open a non-existent device
    - Check that only one client can claim a particular interface
    - Check that a client cannot release an already released interface
    - Wait for device disconnection
    - Cleanup
*/

static uint8_t dev_addr = 0;

typedef enum {
    CLIENT_TEST_STAGE_NONE,
    CLIENT_TEST_STAGE_CONN,
    CLIENT_TEST_STAGE_DCONN,
} client_test_stage_t;

static void test_async_client_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    client_test_stage_t *stage = (client_test_stage_t *)arg;

    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        if (dev_addr == 0) {
            dev_addr = event_msg->new_dev.address;
        } else {
            TEST_ASSERT_EQUAL(dev_addr, event_msg->new_dev.address);
        }
        *stage = CLIENT_TEST_STAGE_CONN;
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        *stage = CLIENT_TEST_STAGE_DCONN;
        break;
    default:
        abort();
        break;
    }
}

TEST_CASE("Test USB Host async API", "[usb_host][low_speed][full_speed][high_speed]")
{
    // Register two clients
    client_test_stage_t client0_stage = CLIENT_TEST_STAGE_NONE;
    client_test_stage_t client1_stage = CLIENT_TEST_STAGE_NONE;

    usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = test_async_client_cb,
            .callback_arg = (void *) &client0_stage,
        },
    };
    usb_host_client_handle_t client0_hdl;
    usb_host_client_handle_t client1_hdl;
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_client_register(&client_config, &client0_hdl));
    client_config.async.callback_arg = (void *)&client1_stage;
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_client_register(&client_config, &client1_hdl));

    // Wait until the device connects and the clients receive the event
    while (!(client0_stage == CLIENT_TEST_STAGE_CONN && client1_stage == CLIENT_TEST_STAGE_CONN)) {
        usb_host_lib_handle_events(0, NULL);
        usb_host_client_handle_events(client0_hdl, 0);
        usb_host_client_handle_events(client1_hdl, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Check that both clients can open the device
    TEST_ASSERT_NOT_EQUAL(0, dev_addr);
    usb_device_handle_t client0_dev_hdl;
    usb_device_handle_t client1_dev_hdl;
    printf("Opening device\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_device_open(client0_hdl, dev_addr, &client0_dev_hdl));
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_device_open(client1_hdl, dev_addr, &client1_dev_hdl));
    TEST_ASSERT_EQUAL_PTR(client0_dev_hdl, client1_dev_hdl);    // Check that its the same device
    // Check that a client cannot open a non-existent device
    TEST_ASSERT_NOT_EQUAL(ESP_OK, usb_host_device_open(client0_hdl, 0, &client0_dev_hdl));

    // Check that the device cannot be opened again by the same client
    const dev_msc_info_t *dev_info = dev_msc_get_info();
    usb_device_handle_t dummy_dev_hdl;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, usb_host_device_open(client0_hdl, dev_addr, &dummy_dev_hdl));
    TEST_ASSERT_NOT_EQUAL(ESP_OK, usb_host_device_open(client1_hdl, dev_addr, &dummy_dev_hdl));
    printf("Claiming interface\n");
    // Check that both clients cannot claim the same interface
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_interface_claim(client0_hdl,
                                                       client0_dev_hdl,
                                                       dev_info->bInterfaceNumber,
                                                       dev_info->bAlternateSetting));
    TEST_ASSERT_NOT_EQUAL(ESP_OK, usb_host_interface_claim(client1_hdl,
                                                           client1_dev_hdl,
                                                           dev_info->bInterfaceNumber,
                                                           dev_info->bAlternateSetting));
    // Check that client0 cannot claim the same interface multiple times
    TEST_ASSERT_NOT_EQUAL(ESP_OK, usb_host_interface_claim(client0_hdl,
                                                           client0_dev_hdl,
                                                           dev_info->bInterfaceNumber,
                                                           dev_info->bAlternateSetting));

    printf("Releasing interface\n");
    // Check that client0 can release the interface
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_interface_release(client0_hdl,
                                                         client0_dev_hdl,
                                                         dev_info->bInterfaceNumber));
    // Check that client0 cannot release interface it has not claimed
    TEST_ASSERT_NOT_EQUAL(ESP_OK, usb_host_interface_release(client0_hdl,
                                                             client0_dev_hdl,
                                                             dev_info->bInterfaceNumber));

    // Trigger a disconnect by powering OFF the root port
    usb_host_lib_set_root_port_power(false);
    // Wait until the device disconnects and the clients receive the event
    while (!(client0_stage == CLIENT_TEST_STAGE_DCONN && client1_stage == CLIENT_TEST_STAGE_DCONN)) {
        usb_host_lib_handle_events(0, NULL);
        usb_host_client_handle_events(client0_hdl, 0);
        usb_host_client_handle_events(client1_hdl, 0);
        vTaskDelay(10);
    }
    printf("Closing device\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_device_close(client0_hdl, client0_dev_hdl));
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_device_close(client1_hdl, client1_dev_hdl));

    // Deregister the clients
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_client_deregister(client0_hdl));
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_client_deregister(client1_hdl));

    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(0, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            break;
        }
        vTaskDelay(10);
    }
}

/*
Test USB Host Asynchronous API single client

Purpose:
    - Test that client can read configuration descriptor by request

Procedure:
    - Install USB Host Library
    - Create a task to run a multiconfig client
    - Create a task to handle system events
    - Start the MSC client task. It will open the device and start handling client events
    - Wait for the main task requests client to read configuration descriptor
    - Compare the requested configuration descriptor with the active configuration descriptor
    - Wait for the host library event handler to report a USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS event
    - Free all devices
    - Uninstall USB Host Library
*/
static void host_lib_task(void *arg)
{
    TaskHandle_t pending_task = (TaskHandle_t)arg;
    while (1) {
        // Start handling system events
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            printf("No more clients\n");
            TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, usb_host_device_free_all());
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            printf("All devices freed\n");
            break;
        }
    }
    // Notify the main task that the host library task is done
    xTaskNotifyGive(pending_task);
    printf("Deleting host_lib_task\n");
    vTaskDelete(NULL);
}

TEST_CASE("Test USB Host multiconfig client (single client)", "[usb_host][full_speed][high_speed]")
{
    SemaphoreHandle_t dev_open_smp = xSemaphoreCreateBinary();
    TaskHandle_t client_task;

    multiconf_client_test_param_t multiconf_params = {
        .dev_open_smp = dev_open_smp,
        .bConfigurationValue = B_CONFIGURATION_VALUE,
    };

    xTaskCreatePinnedToCore(multiconf_client_async_task, "async client", 4096, (void *)&multiconf_params, 2, &client_task, 0);
    TEST_ASSERT_NOT_NULL_MESSAGE(client_task, "Failed to create async client task");
    // Start the task
    xTaskNotifyGive(client_task);

    TaskHandle_t host_lib_task_hdl;
    // Get Current task handle
    TaskHandle_t pending_task = xTaskGetCurrentTaskHandle();
    xTaskCreatePinnedToCore(host_lib_task, "host lib", 4096, (void *)pending_task, 2, &host_lib_task_hdl, 0);
    TEST_ASSERT_NOT_NULL_MESSAGE(host_lib_task_hdl, "Failed to create host lib task");

    // Wait for the device to be open
    xSemaphoreTake(dev_open_smp, portMAX_DELAY);
    multiconf_client_get_conf_desc();
    // Wait for the host library task to finish
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // Cleanup
    vSemaphoreDelete(dev_open_smp);
}

/*
Test USB Host Suspend/Resume

Purpose:
- Test that USB Host lib can Suspend and Resume the root port

Procedure:
    - Install USB Host Library
    - Create and start the MSC client task. It will open the device and start handling client events
    - Test root port suspending and resuming during various conditions (No client, transfers submitted, device reconnection.. )
    - Wait for the host library event handler to report a USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS and USB_HOST_LIB_EVENT_FLAGS_ALL_FREE events
    - Free all devices
    - Uninstall USB Host Library
*/

TEST_CASE("Test USB Host suspend/resume", "[usb_host][full_speed][high_speed]")
{
    // Create task to run client that checks the suspend/resume modes
    msc_client_test_param_t params = {
        .num_sectors_to_read = 2,
        .num_sectors_per_xfer = 1,
        .msc_scsi_xfer_tag = TEST_MSC_SCSI_TAG,
    };

    TaskHandle_t task_hdl;
    xTaskCreatePinnedToCore(msc_client_async_suspend_resume_task, "msc", 4096, (void *)&params, 2, &task_hdl, 0);
    // Start the task
    xTaskNotifyGive(task_hdl);

    bool all_clients_gone = false;
    bool all_dev_free = false;
    while (!all_clients_gone || !all_dev_free) {
        // Start handling system events
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            printf("No more clients\n");
            all_clients_gone = true;
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            printf("All device's freed\n");
            all_dev_free = true;
        }
    }
}

/*
Test USB Host Resume by transfer submit

Purpose:
- Test that a transfer is submitted successfully, even if the root port is suspended

Procedure:
    - Install USB Host Library
    - Create and start the MSC client task. It will open the device and start handling client events
    - Suspend the root port and submit transfers (control and non-control) and expect the transfers to be completed
    - Wait for the host library event handler to report a USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS and USB_HOST_LIB_EVENT_FLAGS_ALL_FREE event
    - Free all devices
    - Uninstall USB Host Library
*/

#define TEST_MSC_AUTO_RESUME_TRANSFERS      2

TEST_CASE("Test USB Host resume by submit transfer", "[usb_host][full_speed][high_speed]")
{
    // Create task to run client that communicates with MSC SCSI interface
    msc_client_test_param_t params = {
        .num_sectors_to_read = TEST_MSC_NUM_SECTORS_TOTAL,
        .num_sectors_per_xfer = TEST_MSC_NUM_SECTORS_PER_XFER,
        .msc_scsi_xfer_tag = TEST_MSC_SCSI_TAG,
    };

    TaskHandle_t task_hdl;
    xTaskCreatePinnedToCore(msc_client_async_resume_by_transfer_task, "msc", 4096, (void *)&params, 2, &task_hdl, 0);
    // Start the task
    xTaskNotifyGive(task_hdl);

    while (1) {
        // Start handling system events
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_AUTO_SUSPEND) {
            printf("Auto suspend timer flag\n");
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            printf("No more clients\n");
            TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, usb_host_device_free_all());
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            printf("All device's freed\n");
            break;
        }
    }
}

#define TEST_AUTO_SUSPEND_ITERATIONS   3
#define TEST_AUTO_SUSPEND_PERIOD_MS    1000

static void notif_msc_client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        ESP_LOGI(USB_HOST_ASYNC_TAG, "Client event -> New device with address %d", event_msg->new_dev.address);
        uint8_t *new_device_address = (uint8_t *)arg;
        *new_device_address = event_msg->new_dev.address;
        break;
    case USB_HOST_CLIENT_EVENT_DEV_SUSPENDED:
        ESP_LOGI(USB_HOST_ASYNC_TAG, "Client event -> Device suspended");
        TEST_ASSERT_EQUAL(ESP_ERR_NOT_ALLOWED, usb_host_lib_root_port_suspend());
        TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_resume());
        break;
    case USB_HOST_CLIENT_EVENT_DEV_RESUMED:
        ESP_LOGI(USB_HOST_ASYNC_TAG, "Client event -> Device resumed");
        break;
    default:
        break;
    }
}

static void notif_msc_client(void *args)
{
    uint8_t new_device_address = 0;
    usb_host_client_handle_t client_hdl = NULL;
    usb_device_handle_t dev_hdl = NULL;

    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = notif_msc_client_event_cb,
            .callback_arg = &new_device_address,
        },
    };

    // Register client
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_client_register(&client_config, &client_hdl));
    // Handle new device connection
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_client_handle_events(client_hdl, portMAX_DELAY));
    // Device is connected, check the new device address
    TEST_ASSERT_MESSAGE(new_device_address, "New device address must not be 0");
    // Open the device, to start getting client events
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_device_open(client_hdl, new_device_address, &dev_hdl));

    // Handle client events
    while (!ulTaskNotifyTake(pdTRUE, 0)) {
        usb_host_client_handle_events(client_hdl, 100);
    }

    // Cleanup
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_device_close(client_hdl, dev_hdl));
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_client_deregister(client_hdl));
    vTaskDelete(NULL);
}

/*
Test USB Host auto Suspend with simple MSC client

Purpose:
- Test USB Host lib automatic Suspend functionality

Procedure:
    - Install USB Host Library, register MSC client, open a device and start handling client events
    - Set PM periodic timer to 2000ms, start handling usb host lib events
    - Expect USB_HOST_LIB_EVENT_FLAGS_AUTO_SUSPEND usb host lib flag
    - Check if the root port is in resumed state, suspend the root port
    - Expect USB_HOST_CLIENT_EVENT_DEV_SUSPENDED client event
    - Check if the root port is in suspended state, resume the root port
    - Repeat the 4 above steps TEST_AUTO_SUSPEND_ITERATIONS times
    - Stop handling client events, deregister the client, free all devices, uninstall USB Host library
*/
TEST_CASE("Test USB Host auto suspend", "[usb_host][low_speed][full_speed][high_speed]")
{
    TaskHandle_t client_task_hdl = NULL;

    // Create mcs client task
    TEST_ASSERT_EQUAL(pdTRUE, xTaskCreate(notif_msc_client, "notif_msc", 4096, NULL, 2, &client_task_hdl));
    TEST_ASSERT_NOT_NULL(client_task_hdl);
    // Set PM timer to periodic
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_auto_pm(USB_HOST_LIB_PM_SUSPEND_PERIODIC, TEST_AUTO_SUSPEND_PERIOD_MS));

    int suspend_iter = 0;
    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_AUTO_SUSPEND) {
            printf("Auto Suspend flag\n");
            // Root port is currently resumed, try to resume again and expect error - to verify the resumed state
            TEST_ASSERT_EQUAL(ESP_ERR_NOT_ALLOWED, usb_host_lib_root_port_resume());

            if (++suspend_iter >= TEST_AUTO_SUSPEND_ITERATIONS) {
                // Stop handling the client events, deregister the client
                xTaskNotifyGive(client_task_hdl);
                // Stop the periodic PM timer
                TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_auto_pm(USB_HOST_LIB_PM_SUSPEND_PERIODIC, 0));
                continue;
            }

            // Suspend the root port
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            printf("No more clients\n");
            TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, usb_host_device_free_all());
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            printf("All devices free\n");
            break;
        }
    }
}

#define TEST_SUSPEND_RESUME_TASKS_NUM 4
#define TEST_SUSPEND_RESUME_TASKS_CYCLES 5

static void suspend_resume_task(void *arg)
{
    TaskHandle_t main_task = (TaskHandle_t)arg;
    esp_err_t ret;

    for (int i = 0; i < TEST_SUSPEND_RESUME_TASKS_CYCLES; i++) {
        ret = usb_host_lib_root_port_suspend();
        // ESP_OK: Suspended successfully, or already issuing a suspend command
        // ESP_ERR_NOT_ALLOWED: Root port not in enabled state (already in suspended state)
        TEST_ASSERT(ret == ESP_OK || ret == ESP_ERR_NOT_ALLOWED);
        vTaskDelay(5);         // Let other tasks to run
    }

    vTaskDelay(50); // Keep the device suspended for a while

    for (int i = 0; i < TEST_SUSPEND_RESUME_TASKS_CYCLES; i++) {
        ret = usb_host_lib_root_port_resume();
        // ESP_OK: Resumed successfully, or already issuing resume command
        // ESP_ERR_NOT_ALLOWED: Root port not in suspended state (already in enabled state)
        TEST_ASSERT(ret == ESP_OK || ret == ESP_ERR_NOT_ALLOWED);
        vTaskDelay(5);          // Let other tasks to run
    }

    // Signalize to the main task, that the test has finished and delete the task
    xTaskNotifyGive(main_task);
    vTaskDelete(NULL);
}

/*
Test suspend/resume calls from multiple tasks

Purpose:
- Test concurrency robustness of root port suspend and resume commands

Procedure:
    - Install USB Host Library
    - Suspend and Resume the root port from multiple tasks
    - Teardown
*/
TEST_CASE("Test USB Host suspend/resume multiple tasks access (no client)", "[usb_host][low_speed][full_speed][high_speed]")
{
    TaskHandle_t host_lib_task_hdl = NULL;
    // Get Current task handle
    TaskHandle_t main_task_hdl = xTaskGetCurrentTaskHandle();
    TEST_ASSERT_EQUAL(pdTRUE, xTaskCreate(host_lib_task, "host lib", 4096, (void *)main_task_hdl, 2, &host_lib_task_hdl));
    TEST_ASSERT_NOT_NULL_MESSAGE(host_lib_task_hdl, "Failed to create host lib task");

    // Some time for the device to enumerate
    vTaskDelay(100);

    // Create multiple tasks
    for (int i = 0; i < TEST_SUSPEND_RESUME_TASKS_NUM; i++) {
        TaskHandle_t suspend_task_hdl = NULL;
#if CONFIG_FREERTOS_UNICORE
        TEST_ASSERT_EQUAL(pdTRUE, xTaskCreate(suspend_resume_task, "suspend_resume_task", 4096, (void *)main_task_hdl, 3 + i, &suspend_task_hdl));
#else
        TEST_ASSERT_EQUAL(pdTRUE, xTaskCreatePinnedToCore(suspend_resume_task, "suspend_resume_task", 4096, (void *)main_task_hdl, 3 + i, &suspend_task_hdl, i % 2));
#endif // CONFIG_FREERTOS_UNICORE
        TEST_ASSERT_NOT_NULL_MESSAGE(suspend_task_hdl, "Failed to create suspend/resume task");
    }

    // Wait for all tasks to finish
    vTaskDelay(pdMS_TO_TICKS(2000));
    TEST_ASSERT_EQUAL_MESSAGE(TEST_SUSPEND_RESUME_TASKS_NUM, ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100)), "One of the suspend tasks did not complete");

    // Free the device
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, usb_host_device_free_all());
    // Wait for the host library task to finish
    TEST_ASSERT_MESSAGE(ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(1000)), "usb host lib task did not finish");
}
