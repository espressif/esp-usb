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
#include "esp_private/usb_phy.h"
//
#include "unity.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "test_task.h"
#include "sdkconfig.h"
#include "device_handling.h"

#define MULTIPLE_THREADS_TASKS_NUM 5

static int nb_of_success = 0;

static void test_task_install(void *arg)
{
    TaskHandle_t parent_task_handle = (TaskHandle_t)arg;

    // Install TinyUSB driver
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(test_device_event_handler);
    tusb_cfg.phy.skip_setup = true; // Skip phy setup to allow multiple tasks to install the driver

    if (tinyusb_driver_install(&tusb_cfg) == ESP_OK) {
        test_device_wait();
        TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
        nb_of_success++;
    }

    // Notify the parent task that the task completed the job
    xTaskNotifyGive(parent_task_handle);
    // Delete task
    vTaskDelete(NULL);
}

// ============================= Tests =========================================

/**
 * @brief TinyUSB Task specific testcase
 *
 * Scenario: Trying to install driver from several tasks
 * Note: when skip_phy_setup = false, the task access will be determined by the first task install the phy
 */
TEST_CASE("Multitask: Install", "[runtime_config][full_speed][high_speed]")
{
    usb_phy_handle_t phy_hdl = NULL;
    // Install the PHY externally
    usb_phy_config_t phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_DEVICE,
    };
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, usb_new_phy(&phy_conf, &phy_hdl), "Unable to install USB PHY ");

    nb_of_success = 0;
    // Create tasks that will start the driver
    for (int i = 0; i < MULTIPLE_THREADS_TASKS_NUM; i++) {
        TEST_ASSERT_EQUAL(pdTRUE, xTaskCreate(test_task_install,
                                              "InstallTask",
                                              4096,
                                              (void *) xTaskGetCurrentTaskHandle(),
                                              4 + i,
                                              NULL));
    }

    // Wait until all tasks are finished
    vTaskDelay(pdMS_TO_TICKS(5000));
    // Check if all tasks finished, we should get all notification from the tasks
    TEST_ASSERT_EQUAL_MESSAGE(MULTIPLE_THREADS_TASKS_NUM, ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000)), "Not all tasks finished");
    // There should be only one task that was able to install the driver
    TEST_ASSERT_EQUAL_MESSAGE(1, nb_of_success, "Only one task should be able to install the driver");
    // Clean-up
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, usb_del_phy(phy_hdl), "Unable to delete PHY");
}

#endif // SOC_USB_OTG_SUPPORTED
