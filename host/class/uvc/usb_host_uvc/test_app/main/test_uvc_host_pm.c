/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "soc/soc_caps.h"
#if SOC_USB_OTG_SUPPORTED

#include <stdio.h>
#include <string.h>
#include "esp_system.h"
#include "unity.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_private/usb_phy.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"
#include "esp_private/uvc_esp_video.h"

#ifdef UVC_HOST_SUSPEND_RESUME_API_SUPPORTED

#define TEST_EVENT_WAIT_MS          500     // Time to expect driver or device event
#define TEST_SUPEND_RESUME_HOLD_MS  1000    // Time to keep the device suspended or resumed
#define START_BIT (1 << 0)

static const char *TAG = "UVC test app";
static QueueHandle_t frames_queue = NULL;
static QueueHandle_t event_queue = NULL;

/**
 * @brief Event group type
 */
enum test_uvc_host_evt_type {
    UVC_HOST_DRIVER_EVENT,          /**< UVC Driver event type */
    UVC_HOST_DEVICE_EVENT,          /**< UVC Device event type */
};

/**
 * @brief Test event queue
 */
typedef struct {
    enum test_uvc_host_evt_type event_type;
    union {
        enum uvc_host_driver_event driver_event;
        enum uvc_host_dev_event device_event;
    };
} test_uvc_event_t;

/**
 * @brief UVC Driver callback
 *
 * Used to process device connection event
 */
static void driver_callback(const uvc_host_driver_event_data_t *event, void *user_ctx)
{
    switch (event->type) {
    case UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED:
        ESP_LOGI(TAG, "Device addr %d connected", event->device_connected.dev_addr);
        break;
    default:
        TEST_FAIL_MESSAGE("Unrecognized driver event");
        break;
    }

    if (event_queue) {
        const test_uvc_event_t test_uvc_event = {
            .event_type = UVC_HOST_DRIVER_EVENT,
            .driver_event = event->type,
        };
        TEST_ASSERT_EQUAL(pdTRUE, xQueueSend(event_queue, &test_uvc_event, 0));
    }
}

static void usb_lib_task_pm(void *arg)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_install(&host_config));
    printf("USB Host installed\n");
    xTaskNotifyGive(arg);

    bool has_clients = true;
    bool has_devices = false;
    while (has_clients) {
        uint32_t event_flags;
        ESP_ERROR_CHECK(usb_host_lib_handle_events(portMAX_DELAY, &event_flags));
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            printf("Get FLAGS_NO_CLIENTS\n");
            if (ESP_OK == usb_host_device_free_all()) {
                printf("All devices marked as free, no need to wait FLAGS_ALL_FREE event\n");
                has_clients = false;
            } else {
                printf("Wait for the FLAGS_ALL_FREE\n");
                has_devices = true;
            }
        }
        if (has_devices && event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            printf("Get FLAGS_ALL_FREE\n");
            has_clients = false;
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_AUTO_SUSPEND) {
            printf("Get FLAGS_AUTO_SUSPEND\n");
            TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());
        }
    }
    printf("No more clients and devices, uninstall USB Host library\n");

    // Clean up USB Host
    vTaskDelay(10); // Short delay to allow clients clean-up
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_uninstall());
    vTaskDelete(NULL);
}

/**
 * @brief Install UVC driver
 *
 * This function installs prerequisites for many tests: USB Host library and UVC driver.
 * It also creates a task that handles USB library events and creates frame and event queues
 */
static void test_install_uvc_driver_pm(void)
{
    // Create a task that will handle USB library events
    TEST_ASSERT_EQUAL(pdTRUE, xTaskCreatePinnedToCore(usb_lib_task_pm, "usb_lib", 4 * 4096, xTaskGetCurrentTaskHandle(), 10, NULL, 0));
    ulTaskNotifyTake(false, 1000);

    printf("Installing UVC driver\n");
    const uvc_host_driver_config_t uvc_driver_config = {
        .driver_task_stack_size = 4 * 1024,
        .driver_task_priority = 10,
        .xCoreID = tskNO_AFFINITY,
        .create_background_task = true,
        .event_cb = driver_callback,
        .user_ctx = NULL,
    };
    TEST_ASSERT_EQUAL(ESP_OK, uvc_host_install(&uvc_driver_config));

    // Create frame queue and event queue
    frames_queue = xQueueCreate(5, sizeof(uvc_host_frame_t *));
    event_queue = xQueueCreate(3, sizeof(test_uvc_event_t));
    TEST_ASSERT_NOT_NULL(frames_queue);
    TEST_ASSERT_NOT_NULL(event_queue);
}

