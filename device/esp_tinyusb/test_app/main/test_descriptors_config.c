/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "soc/soc_caps.h"

#if SOC_USB_OTG_SUPPORTED

#include <stdio.h>
#include <string.h>
#include "esp_system.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_sig_map.h"
#include "unity.h"
#include "tinyusb.h"
#include "tusb_tasks.h"

#define DEVICE_MOUNT_TIMEOUT_MS         5000

// ========================= TinyUSB descriptors ===============================
#define TUSB_DESC_TOTAL_LEN         (TUD_CONFIG_DESC_LEN)

static uint8_t const test_fs_configuration_descriptor[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, 0, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_SELF_POWERED | TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
};

#if (TUD_OPT_HIGH_SPEED)
static uint8_t const test_hs_configuration_descriptor[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, 0, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_SELF_POWERED | TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
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

// ========================== Private logic ====================================
SemaphoreHandle_t desc_config_device_mounted = NULL;

static bool __test_prep(void)
{
    desc_config_device_mounted = xSemaphoreCreateBinary();
    return (desc_config_device_mounted != NULL);
}

static esp_err_t __test_wait_conn(void)
{
    if (!desc_config_device_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    return ( xSemaphoreTake(desc_config_device_mounted, pdMS_TO_TICKS(DEVICE_MOUNT_TIMEOUT_MS))
             ? ESP_OK
             : ESP_ERR_TIMEOUT );
}

static void __test_conn(void)
{
    if (desc_config_device_mounted) {
        xSemaphoreGive(desc_config_device_mounted);
    }
}

static void __test_free(void)
{
    if (desc_config_device_mounted) {
        vSemaphoreDelete(desc_config_device_mounted);
    }
}

// ========================== Callbacks ========================================
// Invoked when device is mounted
void test_descriptors_config_mount_cb(void)
{
    __test_conn();
}

void test_descriptors_config_umount_cb(void)
{

}

TEST_CASE("descriptors_config_all_default", "[usb_device]")
{
    TEST_ASSERT_EQUAL(true, __test_prep());
    // Install TinyUSB driver
    const tinyusb_config_t tusb_cfg = {
        .external_phy = false,
        .device_descriptor = NULL,
        .configuration_descriptor = NULL,
#if (TUD_OPT_HIGH_SPEED)
        .hs_configuration_descriptor = NULL,
#endif // TUD_OPT_HIGH_SPEED
    };
    // Install
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    // Wait for mounted callback
    TEST_ASSERT_EQUAL(ESP_OK, __test_wait_conn());
    // Cleanup
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
    __test_free();
}

TEST_CASE("descriptors_config_device", "[usb_device]")
{
    TEST_ASSERT_EQUAL(true, __test_prep());
    // Install TinyUSB driver
    const tinyusb_config_t tusb_cfg = {
        .external_phy = false,
        .device_descriptor = &test_device_descriptor,
        .configuration_descriptor = NULL,
#if (TUD_OPT_HIGH_SPEED)
        .hs_configuration_descriptor = NULL,
#endif // TUD_OPT_HIGH_SPEED
    };
    // Install
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    // Wait for mounted callback
    TEST_ASSERT_EQUAL(ESP_OK, __test_wait_conn());
    // Cleanup
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
    __test_free();
}

TEST_CASE("descriptors_config_device_and_config", "[usb_device]")
{
    TEST_ASSERT_EQUAL(true, __test_prep());
    // Install TinyUSB driver
    const tinyusb_config_t tusb_cfg = {
        .external_phy = false,
        .device_descriptor = &test_device_descriptor,
        .configuration_descriptor = test_fs_configuration_descriptor,
#if (TUD_OPT_HIGH_SPEED)
        .hs_configuration_descriptor = NULL,
#endif // TUD_OPT_HIGH_SPEED
    };
    // Install
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    // Wait for mounted callback
    TEST_ASSERT_EQUAL(ESP_OK, __test_wait_conn());
    // Cleanup
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
    __test_free();
}

#if (TUD_OPT_HIGH_SPEED)
TEST_CASE("descriptors_config_device_and_fs_config_only", "[usb_device]")
{
    TEST_ASSERT_EQUAL(true, __test_prep());
    // Install TinyUSB driver
    const tinyusb_config_t tusb_cfg = {
        .external_phy = false,
        .device_descriptor = &test_device_descriptor,
        .configuration_descriptor = test_fs_configuration_descriptor,
        .hs_configuration_descriptor = NULL,
    };
    // Install
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    // Wait for mounted callback
    TEST_ASSERT_EQUAL(ESP_OK, __test_wait_conn());
    // Cleanup
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
    __test_free();
}

TEST_CASE("descriptors_config_device_and_hs_config_only", "[usb_device]")
{
    TEST_ASSERT_EQUAL(true, __test_prep());
    // Install TinyUSB driver
    const tinyusb_config_t tusb_cfg = {
        .external_phy = false,
        .device_descriptor = &test_device_descriptor,
        .configuration_descriptor = NULL,
        .hs_configuration_descriptor = test_hs_configuration_descriptor,
    };
    // Install
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    // Wait for mounted callback
    TEST_ASSERT_EQUAL(ESP_OK, __test_wait_conn());
    // Cleanup
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
    __test_free();
}

TEST_CASE("descriptors_config_all_configured", "[usb_device]")
{
    TEST_ASSERT_EQUAL(true, __test_prep());
    // Install TinyUSB driver
    const tinyusb_config_t tusb_cfg = {
        .external_phy = false,
        .device_descriptor = &test_device_descriptor,
        .fs_configuration_descriptor = test_fs_configuration_descriptor,
        .hs_configuration_descriptor = test_hs_configuration_descriptor,
    };
    // Install
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    // Wait for mounted callback
    TEST_ASSERT_EQUAL(ESP_OK, __test_wait_conn());
    // Cleanup
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
    __test_free();
}
#endif // TUD_OPT_HIGH_SPEED

//
// HID Class device configuration
//
#define TUSB_HID_CONFIG_DESC_TOTAL_LEN      (TUD_CONFIG_DESC_LEN + CFG_TUD_HID * TUD_HID_DESC_LEN)

static const tusb_desc_device_t test_hid_class_dev_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0210,       // 2.10
    .bDeviceClass = TUSB_CLASS_UNSPECIFIED,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = 64,
    .idVendor = 0x413C,     // Dell Computer Corp
    .idProduct = 0x301A,    // Dell MS116 Optical Mouse
    .bcdDevice = 0x0100,    // 1.00
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

const uint8_t test_hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(HID_ITF_PROTOCOL_KEYBOARD)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(HID_ITF_PROTOCOL_MOUSE))
};

