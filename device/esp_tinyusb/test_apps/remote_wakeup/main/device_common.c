/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "soc/soc_caps.h"

#if SOC_USB_OTG_SUPPORTED
//
#include <stdio.h>
#include <string.h>
//
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
//
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
//
#include "unity.h"
#include "tinyusb.h"
#include "device_common.h"

typedef struct {
    union {
        tinyusb_event_id_t tinyusb_event_id;  /*!< Event ID, comes from tinyusb.h */
        tinyusb_extra_event_id_t extra_event_id;  /*!< Extra Event ID, extra, that isn't handled inside the driver */
        int id;                             /*!< Common field to get the data of event_id */
    };
} test_device_event_t;

#define TEST_QUEUE_LEN                  6       // Length of the queue for storage events
#define TEST_EVENT_TIMEOUT_MS           30000   // Timeout for waiting storage events

static QueueHandle_t _test_device_event_queue = NULL;

void test_device_setup(void)
{
    _test_device_event_queue = xQueueCreate(TEST_QUEUE_LEN, sizeof(test_device_event_t));
    TEST_ASSERT_NOT_NULL(_test_device_event_queue);
}

void test_device_teardown(void)
{
    TEST_ASSERT_NOT_NULL(_test_device_event_queue);
    vQueueDelete(_test_device_event_queue);
    _test_device_event_queue = NULL;
}

void test_device_wait_event(int event_id)
{
    TEST_ASSERT_NOT_NULL(_test_device_event_queue);
    // Wait for port callback to send an event message
    test_device_event_t evt;
    BaseType_t ret = xQueueReceive(_test_device_event_queue, &evt, pdMS_TO_TICKS(TEST_EVENT_TIMEOUT_MS));
    TEST_ASSERT_EQUAL_MESSAGE(pdPASS, ret, "Device event not generated on time");
    // Check the contents of that event message
    TEST_ASSERT_EQUAL_MESSAGE(event_id, evt.id, "Unexpected device event type received");
}

/**
 * @brief TinyUSB callback for device mount.
 *
 * @note
 * For Linux-based Hosts: Reflects the SetConfiguration() request from the Host Driver.
 * For Win-based Hosts: SetConfiguration() request is present only with available Class in device descriptor.
 */
void test_device_event_handler(tinyusb_event_t *event, void *arg)
{
    test_device_event_t evt = {
        .tinyusb_event_id = event->id,
    };
    xQueueSend(_test_device_event_queue, &evt, portMAX_DELAY);
}

// Some events are still not handled inside the TinyUSB driver, so we need to forward them manually
/**
 * @brief TinyUSB callback for device suspend event.
 *
 * This function is called when the USB host suspends the device.
 *
 * @param remote_wakeup_en Indicates whether the host has enabled remote wakeup for the device.
 *        If true, the device is permitted to signal the host to resume communication.
 */
void tud_suspend_cb(bool remote_wakeup_en)
{
    printf("USB Host suspended the device, remote wakeup enabled: %s\n", remote_wakeup_en ? "true" : "false");

    test_device_event_t evt = {
        .extra_event_id = TINYUSB_EVENT_SUSPENDED,
    };
    xQueueSend(_test_device_event_queue, &evt, portMAX_DELAY);
}

/**
 * @brief TinyUSB callback for device resume event.
 *
 * This function is called by the TinyUSB stack when the USB host resumes the device
 * from a suspended state. It forwards the resume event to the test event queue.
 */
void tud_resume_cb(void)
{
    test_device_event_t evt = {
        .extra_event_id = TINYUSB_EVENT_RESUMED,
    };
    xQueueSend(_test_device_event_queue, &evt, portMAX_DELAY);
}

#endif // SOC_USB_OTG_SUPPORTED