/**
 * @brief Expect USB Host client event
 *
 * @param[in] expected_event_data  Expected client event (NULL to not expect any event)
 * @param[in] ticks  Ticks to wait
 */
static void expect_client_event(test_uvc_event_t *expected_event_data, TickType_t ticks)
{
    test_uvc_event_t event_data;

    // Check, that no event is delivered
    if (expected_event_data == NULL) {
        if (pdFALSE == xQueueReceive(event_queue, &event_data, ticks)) {
            return;
        } else {
            TEST_FAIL_MESSAGE("Expecting NO event, but an event delivered");
        }
    }

    // Check that an event is delivered
    if (pdTRUE == xQueueReceive(event_queue, &event_data, ticks)) {
        TEST_ASSERT_EQUAL_MESSAGE(expected_event_data->event_type, event_data.event_type, "Unexpected event type");
        if (event_data.event_type == UVC_HOST_DRIVER_EVENT) {
            TEST_ASSERT_EQUAL_MESSAGE(expected_event_data->driver_event, event_data.driver_event, "Unexpected driver event");
        } else {
            TEST_ASSERT_EQUAL_MESSAGE(expected_event_data->device_event, event_data.device_event, "Unexpected device event");
        }
    } else {
        TEST_FAIL_MESSAGE("App event not generated on time");
    }
}

static bool frame_callback(const uvc_host_frame_t *frame, void *user_ctx)
{
    assert(frame);
    assert(user_ctx);
    QueueHandle_t frame_q = *((QueueHandle_t *)user_ctx);
    ESP_LOGD(TAG, "Frame cb data len: %d", frame->data_len);
    BaseType_t result = xQueueSendToBack(frame_q, &frame, 0);
    if (pdPASS != result) {
        TEST_FAIL_MESSAGE("Queue full, losing frame");
    }
    return false; // We only passed the frame to Queue, so we must return false and call uvc_host_frame_return() later
}

/**
 * @brief UVC Stream callback
 * Used to process device events
 */
static void stream_callback(const uvc_host_stream_event_data_t *event, void *user_ctx)
{
    switch (event->type) {
    case UVC_HOST_TRANSFER_ERROR:
        ESP_LOGE(TAG, "USB error has occurred, err_no = %i", event->transfer_error.error);
        break;
    case UVC_HOST_DEVICE_DISCONNECTED:
        ESP_LOGI(TAG, "Device suddenly disconnected");
        ESP_ERROR_CHECK(uvc_host_stream_close(event->device_disconnected.stream_hdl));
        break;
    case UVC_HOST_FRAME_BUFFER_OVERFLOW:
        ESP_LOGW(TAG, "Frame buffer overflow");
        break;
    case UVC_HOST_FRAME_BUFFER_UNDERFLOW:
        ESP_LOGW(TAG, "Frame buffer underflow");
        break;
    case UVC_HOST_DEVICE_SUSPENDED:
        ESP_LOGI(TAG, "Device suspended");
        break;
    case UVC_HOST_DEVICE_RESUMED:
        ESP_LOGI(TAG, "Device resumed");
        break;
    default:
        TEST_FAIL_MESSAGE("Unrecognized device event");
        break;
    }

    if (event_queue) {
        const test_uvc_event_t test_uvc_event = {
            .event_type = UVC_HOST_DEVICE_EVENT,
            .device_event = event->type,
        };
        TEST_ASSERT_EQUAL(pdTRUE, xQueueSend(event_queue, &test_uvc_event, 0));
    }
}

/**
 * @brief Default stream config used in all tests
 */
