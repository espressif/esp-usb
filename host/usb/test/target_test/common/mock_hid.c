/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "unity.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "class/hid/hid_device.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mock_hid.h"

// Interrupt IN endpoint parameters (one endpoint per HID interface)
#define MOCK_HID_EP_IN_R5_MPS8      0x81    // Interface 0: 5-byte report, MPS 8 (DWORD)
#define MOCK_HID_EP_IN_R4_MPS4      0x82    // Interface 1: 4-byte report, MPS 4 (DWORD)
#define MOCK_HID_EP_IN_R4_MPS5      0x83    // Interface 2: 4-byte report, MPS 5 (non-DWORD)
#define MOCK_HID_EP_INTERVAL_MS     10

#define MOCK_HID_REPORT_PERIOD_MS   10      // Period of generating mouse movement reports
#define MOCK_HID_MOVE_DELTA         5       // Cursor movement per report

#define MOCK_HID_REPORT_LEN_5B      5       // buttons, X, Y, wheel, AC pan
#define MOCK_HID_REPORT_LEN_4B      4       // buttons, X, Y, wheel

/************* TinyUSB descriptors ****************/

#define TUSB_DESC_TOTAL_LEN      (TUD_CONFIG_DESC_LEN + MOCK_HID_NUM_ITF * TUD_HID_DESC_LEN)

/************* Report descriptors ****************/

/**
 * @brief HID report descriptor: 5-byte mouse (no report ID)
 *
 * Report layout: buttons(1), X(1), Y(1), wheel(1), AC pan(1)
 */
static const uint8_t hid_report_descriptor_5b[] = {
    TUD_HID_REPORT_DESC_MOUSE()
};

/**
 * @brief HID report descriptor: 4-byte mouse (no report ID)
 *
 * Standard mouse template without the consumer page (AC pan) section.
 * Report layout: buttons(1), X(1), Y(1), wheel(1)
 */
static const uint8_t hid_report_descriptor_4b[] = {
    HID_USAGE_PAGE ( HID_USAGE_PAGE_DESKTOP      ),
    HID_USAGE      ( HID_USAGE_DESKTOP_MOUSE     ),
    HID_COLLECTION ( HID_COLLECTION_APPLICATION  ),
    HID_USAGE      ( HID_USAGE_DESKTOP_POINTER ),
    HID_COLLECTION ( HID_COLLECTION_PHYSICAL   ),
    HID_USAGE_PAGE  ( HID_USAGE_PAGE_BUTTON  ),
    HID_USAGE_MIN   ( 1                                      ),
    HID_USAGE_MAX   ( 5                                      ),
    HID_LOGICAL_MIN ( 0                                      ),
    HID_LOGICAL_MAX ( 1                                      ),
    /* Left, Right, Middle, Backward, Forward buttons */
    HID_REPORT_COUNT( 5                                      ),
    HID_REPORT_SIZE ( 1                                      ),
    HID_INPUT       ( HID_DATA | HID_VARIABLE | HID_ABSOLUTE ),
    /* 3 bit padding */
    HID_REPORT_COUNT( 1                                      ),
    HID_REPORT_SIZE ( 3                                      ),
    HID_INPUT       ( HID_CONSTANT                           ),
    HID_USAGE_PAGE  ( HID_USAGE_PAGE_DESKTOP ),
    /* X, Y, wheel position [-127, 127] */
    HID_USAGE       ( HID_USAGE_DESKTOP_X                  ),
    HID_USAGE       ( HID_USAGE_DESKTOP_Y                  ),
    HID_USAGE       ( HID_USAGE_DESKTOP_WHEEL              ),
    HID_LOGICAL_MIN ( 0x81                                 ),
    HID_LOGICAL_MAX ( 0x7f                                 ),
    HID_REPORT_COUNT( 3                                    ),
    HID_REPORT_SIZE ( 8                                    ),
    HID_INPUT       ( HID_DATA | HID_VARIABLE | HID_RELATIVE ),
    HID_COLLECTION_END,
    HID_COLLECTION_END
};

// Report length per HID interface (instance)
static const uint8_t report_len_per_itf[MOCK_HID_NUM_ITF] = {
    MOCK_HID_REPORT_LEN_5B,
    MOCK_HID_REPORT_LEN_4B,
    MOCK_HID_REPORT_LEN_4B,
};

/************* Configuration descriptor ****************/

