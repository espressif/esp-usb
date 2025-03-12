/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "soc/soc_caps.h"

#if SOC_USB_OTG_SUPPORTED

#include "unity.h"
#include "tinyusb.h"
#include "tinyusb_default_configs.h"


/**
 * @brief TinyUSB Task specific testcase
 *
 * Scenario: Provide no descriptors (All default for Full-speed)
 * Awaiting: Install returns ESP_OK, default descriptors are being used, device is enumerated, tusb_mount_cb() is called
 */
TEST_CASE("Config: All default (Full-speed)", "[runtime_config][full_speed]")
{
    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();

    TEST_ASSERT_EQUAL_MESSAGE(false, tusb_cfg.skip_phy_setup, "Wrong default skip_phy_setup value");
    TEST_ASSERT_EQUAL_MESSAGE(TINYUSB_PORT_0, tusb_cfg.port, "Wrong default port number");
    TEST_ASSERT_EQUAL_MESSAGE(TINYUSB_TASK_AFFINITY_CPU0, tusb_cfg.task.affinity, "Wrong default task affinity");
    TEST_ASSERT_EQUAL_MESSAGE(TINYUSB_DEFAULT_TASK_SIZE, tusb_cfg.task.size, "Wrong default task size");
    TEST_ASSERT_EQUAL_MESSAGE(TINYUSB_DEFAULT_TASK_PRIO, tusb_cfg.task.priority, "Wrong default task priority");
    TEST_ASSERT_EQUAL_MESSAGE(false, tusb_cfg.self_powered, "Wrong default self-powered");
    TEST_ASSERT_EQUAL_MESSAGE(-1, tusb_cfg.vbus_monitor_io, "Wrong default VBUS monitor IO");
    TEST_ASSERT_EQUAL_MESSAGE(NULL, tusb_cfg.device_descriptor, "Wrong default device descriptor");
    TEST_ASSERT_EQUAL_MESSAGE(NULL, tusb_cfg.string_descriptor, "Wrong default string descriptor");
    TEST_ASSERT_EQUAL_MESSAGE(0, tusb_cfg.string_descriptor_count, "Wrong default string descriptor count");
    TEST_ASSERT_EQUAL_MESSAGE(NULL, tusb_cfg.configuration_descriptor, "Wrong default configuration descriptor");
}

#if (SOC_USB_OTG_PERIPH_NUM > 1)
/**
 * @brief TinyUSB Task specific testcase
 *
 * Scenario: Provide no descriptors (All default for Full-speed)
 * Awaiting: Install returns ESP_OK, default descriptors are being used, device is enumerated, tusb_mount_cb() is called
 */
TEST_CASE("Config: All default (High-speed)", "[runtime_config][high_speed]")
{
    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();

    TEST_ASSERT_EQUAL_MESSAGE(false, tusb_cfg.skip_phy_setup, "Wrong default skip_phy_setup value");
    TEST_ASSERT_EQUAL_MESSAGE(TINYUSB_PORT_1, tusb_cfg.port, "Wrong default port number");
    TEST_ASSERT_EQUAL_MESSAGE(TINYUSB_TASK_AFFINITY_CPU1, tusb_cfg.task.affinity, "Wrong default task affinity");
    TEST_ASSERT_EQUAL_MESSAGE(TINYUSB_DEFAULT_TASK_SIZE, tusb_cfg.task.size, "Wrong default task size");
    TEST_ASSERT_EQUAL_MESSAGE(TINYUSB_DEFAULT_TASK_PRIO, tusb_cfg.task.priority, "Wrong default task priority");
    TEST_ASSERT_EQUAL_MESSAGE(false, tusb_cfg.self_powered, "Wrong default self-powered");
    TEST_ASSERT_EQUAL_MESSAGE(-1, tusb_cfg.vbus_monitor_io, "Wrong default VBUS monitor IO");
    TEST_ASSERT_EQUAL_MESSAGE(NULL, tusb_cfg.device_descriptor, "Wrong default device descriptor");
    TEST_ASSERT_EQUAL_MESSAGE(NULL, tusb_cfg.string_descriptor, "Wrong default string descriptor");
    TEST_ASSERT_EQUAL_MESSAGE(0, tusb_cfg.string_descriptor_count, "Wrong default string descriptor count");
    TEST_ASSERT_EQUAL_MESSAGE(NULL, tusb_cfg.configuration_descriptor, "Wrong default configuration descriptor");
    TEST_ASSERT_EQUAL_MESSAGE(NULL, tusb_cfg.fs_configuration_descriptor, "Wrong default FS configuration descriptor");
    TEST_ASSERT_EQUAL_MESSAGE(NULL, tusb_cfg.hs_configuration_descriptor, "Wrong default HS configuration descriptor");
    TEST_ASSERT_EQUAL_MESSAGE(NULL, tusb_cfg.qualifier_descriptor, "Wrong default qualifier descriptor");
}
#endif // SOC_USB_OTG_PERIPH_NUM > 1

#endif // SOC_USB_OTG_SUPPORTED
