/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "soc/soc_caps.h"
#if SOC_USB_OTG_SUPPORTED

#include "esp_err.h"
#include "esp_sleep.h"
#include "unity.h"
#include "tinyusb.h"
#include "sdkconfig.h"
#include "common_pm.h"

#if CONFIG_TINYUSB_PM

#define TEST_PM_PUBLIC_API_GROUP "[device_pm][public_api_tinyusb_pm]"

/**
 * @brief Verify `tinyusb_pm_get_lock_status()` when the PM lock is disabled
 *
 * Scenario: Query lock status before install and after install with `pm_lock_enable=false`
 * Awaiting: `ESP_ERR_INVALID_STATE` in both cases; `ESP_ERR_INVALID_ARG` for a NULL output pointer
 */
TEST_CASE("tinyusb_pm_get_lock_status_lock_disabled", TEST_PM_PUBLIC_API_GROUP)
{
    bool lock_held;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, tinyusb_pm_get_lock_status(&lock_held));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tinyusb_pm_get_lock_status(NULL));

    const test_pm_install_opts_t opts = {
        .auto_light_sleep_enable = false,
        .pm_lock_enable = false,
    };

    test_pm_init_tinyusb_cdc(&opts);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, tinyusb_pm_get_lock_status(&lock_held));
}

/**
 * @brief Verify `tinyusb_pm_get_lock_status()` driver-state and argument checks
 *
 * Scenario: Call the API before driver install and with a NULL pointer after install
 * Awaiting: `ESP_ERR_INVALID_STATE` before install; `ESP_ERR_INVALID_ARG` for NULL after install
 */
TEST_CASE("tinyusb_pm_get_lock_status_api_validation", TEST_PM_PUBLIC_API_GROUP)
{
    bool lock_held;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, tinyusb_pm_get_lock_status(&lock_held));

    const test_pm_install_opts_t opts = {
        .auto_light_sleep_enable = false,
        .pm_lock_enable = true,
    };

    test_pm_init_tinyusb_cdc(&opts);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tinyusb_pm_get_lock_status(NULL));
}

/**
 * @brief Verify the PM lock remains held after driver install and host enumeration
 *
 * Scenario: Install with the PM lock enabled, then wait for host attach
 * Awaiting: Lock is held immediately after install and remains held after enumeration
 */
TEST_CASE("tinyusb_pm_get_lock_status_lock_held_on_install", TEST_PM_PUBLIC_API_GROUP)
{
    bool lock_held;
    const test_pm_install_opts_t opts = {
        .pm_lock_enable = true,
        .auto_light_sleep_enable = true,
    };

    test_pm_init_tinyusb_cdc(&opts);

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_pm_get_lock_status(&lock_held));
    TEST_ASSERT_TRUE(lock_held);

    expect_device_event(EVENT_BITS_ATTACHED, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_pm_get_lock_status(&lock_held));
    TEST_ASSERT_TRUE(lock_held);

    // esp32s2 does not support cpu retention
#if SOC_PM_SUPPORT_CPU_PD
    // esp_pm_configure({light_sleep_enable: true}) calls cpu retention init, we need to deinit manually to prevent memory leaks
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_cpu_retention_deinit());
#endif // SOC_PM_SUPPORT_CPU_PD
}

/**
 * @brief Verify PM lock status survives a driver deinit/reinit cycle
 *
 * Scenario: Install, enumerate, uninstall, then install again with the same PM options
 * Awaiting: Lock is held after each install once the host re-enumerates the device
 */
TEST_CASE("tinyusb_pm_get_lock_status_reinstall", TEST_PM_PUBLIC_API_GROUP)
{
    bool lock_held;
    const test_pm_install_opts_t opts = {
        .pm_lock_enable = true,
        .auto_light_sleep_enable = true,
    };

    test_pm_init_tinyusb_cdc(&opts);
    expect_device_event(EVENT_BITS_ATTACHED, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_pm_get_lock_status(&lock_held));
    TEST_ASSERT_TRUE(lock_held);

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_cdcacm_deinit(TINYUSB_CDC_ACM_0));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());

    test_pm_init_tinyusb_cdc(&opts);
    expect_device_event(EVENT_BITS_ATTACHED, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_pm_get_lock_status(&lock_held));
    TEST_ASSERT_TRUE(lock_held);

    // esp32s2 does not support cpu retention
#if SOC_PM_SUPPORT_CPU_PD
    // esp_pm_configure({light_sleep_enable: true}) calls cpu retention init, we need to deinit manually to prevent memory leaks
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_cpu_retention_deinit());
#endif // SOC_PM_SUPPORT_CPU_PD
}

/**
 * @brief Verify PM lock is released when esp_tinyusb handles USB suspend internally
 *
 * Scenario: Install with PM lock enabled, enumerate, then wait for host auto-suspend
 * Awaiting: Lock held while active; lock released after `TINYUSB_EVENT_SUSPENDED`
 */
TEST_CASE("tinyusb_pm_get_lock_status_lifecycle", TEST_PM_PUBLIC_API_GROUP)
{
    bool lock_held;
    const test_pm_install_opts_t opts = {
        .pm_lock_enable = true,
        .auto_light_sleep_enable = true,
    };

    test_pm_init_tinyusb_cdc(&opts);

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_pm_get_lock_status(&lock_held));
    TEST_ASSERT_TRUE(lock_held);

    expect_device_event(EVENT_BITS_ATTACHED, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_pm_get_lock_status(&lock_held));
    TEST_ASSERT_TRUE(lock_held);

    expect_device_event(EVENT_BITS_SUSPENDED_REMOTE_WAKE_DIS, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_pm_get_lock_status(&lock_held));
    TEST_ASSERT_FALSE(lock_held);

    // esp32s2 does not support cpu retention
#if SOC_PM_SUPPORT_CPU_PD
    // esp_pm_configure({light_sleep_enable: true}) calls cpu retention init, we need to deinit manually to prevent memory leaks
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_cpu_retention_deinit());
#endif // SOC_PM_SUPPORT_CPU_PD
}

#undef TEST_PM_PUBLIC_API_GROUP

#endif // CONFIG_TINYUSB_PM

#endif // SOC_USB_OTG_SUPPORTED