static uvc_host_stream_config_t default_stream_config = {
    .event_cb = stream_callback,
    .frame_cb = NULL,
    .user_ctx = NULL,
    .usb = {
        .dev_addr = UVC_HOST_ANY_DEV_ADDR,
        .vid = UVC_HOST_ANY_VID,
        .pid = UVC_HOST_ANY_PID,
        .uvc_stream_index = 0,
    },
    .vs_format = {
        .h_res = 640,
        .v_res = 480,
        .fps = 0, // Default FPS
        .format = UVC_VS_FORMAT_MJPEG,
    },
    .advanced = {
        .number_of_frame_buffers = 3,
        .frame_size = 0,
        .frame_heap_caps = 0,
        .number_of_urbs = 4,
        .urb_size = 10 * 1024,
    },
};

/**
 * @brief Test teardown
 * UVC Host uninstall and queues deletion
 */
static void test_teardown(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, uvc_host_uninstall());
    vQueueDelete(frames_queue);
    vQueueDelete(event_queue);
    frames_queue = NULL;
    event_queue = NULL;
    vTaskDelay(20); // Short delay to allow task to be cleaned up
}

static void test_device_connect_open(uvc_host_stream_hdl_t *stream_ret)
{
    // Wait for the device connection event
    test_uvc_event_t dev_conn_evt = {.event_type = UVC_HOST_DRIVER_EVENT, .driver_event = UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED};
    expect_client_event(&dev_conn_evt, pdMS_TO_TICKS(TEST_EVENT_WAIT_MS));

    // Open stream
    const uvc_host_stream_config_t stream_config = default_stream_config;
    uvc_host_stream_hdl_t stream = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, uvc_host_stream_open(&stream_config, 1000, &stream));
    TEST_ASSERT_NOT_NULL(stream);
    *stream_ret = stream;
    vTaskDelay(10);
}

/**
 * @brief Basic suspend/resume events
 *
 * -# Wait for device connection and opend the device
 * -# Suspend the root port and check that the suspend event is delivered
 * -# Resume the root port and check that the resume event is delivered
 * -# Close the device, test cleanup
 */
TEST_CASE("Basic suspend/resume", "[uvc]")
{
    test_install_uvc_driver_pm();

    uvc_host_stream_hdl_t stream = NULL;
    test_device_connect_open(&stream);
    TEST_ASSERT_NOT_NULL(stream);

    // Suspend the root port and wait for the suspend event
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());
    test_uvc_event_t expected_event = {.event_type = UVC_HOST_DEVICE_EVENT, .device_event = UVC_HOST_DEVICE_SUSPENDED};
    expect_client_event(&expected_event, pdMS_TO_TICKS(TEST_EVENT_WAIT_MS));

    // Resume the root port and wait for the resume event
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_resume());
    expected_event.device_event = UVC_HOST_DEVICE_RESUMED;
    expect_client_event(&expected_event, pdMS_TO_TICKS(TEST_EVENT_WAIT_MS));

    // Close the device, cleanup
    TEST_ASSERT_EQUAL(ESP_OK, uvc_host_stream_close(stream));
    test_teardown();
}

/**
 * @brief Sudden disconnect while root port suspended
 *
 * -# Wait for device connection and opend the device
 * -# Suspend the root port and check that the suspend event is delivered
 * -# Disconnect the device and check that the disconnection event is delivered
 * -# Close the device, test cleanup
 */
TEST_CASE("Suspend sudden disconnect", "[uvc]")
{
    test_install_uvc_driver_pm();

    uvc_host_stream_hdl_t stream = NULL;
    test_device_connect_open(&stream);
    TEST_ASSERT_NOT_NULL(stream);

    // Suspend the root port and wait for the suspend event
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());
    test_uvc_event_t expected_event = {.event_type = UVC_HOST_DEVICE_EVENT, .device_event = UVC_HOST_DEVICE_SUSPENDED};
    expect_client_event(&expected_event, pdMS_TO_TICKS(TEST_EVENT_WAIT_MS));

    // Disconnect the device and wait for the disconnection event
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_root_port_power(false));
    expected_event.device_event = UVC_HOST_DEVICE_DISCONNECTED;
    expect_client_event(&expected_event, pdMS_TO_TICKS(TEST_EVENT_WAIT_MS));

    // Try to resume the root port after the sudden disconnect
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, usb_host_lib_root_port_resume());

    // Close the device
    TEST_ASSERT_EQUAL(ESP_OK, uvc_host_stream_close(stream));
    test_teardown();
}