const char *test_hid_string_descriptor[5] = {
    (char[]){0x09, 0x04},  // 0: is supported language is English (0x0409)
    "Espressif",             // 1: Manufacturer
    "USB Test HID Class",      // 2: Product
    "123456",              // 3: Serials, should use chip ID
    "Test HID Interface",  // 4: HID
};

static const uint8_t test_hid_fs_configuration_descriptor[] = {
    // Configuration number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUSB_HID_CONFIG_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, 4, false, sizeof(test_hid_report_descriptor), 0x81, 16, 10),
};


#if (TUD_OPT_HIGH_SPEED)
static uint8_t const test_hid_hs_configuration_descriptor[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    // Configuration number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUSB_HID_CONFIG_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    // Interface number, string index, boot protocol, report descriptor len, EP In address, size & polling interval
    TUD_HID_DESCRIPTOR(0, 4, false, sizeof(test_hid_report_descriptor), 0x81, 16, 10),
};
#endif // TUD_OPT_HIGH_SPEED

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    return test_hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen)
{
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize)
{

}

//
// CDC Class 1x Interface
//
#define TUSB_CDC_CONFIG_DESC_TOTAL_LEN      (TUD_CONFIG_DESC_LEN + 1 * TUD_CDC_DESC_LEN)

const tusb_desc_device_t test_cdc_class_dev_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = 64,
    .idVendor = 0x0001,
    .idProduct = 0x0002,
    .bcdDevice = 0x0000,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01
};

const char *test_cdc_string_descriptor[5] = {
    (char[]){0x09, 0x04},  // 0: is supported language is English (0x0409)
    "Espressif",             // 1: Manufacturer
    "USB Test CDC Class",      // 2: Product
    "123456",              // 3: Serials, should use chip ID
    "Test CDC Interface",  // 4: CDC
};

static const uint8_t test_cdc_fs_configuration_descriptor[] = {
    // Configuration number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUSB_CDC_CONFIG_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(0, 4, 0x81, 8, 0x02, 0x82, 64),
};

