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
#include "test_device.h"
#include "test_task.h"
#include "sdkconfig.h"

// ========================== Private logic ====================================
/**
 * @brief This top level thread processes all usb events and invokes callbacks
 */
static void test_external_tud_task(void *arg)
{
    printf("External TinyUSB task started\n");
    while (1) { // RTOS forever loop
        tud_task();
    }
}

// ============================= Tests =========================================

/**
 * @brief TinyUSB Task specific testcase
 *
 * Scenario: Initialise with Internal task
 * Awaiting: Install returns ESP_OK, device is enumerated, tusb_mount_cb() is called
 */
TEST_CASE("Task: Internal; Config: [Periph: Default, Affinity: Default, inTaskInit: No]", "[task][default]")
{
    // TinyUSB driver default configuration
    const tinyusb_config_t tusb_cfg = {
        .task_config = {
            .size = TUSB_TASK_SIZE,
            .priority = TUSB_TASK_PRIO,
        },
    };
    // Install TinyUSB driver
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    test_device_wait();
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
}

/**
 * @brief TinyUSB Task specific testcase
 *
 * Scenario: Initialise with Internal task and no affinity
 * Awaiting: Install returns ESP_OK, device is enumerated, tusb_mount_cb() is called
 */
TEST_CASE("Task: Internal; Config: [Periph: Default, Affinity: None, inTaskInit: No]", "[task][default]")
{
    // TinyUSB driver default configuration
    const tinyusb_config_t tusb_cfg = {
        .task_config = {
            .size = TUSB_TASK_SIZE,
            .priority = TUSB_TASK_PRIO,
            .affinity = TINYUSB_TASK_AFFINITY_NONE,
        },
    };
    // Install TinyUSB driver
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    test_device_wait();
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
}

/**
 * @brief TinyUSB Task specific testcase
 *
 * Scenario: Initialise with Internal task and CPU0 affinity
 * Awaiting: Install returns ESP_OK, device is enumerated, tusb_mount_cb() is called
 */
TEST_CASE("Task: Internal; Config: [Periph: Default, Affinity: CPU0, inTaskInit: No]", "[task][default]")
{
    // TinyUSB driver default configuration
    const tinyusb_config_t tusb_cfg = {
        .task_config = {
            .size = TUSB_TASK_SIZE,
            .priority = TUSB_TASK_PRIO,
            .affinity = TINYUSB_TASK_AFFINITY_CPU0,
        },
    };
    // Install TinyUSB driver
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    test_device_wait();
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
}

#if !CONFIG_FREERTOS_UNICORE
/**
 * @brief TinyUSB Task specific testcase
 *
 * Scenario: Initialise with Internal task and CPU1 affinity
 * Awaiting: Install returns ESP_OK, device is enumerated, tusb_mount_cb() is called
 */
TEST_CASE("Task: Internal; Config: [Periph: Default, Affinity: CPU1, inTaskInit: No]", "[task][default]")
{
    // TinyUSB driver default configuration
    const tinyusb_config_t tusb_cfg = {
        .task_config = {
            .size = TUSB_TASK_SIZE,
            .priority = TUSB_TASK_PRIO,
            .affinity = TINYUSB_TASK_AFFINITY_CPU1,
        },
    };
    // Install TinyUSB driver
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    test_device_wait();
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
}
#endif // CONFIG_FREERTOS_UNICORE

/**
 * @brief TinyUSB Task specific testcase
 *
 * Scenario: Initialise with Internal task and init TinyUSB stack in the task
 * Awaiting: Install returns ESP_OK, device is enumerated, tusb_mount_cb() is called
 */
TEST_CASE("Task: Internal; Config: [Periph: Default, Affinity: Default, inTaskInit: Yes]", "[task][default]")
{
    // TinyUSB driver default configuration
    const tinyusb_config_t tusb_cfg = {
        .task_config = {
            .init_in_task = true,
            .size = TUSB_TASK_SIZE,
            .priority = TUSB_TASK_PRIO,
        },
    };
    // Install TinyUSB driver
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    test_device_wait();
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
}

/**
 * @brief TinyUSB Task specific testcase
 *
 * Scenario: Initialise with Internal task and no affinity
 * Awaiting: Install returns ESP_OK, device is enumerated, tusb_mount_cb() is called
 */
TEST_CASE("Task: Internal; Config: [Periph: Default, Affinity: None, inTaskInit: Yes]", "[task][default]")
{
    // TinyUSB driver default configuration
    const tinyusb_config_t tusb_cfg = {
        .task_config = {
            .init_in_task = true,
            .size = TUSB_TASK_SIZE,
            .priority = TUSB_TASK_PRIO,
            .affinity = TINYUSB_TASK_AFFINITY_NONE,
        },
    };
    // Install TinyUSB driver
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    test_device_wait();
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
}

/**
 * @brief TinyUSB Task specific testcase
 *
 * Scenario: Initialise with Internal task and CPU0 affinity
 * Awaiting: Install returns ESP_OK, device is enumerated, tusb_mount_cb() is called
 */