/**
 * @brief Configuration descriptor: composite mouse with 3 HID interfaces
 *
 * 1 configuration, 3 HID interfaces (boot protocol mouse), one interrupt IN endpoint each
 */
static const uint8_t hid_configuration_descriptor[] = {
    // Configuration number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, MOCK_HID_NUM_ITF, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    // Interface number, string index, protocol, report descriptor len, EP In address, size & polling interval
    TUD_HID_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_MOUSE, sizeof(hid_report_descriptor_5b), MOCK_HID_EP_IN_R5_MPS8, 8, MOCK_HID_EP_INTERVAL_MS),
    TUD_HID_DESCRIPTOR(1, 0, HID_ITF_PROTOCOL_MOUSE, sizeof(hid_report_descriptor_4b), MOCK_HID_EP_IN_R4_MPS4, 4, MOCK_HID_EP_INTERVAL_MS),
    TUD_HID_DESCRIPTOR(2, 0, HID_ITF_PROTOCOL_MOUSE, sizeof(hid_report_descriptor_4b), MOCK_HID_EP_IN_R4_MPS5, 5, MOCK_HID_EP_INTERVAL_MS),
};

/************* Device descriptors ****************/

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

/************* String descriptors ****************/

/**
 * @brief String descriptor
 */
static const char *hid_mouse_string_descriptor[] = {
    (char[]){0x09, 0x04},  // 0: Supported language is English (0x0409)
    "TinyUSB",             // 1: Manufacturer
    "Mock HID Mouse",      // 2: Product
    "123456",              // 3: Serial, should use chip ID
};

/************* Device qualifiers ****************/

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
    return (instance == 0) ? hid_report_descriptor_5b : hid_report_descriptor_4b;
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
 * @brief Mouse task
 *
 * Task that periodically generates mouse movement (square pattern) on every HID interface
 *
 * Keeps the interrupt IN endpoints of all interfaces loaded with reports.
 * Each interface sends report_len_per_itf[instance] bytes per report (4 or 5).
 */
static void mock_hid_mouse_task(void *arg)
{
    (void)arg;
    uint8_t step = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(MOCK_HID_REPORT_PERIOD_MS));
        if (!tud_mounted()) {
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
        // buttons, X, Y, wheel, AC pan (each interface sends only the first report_len_per_itf bytes)
        const uint8_t report[MOCK_HID_REPORT_LEN_5B] = {0, (uint8_t)dx, (uint8_t)dy, 0, 0};
        for (uint8_t instance = 0; instance < MOCK_HID_NUM_ITF; instance++) {
            if (tud_hid_n_ready(instance)) {
                tud_hid_n_report(instance, 0, report, report_len_per_itf[instance]);
            }
        }
    }
}

void mock_hid_dev_run(void)
{
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = &hid_device_descriptor;
    tusb_cfg.descriptor.string = hid_mouse_string_descriptor;
    tusb_cfg.descriptor.string_count = sizeof(hid_mouse_string_descriptor) / sizeof(hid_mouse_string_descriptor[0]);
    tusb_cfg.descriptor.full_speed_config = hid_configuration_descriptor;
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.high_speed_config = hid_configuration_descriptor;
    tusb_cfg.descriptor.qualifier = &device_qualifier;
#endif // TUD_OPT_HIGH_SPEED

    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    xTaskCreate(mock_hid_mouse_task, "mock_hid_mouse", 4096, NULL, 5, NULL);

    printf("USB initialization DONE\n");
}

/* Following test case implements a composite HID mouse USB device that can be used as a mock device for USB Host tests */

/**
 * @brief Run mock HID mouse device: composite with 3 HID interfaces
 *
 *  - Interface 0 (EP 0x81): 5-byte report, MPS 8 (DWORD)      - report < MPS, short packet
 *  - Interface 1 (EP 0x82): 4-byte report, MPS 4 (DWORD)      - report == MPS, multi-packet fill
 *  - Interface 2 (EP 0x83): 4-byte report, MPS 5 (non-DWORD)  - report < MPS, short packet
 *
 * Each interface periodically generates mouse movement reports on its interrupt IN endpoint
 */
TEST_CASE("mock_hid_mouse_dev", "[esp_tinyusb][hid_dev_mouse][ignore]")
{
    mock_hid_dev_run();

    while (1) {
        vTaskDelay(10);
    }
}
