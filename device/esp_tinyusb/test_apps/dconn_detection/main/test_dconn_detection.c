/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "soc/soc_caps.h"

#if SOC_USB_OTG_SUPPORTED

#include <stdio.h>
#include <string.h>
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_sig_map.h"
#include "unity.h"
#include "tinyusb.h"
#include "tusb_tasks.h"

static const char *TAG = "dconn_detection";

#define DEVICE_DETACH_TEST_ROUNDS           10
#define DEVICE_DETACH_ROUND_DELAY_MS        10
#define TEARDOWN_DEVICE_ATTACH_TIMEOUT_MS   1000

#if (CONFIG_IDF_TARGET_ESP32P4)
#define USB_SRP_BVALID_IN_IDX       USB_SRP_BVALID_PAD_IN_IDX
#endif // CONFIG_IDF_TARGET_ESP32P4

/* TinyUSB descriptors
   ********************************************************************* */
#define TUSB_DESC_TOTAL_LEN         (TUD_CONFIG_DESC_LEN)

SemaphoreHandle_t wait_dev_stage_change = NULL;
static unsigned int dev_mounted = 0;
static unsigned int dev_umounted = 0;


static uint8_t const test_configuration_descriptor[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, 0, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_SELF_POWERED | TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
};

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

#if (TUD_OPT_HIGH_SPEED)
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

// Invoked when device is mounted
void tud_mount_cb(void)
{
    /**
     * @attention Tests relying on this callback only pass on Linux USB Host!
     *
     * This callback is issued after SetConfiguration command from USB Host.
     * However, Windows issues SetConfiguration only after a USB driver was assigned to the device.
     * So in case you are implementing a Vendor Specific class, or your device has 0 interfaces, this callback is not issued on Windows host.
     */
    ESP_LOGD(TAG, "%s", __FUNCTION__);
    dev_mounted++;
    xSemaphoreGive(wait_dev_stage_change);
}

// Invoked when device is unmounted
void tud_umount_cb(void)
{
    ESP_LOGD(TAG, "%s", __FUNCTION__);
    dev_umounted++;
    xSemaphoreGive(wait_dev_stage_change);
}

/**
 * @brief TinyUSB Disconnect Detection test case
 *
 * This is specific artificial test for verifying the disconnection detection event.
 * Normally, this event comes as a result of detaching USB device from the port and disappearing the VBUS voltage.
 * In this test case, we use GPIO matrix and connect the signal to the ZERO or ONE constant inputs.
 * Connection to constant ONE input emulates the attachment to the USB Host port (appearing VBUS).
 * Connection to constant ZERO input emulates the detachment from the USB Host port (removing VBUS).
 *
 * Test logic:
 * - Install TinyUSB Device stack without any class
 * - In cycle:
 *      - Emulate the detachment, get the tud_umount_cb(), increase the dev_umounted value
 *      - Emulate the attachment, get the tud_mount_cb(), increase the dev_mounted value
 * - Verify that dev_umounted == dev_mounted
 * - Verify that dev_mounted == DEVICE_DETACH_TEST_ROUNDS, where DEVICE_DETACH_TEST_ROUNDS - amount of rounds
 * - Uninstall TinyUSB Device stack
 *
 */
TEST_CASE("dconn_detection", "[esp_tinyusb][dconn]")
{
    wait_dev_stage_change = xSemaphoreCreateBinary();

    // Install TinyUSB driver
    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = &test_device_descriptor,
        .string_descriptor = NULL,
        .string_descriptor_count = 0,
        .external_phy = false,
#if (TUD_OPT_HIGH_SPEED)
        .fs_configuration_descriptor = test_configuration_descriptor,
        .hs_configuration_descriptor = test_configuration_descriptor,
        .qualifier_descriptor = &device_qualifier,
#else
        .configuration_descriptor = test_configuration_descriptor,
#endif // TUD_OPT_HIGH_SPEED
        .self_powered = true,
        .vbus_monitor_io = 0, // Doesn't matter in the current test scenario, as the connection and disconnection events are emulated by multiplexing bvalid signal
    };

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(wait_dev_stage_change, pdMS_TO_TICKS(TEARDOWN_DEVICE_ATTACH_TIMEOUT_MS)));

    //

    unsigned int rounds = DEVICE_DETACH_TEST_ROUNDS;
    dev_mounted = 0;
    dev_umounted = 0;

    while (rounds--) {
        // HIGH to emulate disconnect USB device
        ESP_LOGD(TAG, "bvalid(0)");
        esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ZERO_INPUT, USB_SRP_BVALID_IN_IDX, false);
        TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(wait_dev_stage_change, pdMS_TO_TICKS(TEARDOWN_DEVICE_ATTACH_TIMEOUT_MS)));
        // HIGH to emulate connect USB device
        ESP_LOGD(TAG, "bvalid(1)");
        esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, USB_SRP_BVALID_IN_IDX, false);
        TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(wait_dev_stage_change, pdMS_TO_TICKS(TEARDOWN_DEVICE_ATTACH_TIMEOUT_MS)));
    }
    // Cleanup
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());

    // Verify test results
    TEST_ASSERT_EQUAL(dev_umounted, dev_mounted);
    TEST_ASSERT_EQUAL(DEVICE_DETACH_TEST_ROUNDS, dev_mounted);

}
#endif // SOC_USB_OTG_SUPPORTED