TEST_CASE("Task: Internal; Config: [Periph: Default, Affinity: CPU0, inTaskInit: Yes]", "[task][default]")
{
    // TinyUSB driver default configuration
    const tinyusb_config_t tusb_cfg = {
        .task_config = {
            .init_in_task = true,
            .size = TUSB_TASK_SIZE,
            .priority = TUSB_TASK_PRIO,
            .affinity = TINYUSB_TASK_AFFINITY_CPU0,
        },
    };
    // Install TinyUSB driver
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    test_device_wait();
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
}

#if !CONFIG_FREERTOS_UNICORE
/**
 * @brief TinyUSB Task specific testcase
 *
 * Scenario: Initialise with Internal task and CPU1 affinity
 * Awaiting: Install returns ESP_OK, device is enumerated, tusb_mount_cb() is called
 */
TEST_CASE("Task: Internal; Config: [Periph: Default, Affinity: CPU1, inTaskInit: Yes]", "[task][default]")
{
    // TinyUSB driver default configuration
    const tinyusb_config_t tusb_cfg = {
        .task_config = {
            .init_in_task = true,
            .size = TUSB_TASK_SIZE,
            .priority = TUSB_TASK_PRIO,
            .affinity = TINYUSB_TASK_AFFINITY_CPU1,
        },
    };
    // Install TinyUSB driver
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    test_device_wait();
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
}
#endif // CONFIG_FREERTOS_UNICORE

/**
 * @brief TinyUSB Task specific testcase
 *
 * Scenario: Initialise with wrong Peripheral type
 * Awaiting: Install returns ESP_ERR_INVALID_ARG
 */
TEST_CASE("Task: Internal; Config: [Periph: Invalid]", "[task][default]")
{
    // TinyUSB driver default configuration
    const tinyusb_config_t tusb_cfg = {
        .periph = TINYUSB_PERIPH_MAX,
        .task_config = {
            .size = TUSB_TASK_SIZE,
            .priority = TUSB_TASK_PRIO,
        },
    };
    // Install TinyUSB driver
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tinyusb_driver_install(&tusb_cfg));
}

/**
 * @brief TinyUSB Task specific testcase
 *
 * Scenario: Initialis with External task
 * Awaiting: Install returns ESP_OK, device is enumerated, tusb_mount_cb() is called
 */
TEST_CASE("Task: External; Config: [Periph: Default, Affinity: Default, inTaskInit: No]", "[task][default]")
{
    // TinyUSB driver default configuration
    const tinyusb_config_t tusb_cfg = {
        .task_type = TINYUSB_TASK_TYPE_EXTERNAL,
    };

    // Install TinyUSB driver
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    // Create an external task for tinyusb device stack
    TaskHandle_t tusb_task_handle;
    xTaskCreate(test_external_tud_task,
                "TinyUSB",
                TUSB_TASK_SIZE,
                NULL,
                TUSB_TASK_PRIO,
                &tusb_task_handle);

    TEST_ASSERT_NOT_NULL(tusb_task_handle);
    test_device_wait();
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
    // Remove primitives
    vTaskDelete(tusb_task_handle);
}

/**
 * @brief TinyUSB Task specific testcase
 *
 * Scenario: Initialise with External task and init TinyUSB stack in the task
 * Awaiting: Install returns ESP_ERR_INVALID_ARG
 */
TEST_CASE("Task: External; Config: [Periph: Default, Affinity: Default, inTaskInit: Yes]", "[task][default]")
{
    // TinyUSB driver default configuration
    const tinyusb_config_t tusb_cfg = {
        .task_type = TINYUSB_TASK_TYPE_EXTERNAL,
        .task_config = {
            .init_in_task = true,
        },
    };

    // Install TinyUSB driver
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tinyusb_driver_install(&tusb_cfg));
}

/**
 * @brief TinyUSB Task specific testcase
 *
 * Scenario: Initialise with External task and no affinity
 * Awaiting: Install returns ESP_OK, device is enumerated, tusb_mount_cb() is called
 */
TEST_CASE("Task: External; Config: [Periph: Default, Affinity: None, inTaskInit: No]", "[task][default]")
{
    // TinyUSB driver default configuration
    const tinyusb_config_t tusb_cfg = {
        .task_type = TINYUSB_TASK_TYPE_EXTERNAL,
    };

    // Install TinyUSB driver
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    // Create an external task for tinyusb device stack
    TaskHandle_t tusb_task_handle;
    xTaskCreatePinnedToCore(test_external_tud_task,
                            "TinyUSB",
                            TUSB_TASK_SIZE,
                            NULL,
                            TUSB_TASK_PRIO,
                            &tusb_task_handle,
                            TUSB_TASK_AFFINITY_NO);
    TEST_ASSERT_NOT_NULL(tusb_task_handle);
    test_device_wait();
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
    // Remove primitives
    vTaskDelete(tusb_task_handle);
}