/**
 * @brief Notification events used to control the frame processing task from the test case
 */
enum test_notify_event {
    TEST_NOTIFY_STREAM_START,       /**< Start the stream */
    TEST_NOTIFY_STREAM_STOP,        /**< Stop the stream */
    TEST_NOTIFY_DEVICE_SUSPENDED,   /**< Device suspended, stop expecting new frames */
    TEST_NOTIFY_STREAM_CLOSE,       /**< Close the stream */
};

/**
 * @brief Frame handling task
 *
 * Open and start video stream, receive frames from frame queue
 * Processing task is controlled from the main task by test_notify_event
 */
static void test_fame_handling_task(void *args)
{
    // Get stream config and open stream
    TaskHandle_t main_task_hdl = (TaskHandle_t)args;
    uvc_host_stream_hdl_t uvc_stream = NULL;
    uvc_host_stream_config_t stream_config = default_stream_config;
    stream_config.frame_cb = frame_callback;
    stream_config.user_ctx = &frames_queue;

    TEST_ASSERT_EQUAL(ESP_OK, uvc_host_stream_open(&stream_config, 1000, &uvc_stream));
    TEST_ASSERT_NOT_NULL(uvc_stream);
    vTaskDelay(10);

    while (!ulTaskNotifyTakeIndexed(TEST_NOTIFY_STREAM_CLOSE, pdTRUE, 0)) {

        // Wait for notification from the main task to start stream
        TEST_ASSERT_EQUAL(pdTRUE, ulTaskNotifyTakeIndexed(TEST_NOTIFY_STREAM_START, pdTRUE, pdMS_TO_TICKS(2000)));
        TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, uvc_host_stream_start(uvc_stream), "Stream not started");

        // Start receiving frames, until the main task does not stop it
        while (!ulTaskNotifyTakeIndexed(TEST_NOTIFY_STREAM_STOP, pdTRUE, 0)) {
            uvc_host_frame_t *frame;
            if (xQueueReceive(frames_queue, &frame, pdMS_TO_TICKS(1000)) == pdPASS) {
                ESP_LOGI(TAG, "New frame %dx%d! Len: %d", frame->vs_format.h_res, frame->vs_format.v_res, frame->data_len);

                TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, uvc_host_frame_return(uvc_stream, frame), "Frame return error");
            } else {
                if ( pdTRUE == ulTaskNotifyTakeIndexed(TEST_NOTIFY_DEVICE_SUSPENDED, pdTRUE, 0)) {
                    // Device was suspended while getting frames, exit loop from here
                    break;
                } else {
                    TEST_FAIL_MESSAGE("Frame not received on time");
                }
            }
        }
    }

    // Cleanup
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, uvc_host_stream_stop(uvc_stream), "Stream can't be stopped");
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, uvc_host_stream_close(uvc_stream), "Stream can't be closed");
    xTaskNotifyGive(main_task_hdl);
    vTaskDelete(NULL);
}

/**
 * @brief Suspend/Resume while receiving frames
 *
 * -# Wait for device connection, opend the device create frame handling task
 * -# Start video stream and receive frames in the frame handling task
 * -# Suspend the root port while receiving frames and check that the suspend event is delivered
 * -# Resume the root port and check that the resume event is delivered
 * -# Repeat the 3 above steps
 * -# Disconnect the device and check that the disconnection event is delivered
 * -# Close the device, test cleanup
 */
