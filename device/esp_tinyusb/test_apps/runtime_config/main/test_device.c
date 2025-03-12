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
#include "test_task.h"
//
#include "descriptors_control.h"


static SemaphoreHandle_t wait_mount = NULL;

#define TUSB_DEVICE_DELAY_MS        1000

void test_device_setup(void)
{
    wait_mount = xSemaphoreCreateBinary();
    TEST_ASSERT_NOT_NULL(wait_mount);
}

void test_device_release(void)
{
    TEST_ASSERT_NOT_NULL(wait_mount);
    vSemaphoreDelete(wait_mount);
}

void test_device_wait(void)
{
    // Wait for tud_mount_cb() to be called
    TEST_ASSERT_EQUAL_MESSAGE(pdTRUE, xSemaphoreTake(wait_mount, pdMS_TO_TICKS(TUSB_DEVICE_DELAY_MS)), "No tusb_mount_cb() in time");
    // Delay to allow finish the enumeration
    // Disable this delay could lead to potential race conditions when the tud_task() is pinned to another CPU
    vTaskDelay(pdMS_TO_TICKS(250));
}

// ========================== TinyUSB General Device Descriptors ===============================

#define TUSB_CFG_DESC_TOTAL_LEN                 (TUD_CONFIG_DESC_LEN + CFG_TUD_CDC * TUD_CDC_DESC_LEN)

static const uint8_t test_fs_configuration_descriptor[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, CFG_TUD_CDC * 2, 0, TUSB_CFG_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_SELF_POWERED | TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(0, 4, 0x81, 8, 0x02, 0x82, 64),
#if CFG_TUD_CDC > 1
    TUD_CDC_DESCRIPTOR(2, 4, 0x83, 8, 0x04, 0x84, 64),
#endif
};

#if (TUD_OPT_HIGH_SPEED)
static const uint8_t test_hs_configuration_descriptor[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, 4, 0, TUSB_CFG_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_SELF_POWERED | TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(0, 4, 0x81, 8, 0x02, 0x82, 512),
    TUD_CDC_DESCRIPTOR(2, 4, 0x83, 8, 0x04, 0x84, 512),
};

static const tusb_desc_device_qualifier_t device_qualifier = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 0x01,
    .bReserved = 0
};
#endif // TUD_OPT_HIGH_SPEED

static const tusb_desc_device_t test_device_descriptor = {
    .bLength = sizeof(test_device_descriptor),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A, // This is Espressif VID. This needs to be changed according to Users / Customers
    .idProduct = 0x4002,
    .bcdDevice = 0x100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01
};

/**
 * @brief String descriptor
 */
const char *test_string_descriptor[USB_STRING_DESCRIPTOR_ARRAY_SIZE + 1] = {
    // array of pointer to string descriptors
    (char[]){0x09, 0x04},  // 0: is supported language is English (0x0409)
    "TinyUSB",             // 1: Manufacturer
    "TinyUSB Device",      // 2: Product
    "123456",              // 3: Serials, should use chip ID
    "String 4",            // 4: Test string #5
    "String 5",            // 5: Test string #6
    "String 6",            // 6: Test string #7
    "String 7",            // 7: Test string #8
    "String 8",            // 8: Test string #9
};


// ========================== Callbacks ========================================
/**
 * @brief TinyUSB callback for device mount.
 *
 * @note
 * For Linux-based Hosts: Reflects the SetConfiguration() request from the Host Driver.
 * For Win-based Hosts: SetConfiguration() request is present only with available Class in device descriptor.
 */
void tud_mount_cb(void)
{
    xSemaphoreGive(wait_mount);
}


/**
 * @brief TinyUSB Task specific testcase
 *
 * Scenario: Verify backward compatibility with v1.x.x
 *
 * @note Only for a local run as backward compatibility is not supported
 */
TEST_CASE("Device: v1.x.x backward compatibility", "[local_run]")
{
    // TinyUSB driver default configuration
    const tinyusb_config_t tusb_cfg = { 0 };
    // Install TinyUSB driver
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    test_device_wait();
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
}

