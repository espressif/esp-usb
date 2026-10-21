/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include "sdkconfig.h"

#if (CONFIG_TINYUSB_HID_COUNT > 0)

#include "esp_log.h"
#include "unity.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "class/hid/hid_device.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mock_hid.h"

// Interrupt IN endpoint parameters
#define MOCK_HID_EP_IN              0x81
#define MOCK_HID_EP_MPS             8
#define MOCK_HID_EP_INTERVAL_MS     10

#define MOCK_HID_REPORT_PERIOD_MS   10      // Period of generating mouse movement reports
#define MOCK_HID_MOVE_DELTA         5       // Cursor movement per report

static const char *TAG = "mock_hid_dev";

/************* TinyUSB descriptors ****************/

#define TUSB_DESC_TOTAL_LEN      (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)

/**
 * @brief HID report descriptor: mouse (no report ID)
 */
static const uint8_t hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_MOUSE()
};

/**
 * @brief Configuration descriptor
 *
 * 1 configuration, 1 HID interface (boot protocol mouse)
 */
static const uint8_t hid_configuration_descriptor[] = {
    // Configuration number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    // Interface number, string index, protocol, report descriptor len, EP In address, size & polling interval
    TUD_HID_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_MOUSE, sizeof(hid_report_descriptor), MOCK_HID_EP_IN, MOCK_HID_EP_MPS, MOCK_HID_EP_INTERVAL_MS),
};

/**
 * @brief Device descriptor
 */
static const tusb_desc_device_t hid_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = TINYUSB_ESPRESSIF_VID,
    .idProduct = 0x4004,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01
};

/**
 * @brief String descriptor
 */
static const char *hid_string_descriptor[] = {
    (char[]){0x09, 0x04},  // 0: Supported language is English (0x0409)
    "TinyUSB",             // 1: Manufacturer
    "Mock HID Mouse",      // 2: Product
    "123456",              // 3: Serial, should use chip ID
};

#if (TUD_OPT_HIGH_SPEED)
/**
 * @brief Device qualifier descriptor
 */
static const tusb_desc_device_qualifier_t device_qualifier = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 0x01,
    .bReserved = 0
};
#endif // TUD_OPT_HIGH_SPEED

/********* TinyUSB HID callbacks ***************/

// Invoked when received GET HID REPORT DESCRIPTOR request
// Application return pointer to descriptor, whose contents must exist long enough for transfer to complete
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return hid_report_descriptor;
}

// Invoked when received GET_REPORT control request
// Application must fill buffer report's content and return its length.
// Return zero will cause the stack to STALL request
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

// Invoked when received SET_REPORT control request or
// received data on OUT endpoint ( Report ID = 0, Type = 0 )
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}

/************* Mock HID device *****************/

/**
 * @brief Task that periodically generates mouse movement (square pattern)
 *
 * Keeps the interrupt IN endpoint loaded with reports.
 */
static void mock_hid_mouse_task(void *arg)
{
    (void)arg;
    uint8_t step = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(MOCK_HID_REPORT_PERIOD_MS));
        if (!tud_mounted() || !tud_hid_ready()) {
            continue;
        }
        // Move the cursor in a square pattern
        int8_t dx = 0, dy = 0;
        switch (step++ & 0x03) {
        case 0:
            dx = MOCK_HID_MOVE_DELTA;
            break;
        case 1:
            dy = MOCK_HID_MOVE_DELTA;
            break;
        case 2:
            dx = -MOCK_HID_MOVE_DELTA;
            break;
        case 3:
            dy = -MOCK_HID_MOVE_DELTA;
            break;
        }
        tud_hid_mouse_report(0, 0, dx, dy, 0, 0);
    }
}

void mock_hid_dev_run(void)
{
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = &hid_device_descriptor;
    tusb_cfg.descriptor.full_speed_config = hid_configuration_descriptor;
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.qualifier = &device_qualifier;
    tusb_cfg.descriptor.high_speed_config = hid_configuration_descriptor;
#endif // TUD_OPT_HIGH_SPEED
    tusb_cfg.descriptor.string = hid_string_descriptor;
    tusb_cfg.descriptor.string_count = sizeof(hid_string_descriptor) / sizeof(hid_string_descriptor[0]);

    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(mock_hid_mouse_task, "mock_hid_mouse", 4096, NULL, 5, NULL));
    ESP_LOGI(TAG, "USB initialization DONE");
}

/* Following test case implements an HID mouse USB device that can be used as a mock device for USB Host tests */

/**
 * @brief Run mock HID mouse device
 *
 * The device periodically generates mouse movement reports on its interrupt IN endpoint
 */
TEST_CASE("mock_hid_dev", "[esp_tinyusb][hid_dev][ignore]")
{
    mock_hid_dev_run();

    while (1) {
        vTaskDelay(10);
    }
}

#endif // (CONFIG_TINYUSB_HID_COUNT > 0)