TEST_CASE("Suspend/Resume while streaming", "[uvc]")
{
    test_install_uvc_driver_pm();

    // Wait for the device connection event
    test_uvc_event_t dev_conn_evt = {.event_type = UVC_HOST_DRIVER_EVENT, .driver_event = UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED};
    expect_client_event(&dev_conn_evt, pdMS_TO_TICKS(TEST_EVENT_WAIT_MS));

    // Create frame processing task
    TaskHandle_t frame_handling_task_hdl = NULL;
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(test_fame_handling_task, "frame_handing", 4096, (void *)xTaskGetCurrentTaskHandle(), 4, &frame_handling_task_hdl));
    TEST_ASSERT_NOT_NULL(frame_handling_task_hdl);

    // Start the stream -> suspend the root port while streaming -> resume the root port -> Start the stream
    for (int i = 0; i < 5; i++) {

        // Start the stream and keep it running for a while
        xTaskNotifyGiveIndexed(frame_handling_task_hdl, TEST_NOTIFY_STREAM_START);
        vTaskDelay(pdMS_TO_TICKS(TEST_SUPEND_RESUME_HOLD_MS * 2));

        // Suspend the root port and wait for the suspend event
        TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());
        test_uvc_event_t expected_event = {.event_type = UVC_HOST_DEVICE_EVENT, .device_event = UVC_HOST_DEVICE_SUSPENDED};
        expect_client_event(&expected_event, pdMS_TO_TICKS(TEST_EVENT_WAIT_MS));

        // Notify the frame processing task, that the device is suspended, to stop expecting new frames from frame queue
        xTaskNotifyGiveIndexed(frame_handling_task_hdl, TEST_NOTIFY_DEVICE_SUSPENDED);

        // Keep the device suspended for a while
        vTaskDelay(pdMS_TO_TICKS(TEST_SUPEND_RESUME_HOLD_MS));

        // Resume the root port and wait for the resume event
        TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_resume());
        expected_event.device_event = UVC_HOST_DEVICE_RESUMED;
        expect_client_event(&expected_event, pdMS_TO_TICKS(TEST_EVENT_WAIT_MS));
    }

    // Start the stream again and keep it running for a while
    xTaskNotifyGiveIndexed(frame_handling_task_hdl, TEST_NOTIFY_STREAM_START);
    vTaskDelay(pdMS_TO_TICKS(TEST_SUPEND_RESUME_HOLD_MS));

    // Stop and close the stream
    xTaskNotifyGiveIndexed(frame_handling_task_hdl, TEST_NOTIFY_STREAM_STOP);
    xTaskNotifyGiveIndexed(frame_handling_task_hdl, TEST_NOTIFY_STREAM_CLOSE);

    // Wait for the cleanup of the frame processing task
    TEST_ASSERT_EQUAL_MESSAGE(pdTRUE, ulTaskNotifyTake(true, pdMS_TO_TICKS(2000)), "Frame processing task was not deleted");
    vTaskDelay(10);
    test_teardown();
}

/**
 * @brief Suspend/Resume while receiving frames
 *
 * -# Wait for device connection, opend the device create frame handling task
 * -# Start video stream
 * -# Receive frames in the frame handling task
 * -# Suspend the root port while receiving frames and check that the suspend event is delivered
 * -# Start the stream, which automatically resumes the root port, check that the resume event is delivered
 * -# Repeat the 3 above steps
 * -# Disconnect the device and check that the disconnection event is delivered
 * -# Close the device, test cleanup
 */