/**
 * @brief TinyUSB Task specific testcase
 *
 * Scenario: Invalid configuration
 * Awaiting: Install returns ESP_ERR_INVALID_ARG
 */
TEST_CASE("Device: Invalid config", "[runtime_config][default]")
{
    // TinyUSB driver default configuration
    const tinyusb_config_t tusb_cfg = { 0 };
    // Install TinyUSB driver
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tinyusb_driver_install(&tusb_cfg));
}

/**
 * @brief TinyUSB Task specific testcase
 *
 * Scenario: Invalid Task Priority
 * Awaiting: Install returns ESP_ERR_INVALID_ARG
 */
TEST_CASE("Device: Invalid Task Priority", "[runtime_config][default]")
{
    // TinyUSB driver default configuration
    const tinyusb_config_t tusb_cfg = {
        .task_config = {
            .size = 1,
        },
    };
    // Install TinyUSB driver
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, tinyusb_driver_install(&tusb_cfg));
}

/**
 * @brief TinyUSB Task specific testcase
 *
 * Scenario: Invalid Peripheral Speed
 * Awaiting: Install returns ESP_ERR_INVALID_ARG
 */
TEST_CASE("Device: Invalid Speed", "[runtime_config][default]")
{
    // TinyUSB driver default configuration
    const tinyusb_config_t tusb_cfg = {
        .speed = TINYUSB_SPEED_MAX,
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
 * Scenario: Provide more string descriptors than supported
 * Awaiting: Install returns ESP_ERR_NOT_SUPPORTED
 */
TEST_CASE("Device: String Descriptors overflow", "[runtime_config][default]")
{
    // TinyUSB driver default configuration
    const tinyusb_config_t tusb_cfg = {
        .task_config = {
            .size = TUSB_TASK_SIZE,
            .priority = TUSB_TASK_PRIO,
        },
        .string_descriptor = test_string_descriptor,
        .string_descriptor_count = USB_STRING_DESCRIPTOR_ARRAY_SIZE + 1,
    };
    // Install TinyUSB driver
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, tinyusb_driver_install(&tusb_cfg));
}

/**
 * @brief TinyUSB Task specific testcase
 *
 * Scenario: Provide maximum supported string descriptors
 * Awaiting: Install returns ESP_OK, device is enumerated, tusb_mount_cb() is called
 */
TEST_CASE("Device: String Descriptos maximum value", "[runtime_config][default]")
{
    // TinyUSB driver default configuration
    const tinyusb_config_t tusb_cfg = {
        .task_config = {
            .size = TUSB_TASK_SIZE,
            .priority = TUSB_TASK_PRIO,
        },
        .string_descriptor = test_string_descriptor,
        .string_descriptor_count = USB_STRING_DESCRIPTOR_ARRAY_SIZE,
    };
    // Install TinyUSB driver
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    test_device_wait();
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
}

/**
 * @brief TinyUSB Task specific testcase
 *
 * Scenario: Provide only device descriptor
 * Awaiting: Install returns ESP_OK, device is enumerated, tusb_mount_cb() is called
 */
TEST_CASE("Device: Device & Configuration]", "[runtime_config][default]")
{
    // TinyUSB driver configuration
    const tinyusb_config_t tusb_cfg = {
        .task_config = {
            .size = TUSB_TASK_SIZE,
            .priority = TUSB_TASK_PRIO,
        },
        .device_descriptor = &test_device_descriptor,
#if (TUD_OPT_HIGH_SPEED)
        .fs_configuration_descriptor = test_fs_configuration_descriptor,
        .hs_configuration_descriptor = test_hs_configuration_descriptor,
        .qualifier_descriptor = &device_qualifier,
#else
        .configuration_descriptor = test_fs_configuration_descriptor,
#endif // TUD_OPT_HIGH_SPEED
    };
    // Install TinyUSB driver
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    test_device_wait();
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
}

/**
 * @brief TinyUSB Task specific testcase
 *
 * Scenario: Provide no descriptors (All default for Full-speed)
 * Awaiting: Install returns ESP_OK, default descriptors are being used, device is enumerated, tusb_mount_cb() is called
 */
TEST_CASE("Device: All default (Full-speed)", "[runtime_config][full_speed]")
{
    const tinyusb_config_t tusb_cfg = {
        .periph = TINYUSB_PERIPH_FS,
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
 * Scenario: Provide only device descriptor
 * Awaiting: Install returns ESP_OK, default descriptor is being used, device is enumerated, tusb_mount_cb() is called
 */
TEST_CASE("Device: Device Descriptor only", "[runtime_config][default]")
{
    const tinyusb_config_t tusb_cfg = {
        .task_config = {
            .size = TUSB_TASK_SIZE,
            .priority = TUSB_TASK_PRIO,
        },
        .device_descriptor = &test_device_descriptor,
#if (TUD_OPT_HIGH_SPEED)
        .fs_configuration_descriptor = NULL,
        .hs_configuration_descriptor = NULL,
        .qualifier_descriptor = &device_qualifier,
#else
        .configuration_descriptor = NULL,
#endif // TUD_OPT_HIGH_SPEED
    };
    // Install TinyUSB driver
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    test_device_wait();
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
}

/**
 * @brief TinyUSB Task specific testcase
 *
 * Scenario: Provide only device & FS configuration descriptor
 * Awaiting: Install returns ESP_OK, default FS configuration descriptor is being used, device is enumerated, tusb_mount_cb() is called
 *
 * Note: HS configuration descriptor is not provided by user (legacy compatibility) and default configuration descriptor for HS is used.
 */
TEST_CASE("Device: Device & Full-speed config only", "[runtime_config][default]")
{
    const tinyusb_config_t tusb_cfg = {
        .task_config = {
            .size = TUSB_TASK_SIZE,
            .priority = TUSB_TASK_PRIO,
        },
        .device_descriptor = &test_device_descriptor,
#if (TUD_OPT_HIGH_SPEED)
        .fs_configuration_descriptor = test_fs_configuration_descriptor,
        .hs_configuration_descriptor = NULL,
        .qualifier_descriptor = &device_qualifier,
#else
        .configuration_descriptor = test_fs_configuration_descriptor,
#endif // TUD_OPT_HIGH_SPEED
    };
    // Install TinyUSB driver
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    test_device_wait();
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
}

#if (CONFIG_IDF_TARGET_ESP32P4)
/**
 * @brief TinyUSB Task specific testcase
 *
 * Scenario: Provide no descriptors (All default for High-speed)
 * Awaiting: Install returns ESP_OK, default descriptors are being used, device is enumerated, tusb_mount_cb() is called
 */
TEST_CASE("Device: All default (High-speed)", "[runtime_config][high_speed]")
{
    const tinyusb_config_t tusb_cfg = {
        .periph = TINYUSB_PERIPH_HS,
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
 * Scenario: Provide only device & HS configuration descriptor
 * Awaiting: Install returns ESP_OK, default FS configuration descriptor is being used, device is enumerated, tusb_mount_cb() is called
 *
 * Note: FS configuration descriptor is not provided by user (legacy compatibility) and default configuration descriptor for FS is used.
 */
TEST_CASE("Device: Device and High-speed config only", "[runtime_config][high_speed]")
{
    const tinyusb_config_t tusb_cfg = {
        .task_config = {
            .size = TUSB_TASK_SIZE,
            .priority = TUSB_TASK_PRIO,
        },
        .device_descriptor = &test_device_descriptor,
        .configuration_descriptor = NULL,
        .hs_configuration_descriptor = test_hs_configuration_descriptor,
        .qualifier_descriptor = &device_qualifier,
    };
    // Install TinyUSB driver
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    test_device_wait();
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
}
#endif // CONFIG_IDF_TARGET_ESP32P4

#endif // SOC_USB_OTG_SUPPORTED