/**
 * @brief TinyUSB Task specific testcase
 *
 * Scenario: Initialise with External task and CPU0 affinity
 * Awaiting: Install returns ESP_OK, device is enumerated, tusb_mount_cb() is called
 */
TEST_CASE("Task: External; Config: [Periph: Default; Affinity: CPU0, inTaskInit: No]", "[task][default]")
{
    // TinyUSB driver default configuration
    const tinyusb_config_t tusb_cfg = {
        .task_type = TINYUSB_TASK_TYPE_EXTERNAL,
    };

    // Install TinyUSB driver
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    // Create an external task for tinyusb device stack
    TaskHandle_t tusb_task_handle;
    xTaskCreatePinnedToCore(test_external_tud_task,
                            "TinyUSB",
                            TUSB_TASK_SIZE,
                            NULL,
                            TUSB_TASK_PRIO,
                            &tusb_task_handle,
                            TUSB_TASK_AFFINITY_CPU0);
    TEST_ASSERT_NOT_NULL(tusb_task_handle);
    test_device_wait();
    // Delete task before uninstalling the driver
    vTaskDelete(tusb_task_handle);
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
}

#if !CONFIG_FREERTOS_UNICORE
/**
 * @brief TinyUSB Task specific testcase
 *
 * Scenario: Initialise with External task and CPU1 affinity
 * Awaiting: Install returns ESP_OK, device is enumerated, tusb_mount_cb() is called
 */
TEST_CASE("Task: External; Config: [Periph: Default; Affinity: CPU1, inTaskInit: No]", "[task][default]")
{
    // TinyUSB driver default configuration
    const tinyusb_config_t tusb_cfg = {
        .task_type = TINYUSB_TASK_TYPE_EXTERNAL,
    };

    // Install TinyUSB driver
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    // Create an external task for tinyusb device stack
    TaskHandle_t tusb_task_handle;
    xTaskCreatePinnedToCore(test_external_tud_task,
                            "TinyUSB",
                            TUSB_TASK_SIZE,
                            NULL,
                            TUSB_TASK_PRIO,
                            &tusb_task_handle,
                            TUSB_TASK_AFFINITY_CPU1);
    TEST_ASSERT_NOT_NULL(tusb_task_handle);
    test_device_wait();
    // Delete task before uninstalling the driver
    vTaskDelete(tusb_task_handle);
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
}
#endif // !CONFIG_FREERTOS_UNICORE

/**
 * @brief TinyUSB Task specific testcase
 *
 * Scenario: Initialise with External task and uninstall when task still running
 * Awaiting: Install returns ESP_OK, device is enumerated, tusb_mount_cb() is called
 */
TEST_CASE("Task: External; Config: [Periph: Default; Affinity: CPU0, inTaskInit: No], uninstall when task still running", "[task][default]")
{
    // TinyUSB driver default configuration
    const tinyusb_config_t tusb_cfg = {
        .task_type = TINYUSB_TASK_TYPE_EXTERNAL,
    };

    // Install TinyUSB driver
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    // Create an external task for tinyusb device stack
    TaskHandle_t tusb_task_handle;
    xTaskCreatePinnedToCore(test_external_tud_task,
                            "TinyUSB",
                            TUSB_TASK_SIZE,
                            NULL,
                            TUSB_TASK_PRIO,
                            &tusb_task_handle,
                            TUSB_TASK_AFFINITY_CPU0);
    TEST_ASSERT_NOT_NULL(tusb_task_handle);
    test_device_wait();
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
    vTaskDelete(tusb_task_handle);
}

#if !CONFIG_FREERTOS_UNICORE
/**
 * @brief TinyUSB Task specific testcase
 *
 * @note This test is failing on ESP32P4 with TinyUSB debug level = 3
 *
 * Scenario: Initialise with External task and CPU1 affinity
 * Awaiting: Install returns ESP_OK, device is enumerated, tusb_mount_cb() is called
 */
TEST_CASE("Task: External; Config: [Periph: Default; Affinity: CPU1, inTaskInit: No], uninstall when task still running", "[task][default]")
{
    // TinyUSB driver default configuration
    const tinyusb_config_t tusb_cfg = {
        .task_type = TINYUSB_TASK_TYPE_EXTERNAL,
    };

    // Install TinyUSB driver
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    // Create an external task for tinyusb device stack
    TaskHandle_t tusb_task_handle;
    xTaskCreatePinnedToCore(test_external_tud_task,
                            "TinyUSB",
                            TUSB_TASK_SIZE,
                            NULL,
                            TUSB_TASK_PRIO,
                            &tusb_task_handle,
                            TUSB_TASK_AFFINITY_CPU1);
    TEST_ASSERT_NOT_NULL(tusb_task_handle);
    test_device_wait();
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
    vTaskDelete(tusb_task_handle);
}
#endif // !CONFIG_FREERTOS_UNICORE

#endif // SOC_USB_OTG_SUPPORTED