TEST_CASE("Resume by transfer submit", "[uvc]")
{
    test_install_uvc_driver_pm();

    // Wait for the device connection event
    test_uvc_event_t dev_conn_evt = {.event_type = UVC_HOST_DRIVER_EVENT, .driver_event = UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED};
    expect_client_event(&dev_conn_evt, pdMS_TO_TICKS(TEST_EVENT_WAIT_MS));

    // Create frame processing task
    TaskHandle_t frame_handling_task_hdl = NULL;
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(test_fame_handling_task, "frame_handing", 4096, (void *)xTaskGetCurrentTaskHandle(), 4, &frame_handling_task_hdl));
    TEST_ASSERT_NOT_NULL(frame_handling_task_hdl);

    // Start the stream
    xTaskNotifyGiveIndexed(frame_handling_task_hdl, TEST_NOTIFY_STREAM_START);

    // Start the stream -> suspend the root port while streaming -> resume the root port by transfer submit -> Start the stream
    for (int i = 0; i < 5; i++) {

        // Keep the stream running for a while
        vTaskDelay(pdMS_TO_TICKS(TEST_SUPEND_RESUME_HOLD_MS * 2));

        // Suspend the root port and wait for the suspend event
        TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());
        test_uvc_event_t expected_event = {.event_type = UVC_HOST_DEVICE_EVENT, .device_event = UVC_HOST_DEVICE_SUSPENDED};
        expect_client_event(&expected_event, pdMS_TO_TICKS(TEST_EVENT_WAIT_MS));

        // Notify the frame processing task, that the device is suspended, to stop expecting new frames from frame queue
        xTaskNotifyGiveIndexed(frame_handling_task_hdl, TEST_NOTIFY_DEVICE_SUSPENDED);

        // Keep the device suspended for a while
        vTaskDelay(pdMS_TO_TICKS(TEST_SUPEND_RESUME_HOLD_MS));

        // Start the stream again and keep it running for a while
        // uvc_host_stream_start() submits a ctrl transfer, which automatically resumes the root port
        xTaskNotifyGiveIndexed(frame_handling_task_hdl, TEST_NOTIFY_STREAM_START);

        // Expect resumed event triggered by stream start
        expected_event.device_event = UVC_HOST_DEVICE_RESUMED;
        expect_client_event(&expected_event, pdMS_TO_TICKS(TEST_EVENT_WAIT_MS));
    }

    // Keep the stream running for a while
    vTaskDelay(pdMS_TO_TICKS(TEST_SUPEND_RESUME_HOLD_MS));

    // Stop and close the stream
    xTaskNotifyGiveIndexed(frame_handling_task_hdl, TEST_NOTIFY_STREAM_STOP);
    xTaskNotifyGiveIndexed(frame_handling_task_hdl, TEST_NOTIFY_STREAM_CLOSE);

    // Wait for the cleanup of the frame processing task
    TEST_ASSERT_EQUAL_MESSAGE(pdTRUE, ulTaskNotifyTake(true, pdMS_TO_TICKS(2000)), "Frame processing task was not deleted");
    vTaskDelay(10);
    test_teardown();
}

/**
 * @brief Close suspended device
 *
 * -# Wait for device connection, opend the device
 * -# Suspend the root port and check that the suspend event is delivered
 * -# Close the device, test cleanup
 */
TEST_CASE("Device close while suspended", "[uvc]")
{
    test_install_uvc_driver_pm();

    // Connect device and open stream
    uvc_host_stream_hdl_t stream = NULL;
    test_device_connect_open(&stream);
    TEST_ASSERT_NOT_NULL(stream);

    // Suspend the root port and wait for the suspend event
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());
    test_uvc_event_t expected_event = {.event_type = UVC_HOST_DEVICE_EVENT, .device_event = UVC_HOST_DEVICE_SUSPENDED};
    expect_client_event(&expected_event, pdMS_TO_TICKS(TEST_EVENT_WAIT_MS));

    // Close the device while being suspended
    TEST_ASSERT_EQUAL(ESP_OK, uvc_host_stream_close(stream));

    // Try to open still suspended device
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, uvc_host_stream_open(&default_stream_config, 100, &stream));
    test_teardown();
}

/**
 * @brief Suspend closed device
 *
 * -# Wait for device connection, opend the device and immediately close it
 * -# Suspend the root port and check that no suspend event is delivered
 * -# Test cleanup
 */
TEST_CASE("Suspend closed device", "[uvc]")
{
    test_install_uvc_driver_pm();

    // Connect device and open stream
    uvc_host_stream_hdl_t stream = NULL;
    test_device_connect_open(&stream);
    TEST_ASSERT_NOT_NULL(stream);

    // Close the device
    TEST_ASSERT_EQUAL(ESP_OK, uvc_host_stream_close(stream));

    // Suspend the root port and expect no suspend event
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());
    expect_client_event(NULL, pdMS_TO_TICKS(2000));

    test_teardown();
}

/**
 * @brief Suspend closed device
 *
 * -# Wait for device connection and connection event
 * -# Suspend the root port and check that no suspend event is delivered (no device is opened by the driver)
 * -# Try to open device, while the root port is suspended
 * -# Resume the root port and check that no resume event is delivered (no device is opened by the driver)
 * -# Open and close the resumed device
 * -# Test cleanup
 */
