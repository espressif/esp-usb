/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "soc/soc_caps.h"

#if SOC_USB_OTG_SUPPORTED

#include "esp_err.h"
#include "unity.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "device_handling.h"

/**
 * @brief TinyUSB PM specific testcase
 *
 * Scenario: Install and uninstall with default PM configuration (lock disabled)
 * Awaiting: Install returns ESP_OK, device is enumerated, uninstall returns ESP_OK
 */
TEST_CASE("PM: install succeeds with lock disabled", "[runtime_config][full_speed][high_speed]")
{
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(test_device_event_handler);
    TEST_ASSERT_FALSE(tusb_cfg.pm_lock_enable);
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    test_device_wait();
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
}

/**
 * @brief TinyUSB PM specific testcase
 *
 * Scenario: Install and uninstall with default PM configuration (lock enabled in config struct but not in sdkconfig)
 * Awaiting: Install returns ESP_OK, device is enumerated, uninstall returns ESP_OK
 */
TEST_CASE("PM: install succeeds with lock enabled", "[runtime_config][full_speed][high_speed]")
{
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(test_device_event_handler);
    tusb_cfg.pm_lock_enable = true;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    test_device_wait();
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
}

#endif // SOC_USB_OTG_SUPPORTED