//
// MSC Class
//
#define TUSB_MSC_CONFIG_DESC_TOTAL_LEN      (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)


const tusb_desc_device_t test_msc_class_dev_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0210,       // 2.10
    .bDeviceClass = TUSB_CLASS_MSC,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = 64,
    .idVendor = 0x0781,     // SanDisk Corp
    .idProduct = 0x5595,
    .bcdDevice = 0x0100,    // 1.00
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01
};

const char *test_msc_string_descriptor[4] = {
    (char[]){0x09, 0x04},  // 0: is supported language is English (0x0409)
    "Espressif",             // 1: Manufacturer
    "USB Test MSC Class",    // 2: Product
    NULL,                   // 3: NULL
};

static const uint8_t test_msc_fs_configuration_descriptor[] = {
    // Configuration number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUSB_MSC_CONFIG_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_MSC_DESCRIPTOR(1, 0, 0x01, 0x81, 64),
};

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    (void) lun;
    // Our device doesn't have any media for testing purpose
    return false;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void) lun;
    *block_count = 0;
    *block_size  = 0;
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
    (void) lun;
    const char vid[] = "USB";
    const char pid[] = "MSC Class";
    const char rev[] = "0.1";

    memcpy(vendor_id, vid, strlen(vid));
    memcpy(product_id, pid, strlen(pid));
    memcpy(product_rev, rev, strlen(rev));
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    return 0;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    return 0;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
{
    return 0;
}

//
// Configuration array
//

enum {
    TEST_CFG_HID_CLASS = 0,
    TEST_CFG_CDC_CLASS,
    TEST_CFG_MSC_CLASS,
};

const tinyusb_config_t tusb_cfg[] = {
    // HID Class
    {
        .external_phy = false,
        .device_descriptor = &test_hid_class_dev_desc,
        .string_descriptor = test_hid_string_descriptor,
        .string_descriptor_count = sizeof(test_hid_string_descriptor) / sizeof(test_hid_string_descriptor[0]),
        .configuration_descriptor = test_hid_fs_configuration_descriptor,
#if (TUD_OPT_HIGH_SPEED)
        .hs_configuration_descriptor = test_hid_hs_configuration_descriptor,
#endif // TUD_OPT_HIGH_SPEED
    },
    // CDC Class
    {
        .external_phy = false,
        .device_descriptor = &test_cdc_class_dev_desc,
        .string_descriptor = test_cdc_string_descriptor,
        .string_descriptor_count = sizeof(test_cdc_string_descriptor) / sizeof(test_cdc_string_descriptor[0]),
        .configuration_descriptor = test_cdc_fs_configuration_descriptor,
#if (TUD_OPT_HIGH_SPEED)
        .hs_configuration_descriptor = test_cdc_hs_configuration_descriptor,
#endif // TUD_OPT_HIGH_SPEED
    },
    //  MSC Class
    {
        .external_phy = false,
        .device_descriptor = &test_msc_class_dev_desc,
        .string_descriptor = test_msc_string_descriptor,
        .string_descriptor_count = sizeof(test_msc_string_descriptor) / sizeof(test_msc_string_descriptor[0]),
        .configuration_descriptor = test_msc_fs_configuration_descriptor,
#if (TUD_OPT_HIGH_SPEED)
        .hs_configuration_descriptor = test_msc_hs_configuration_descriptor,
#endif // TUD_OPT_HIGH_SPEED
    },
};


TEST_CASE("device_class_reconfiguration", "[usb_device]")
{
    TEST_ASSERT_EQUAL(true, __test_prep());
    // Install HID Class only
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg[TEST_CFG_HID_CLASS]));
    // Wait for mounted callback
    TEST_ASSERT_EQUAL(ESP_OK, __test_wait_conn());
    // Cleanup
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());

    // Install CDC Class only
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg[TEST_CFG_CDC_CLASS]));
    // Wait for mounted callback
    TEST_ASSERT_EQUAL(ESP_OK, __test_wait_conn());
    // Cleanup
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());

    // Install MSC Class only
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg[TEST_CFG_MSC_CLASS]));
    // Wait for mounted callback
    TEST_ASSERT_EQUAL(ESP_OK, __test_wait_conn());
    // Cleanup
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());

    __test_free();
}

#endif // SOC_USB_OTG_SUPPORTED