TEST_CASE("Device open while suspended", "[uvc]")
{
    test_install_uvc_driver_pm();

    // Expect device connect event
    test_uvc_event_t dev_conn_evt = {.event_type = UVC_HOST_DRIVER_EVENT, .driver_event = UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED};
    expect_client_event(&dev_conn_evt, pdMS_TO_TICKS(500));

    // Suspend the root port, but do not expect any event, since the device was never opened
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());
    expect_client_event(NULL, 100);

    uvc_host_stream_hdl_t stream = NULL;
    const uvc_host_stream_config_t stream_config = default_stream_config;

    // Try to open the suspended device
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, uvc_host_stream_open(&stream_config, 100, &stream));
    TEST_ASSERT_NULL(stream);

    // Resume the root port, but do not expect any event, since the device was never opened
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_resume());
    expect_client_event(NULL, 100);

    // Open the resumed device
    TEST_ASSERT_EQUAL(ESP_OK, uvc_host_stream_open(&stream_config, 1000, &stream));
    TEST_ASSERT_NOT_NULL(stream);

    // Close the device
    TEST_ASSERT_EQUAL(ESP_OK, uvc_host_stream_close(stream));
    test_teardown();
}

#if CONFIG_FREERTOS_UNICORE
#define CORE_FOR_SUSPEND    tskNO_AFFINITY
#define CORE_FOR_DISCONNECT tskNO_AFFINITY
#else
#define CORE_FOR_SUSPEND    1
#define CORE_FOR_DISCONNECT 0
#endif

/**
 * @brief Only suspends the root port
 */
static void suspend_task(void *arg)
{
    EventGroupHandle_t start_event = (EventGroupHandle_t)arg;
    xEventGroupWaitBits(start_event, START_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    // Suspending procedure shall be started, but not finished
    TEST_ASSERT_NOT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());
    vTaskDelete(NULL);
}

/**
 * @brief Only disconnects the device
 */
static void disconnect_task(void *arg)
{
    EventGroupHandle_t start_event = (EventGroupHandle_t)arg;
    xEventGroupWaitBits(start_event, START_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    // Device shall be disconnected
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_root_port_power(false));
    vTaskDelete(NULL);
}

/**
 * @brief Multiple task access suspend and disconnect
 *
 * -# Wait for device connection and connection event, open stream
 * -# Create suspend task, which suspends the root port
 * -# Create disconnect task, which disconnects the device
 * -# Start the tasks simultaneously
 * -# Expect only disconnection event
 * -# Close device, test cleanup
 */
TEST_CASE("Multiple tasks access suspend/disconnect", "[uvc]")
{
    test_install_uvc_driver_pm();

    // Connect device and open stream
    uvc_host_stream_hdl_t stream = NULL;
    test_device_connect_open(&stream);
    TEST_ASSERT_NOT_NULL(stream);

    // Create suspend task and disconnect task od different cores
    EventGroupHandle_t start_event;
    start_event = xEventGroupCreate();
    TEST_ASSERT_NOT_NULL(start_event);

    TaskHandle_t suspend_task_hld = NULL, disconnect_task_hdl = NULL;
    TEST_ASSERT_EQUAL(pdTRUE, xTaskCreatePinnedToCore(suspend_task, "suspend_task", 4096, (void *)start_event, 2, &suspend_task_hld, CORE_FOR_SUSPEND));
    TEST_ASSERT_EQUAL(pdTRUE, xTaskCreatePinnedToCore(disconnect_task, "disconnect_task", 4096, (void *)start_event, 2, &disconnect_task_hdl, CORE_FOR_DISCONNECT));
    TEST_ASSERT_NOT_NULL(suspend_task_hld);
    TEST_ASSERT_NOT_NULL(disconnect_task_hdl);

    // Start both tasks
    xEventGroupSetBits(start_event, START_BIT);

    // Only device disconnection event shall be delivered
    test_uvc_event_t expected_event = {.event_type = UVC_HOST_DEVICE_EVENT, .device_event = UVC_HOST_DEVICE_DISCONNECTED};
    expect_client_event(&expected_event, pdMS_TO_TICKS(TEST_EVENT_WAIT_MS));
    expect_client_event(NULL, pdMS_TO_TICKS(TEST_EVENT_WAIT_MS));

    TEST_ASSERT_EQUAL(ESP_OK, uvc_host_stream_close(stream));
    vEventGroupDelete(start_event);
    test_teardown();
}


#define TEST_UVC_SUSPEND_TIMER_INTERVAL_MS   1000
#define TEST_UVC_SUSPEND_TIMER_MARGIN_MS     50

/**
 * @brief Automatic suspend timer
 *
 * -# Wait for device connection and connection event, open stream
 * -# Set auto suspend timer to one-shot timer and expect suspend event within the set interval
 * -# Resume the root port and check that resume event is delivered
 * -# Set auto suspend timer to periodic timer and expect suspend events periodically
 * -# Stop periodic timer
 * -# Close device, test cleanup
 */
TEST_CASE("Automatic suspend timer", "[uvc]")
{
    test_install_uvc_driver_pm();

    // Connect device and open stream
    uvc_host_stream_hdl_t stream = NULL;
    test_device_connect_open(&stream);
    TEST_ASSERT_NOT_NULL(stream);

    // Set one-shot suspend timer and expect suspend event within the suspend timer interval
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_auto_suspend(USB_HOST_LIB_AUTO_SUSPEND_ONE_SHOT, TEST_UVC_SUSPEND_TIMER_INTERVAL_MS));
    test_uvc_event_t expected_event = {.event_type = UVC_HOST_DEVICE_EVENT, .device_event = UVC_HOST_DEVICE_SUSPENDED};
    expect_client_event(&expected_event, pdMS_TO_TICKS(TEST_UVC_SUSPEND_TIMER_INTERVAL_MS + TEST_UVC_SUSPEND_TIMER_MARGIN_MS));

    // Manually resume the root port and wait for the resume event
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_resume());
    expected_event.device_event = UVC_HOST_DEVICE_RESUMED;
    expect_client_event(&expected_event, pdMS_TO_TICKS(500));

    // Make sure no other event is delivered,
    // since the timer is a One-Shot timer and it shall not automatically suspend the root port again
    expect_client_event(NULL, pdMS_TO_TICKS(TEST_UVC_SUSPEND_TIMER_INTERVAL_MS * 2));

    // Set Periodic auto suspend timer
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_auto_suspend(USB_HOST_LIB_AUTO_SUSPEND_PERIODIC, TEST_UVC_SUSPEND_TIMER_INTERVAL_MS));

    for (int i = 0; i < 3; i++) {
        // Expect suspended event from auto suspend timer
        expected_event.device_event = UVC_HOST_DEVICE_SUSPENDED;
        expect_client_event(&expected_event, pdMS_TO_TICKS(TEST_UVC_SUSPEND_TIMER_INTERVAL_MS + TEST_UVC_SUSPEND_TIMER_MARGIN_MS));

        // Resume the root port manually and expect the resume event
        TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_resume());
        expected_event.device_event = UVC_HOST_DEVICE_RESUMED;
        expect_client_event(&expected_event, pdMS_TO_TICKS(500));

        // Verify data transmit on resumed device
        // Change the format
        uvc_host_stream_format_t format2 = {
            .h_res = 640,
            .v_res = 480,
            .fps = 30,
            .format = UVC_VS_FORMAT_MJPEG,
        };
        TEST_ASSERT_EQUAL(ESP_OK, uvc_host_stream_format_select(stream, &format2));
    }

    // Disable the Periodic auto suspend timer
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_auto_suspend(USB_HOST_LIB_AUTO_SUSPEND_PERIODIC, 0));
    // Make sure no other event is delivered
    expect_client_event(NULL, pdMS_TO_TICKS(TEST_UVC_SUSPEND_TIMER_INTERVAL_MS * 2));

    // Close the device, cleanup
    TEST_ASSERT_EQUAL(ESP_OK, uvc_host_stream_close(stream));
    test_teardown();
}

#endif // UVC_HOST_SUSPEND_RESUME_API_SUPPORTED

#endif // SOC_USB_OTG_SUPPORTED
