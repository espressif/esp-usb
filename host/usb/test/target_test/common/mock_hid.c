/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <string.h>
#include "sdkconfig.h"

#if (CONFIG_TINYUSB_HID_COUNT > 0)

#include "esp_log.h"
#include "unity.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "class/hid/hid_device.h"
#include "device/usbd_pvt.h"    // For usbd_edpt_stall()
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "mock_hid.h"

// Device geometry (endpoint addresses, MPS, report lengths, intervals) is single-sourced from mock_hid.h
#define MOCK_HID_LOOPBACK_MAX_LEN   (3 * MOCK_HID_EP_MPS_ITF0)  // Longest echoed report (multi-packet OUT)

#define MOCK_HID_REPORT_PERIOD_MS   10      // Period of generating mouse movement reports
#define MOCK_HID_MOVE_DELTA         5       // Cursor movement per report

static const char *TAG = "mock_hid_dev";

/************* TinyUSB descriptors ****************/

// The loopback interface (interface 0) has an interrupt OUT endpoint alongside its IN endpoint
#define TUSB_DESC_TOTAL_LEN      (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN + (MOCK_HID_NUM_ITF - 1) * TUD_HID_DESC_LEN)

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
    MOCK_HID_REPORT_LEN_5B,
};

/************* Configuration descriptor ****************/

/**
 * @brief Configuration descriptor: composite mouse with 4 HID interfaces
 *
 * 1 configuration, 4 HID interfaces (boot protocol mouse), one interrupt IN endpoint each;
 * the loopback interface (interface 0) additionally has an interrupt OUT endpoint
 */
static const uint8_t hid_configuration_descriptor[] = {
    // Configuration number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, MOCK_HID_NUM_ITF, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    // Interface number, string index, protocol, report descriptor len, EP Out & In address, size & polling interval
    TUD_HID_INOUT_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_MOUSE, sizeof(hid_report_descriptor_5b), MOCK_HID_EP_OUT_LOOPBACK, MOCK_HID_EP_IN_R5_MPS8, MOCK_HID_EP_MPS_ITF0, MOCK_HID_EP_INTERVAL_MS),
    // Interface number, string index, protocol, report descriptor len, EP In address, size & polling interval
    TUD_HID_DESCRIPTOR(1, 0, HID_ITF_PROTOCOL_MOUSE, sizeof(hid_report_descriptor_4b), MOCK_HID_EP_IN_R4_MPS4, MOCK_HID_EP_MPS_ITF1, MOCK_HID_EP_STRICT_INTERVAL),
    TUD_HID_DESCRIPTOR(2, 0, HID_ITF_PROTOCOL_MOUSE, sizeof(hid_report_descriptor_4b), MOCK_HID_EP_IN_R4_MPS5, MOCK_HID_EP_MPS_ITF2, MOCK_HID_EP_INTERVAL_MS),
    TUD_HID_DESCRIPTOR(3, 0, HID_ITF_PROTOCOL_MOUSE, sizeof(hid_report_descriptor_5b), MOCK_HID_EP_IN_R5_MPS5, MOCK_HID_EP_MPS_ITF3, MOCK_HID_EP_INTERVAL_MS),
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

// Loopback queue: a report received on the loopback interface's OUT endpoint is echoed on its IN endpoint
#define LOOPBACK_QUEUE_LEN      4

/**
 * @brief OUT report received on the loopback interface, pending echo on its IN EP
 */
typedef struct {
    uint8_t len;                                /**< Number of valid bytes in data */
    uint8_t data[MOCK_HID_LOOPBACK_MAX_LEN];    /**< Report payload (zero-padded) */
} loopback_report_t;

static StaticQueue_t s_loopback_queue_struct;
static uint8_t s_loopback_queue_storage[LOOPBACK_QUEUE_LEN * sizeof(loopback_report_t)];
static QueueHandle_t s_loopback_queue;  // Written by tud_hid_set_report_cb(), read by the mouse task

// Set by the SET_REPORT mode hook to select the mock HID device behavior
static mock_hid_device_mode_t s_device_mode = MOCK_HID_DEVICE_MODE_MOUSE_REPORT;

// Invoked when received GET HID REPORT DESCRIPTOR request
// Application return pointer to descriptor, whose contents must exist long enough for transfer to complete
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    // Instances 0 and 3 have a 5-byte report, the rest a 4-byte report
    return (instance == 0 || instance == 3) ? hid_report_descriptor_5b : hid_report_descriptor_4b;
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
// received data on OUT endpoint
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize)
{
    (void)report_id;
    if (bufsize < 1) {
        return;     // Nothing to do for an empty report
    }

    ESP_LOGI(TAG, "set_report: itf=%u type=%u len=%u b0=0x%02x", instance, report_type, bufsize, buffer[0]);
    // Host test hook: stall the interrupt IN endpoint on a SET_REPORT carrying the STALL trigger byte
    if (buffer[0] == MOCK_HID_SET_REPORT_STALL_TRIGGER) {
        usbd_edpt_stall(0, MOCK_HID_EP_IN_R5_MPS8);
        return;
    }

    // Host test hook: select the device mode on a SET_REPORT carrying the mode trigger byte;
    // the second byte holds the mock_hid_device_mode_t value to select
    if (bufsize >= 2 && buffer[0] == MOCK_HID_SET_REPORT_MODE_TRIGGER) {
        switch (buffer[1]) {
        case MOCK_HID_DEVICE_MODE_MOUSE_REPORT:
        case MOCK_HID_DEVICE_MODE_LOOPBACK_ONLY:
            s_device_mode = (mock_hid_device_mode_t)buffer[1];
            break;
        default:
            break;
        }
        return;
    }
    // Loopback: queue a report received on the OUT endpoint for echo on the IN endpoint
    // (sent by the mouse task). Reports up to MOCK_HID_LOOPBACK_MAX_LEN are echoed.
    // Note: OUT endpoint data shares this callback with control SET_REPORT requests,
    // so an OUT report starting with a trigger value would hit the test hooks above
    // instead; host tests must not use a trigger value as the first byte of an OUT report.
    if (instance == MOCK_HID_LOOPBACK_ITF) {
        if (bufsize <= MOCK_HID_LOOPBACK_MAX_LEN) {
            loopback_report_t echo = {
                .len = (uint8_t)bufsize,
                .data = {0}
            };
            memcpy(echo.data, buffer, bufsize);
            if (xQueueSend(s_loopback_queue, &echo, 0) != pdTRUE) {
                ESP_LOGW(TAG, "Loopback queue full, OUT report dropped");
            }
        }
    }
}

/************* Mock HID device *****************/

static void mock_hid_queue_strict_report(void)
{
    static uint8_t report_sequence;
    const uint8_t report[MOCK_HID_REPORT_LEN_4B] = {0, report_sequence++, 0, 0};
    if (tud_hid_n_ready(MOCK_HID_STRICT_ITF)) {
        tud_hid_n_report(MOCK_HID_STRICT_ITF, 0, report, sizeof(report));
    }
}

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len)
{
    (void)report;
    (void)len;
    if (instance == MOCK_HID_STRICT_ITF) {
        mock_hid_queue_strict_report();
    }
}

/**
 * @brief Mouse task
 *
 * Task that periodically generates mouse movement (square pattern) on every HID interface
 *
 * Keeps the interrupt IN endpoints of all interfaces loaded with reports.
 * Each interface sends report_len_per_itf[instance] bytes per report (4 or 5).
 * On the loopback interface, a pending looped-back OUT report is sent instead of mouse
 * movement; in MOCK_HID_DEVICE_MODE_LOOPBACK_ONLY mode, no mouse reports are sent on it.
 */
static void mock_hid_mouse_task(void *arg)
{
    (void)arg;
    uint8_t step = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(MOCK_HID_REPORT_PERIOD_MS));
        if (!tud_mounted()) {
            // Fall back to mouse reports after the host disconnects at the end of a test case
            s_device_mode = MOCK_HID_DEVICE_MODE_MOUSE_REPORT;
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
            if (instance == MOCK_HID_STRICT_ITF) {
                continue;   // Fed by the report-complete callback instead (never NAKs)
            }
            if (!tud_hid_n_ready(instance)) {
                continue;
            }
            if (instance == MOCK_HID_LOOPBACK_ITF) {
                // The loopback interface prioritizes echoing a received OUT report over
                // mouse movement; in LOOPBACK_ONLY mode it NAKs until an echo is queued
                if (uxQueueMessagesWaiting(s_loopback_queue) > 0) {
                    loopback_report_t echo;
                    // Cannot fail: queue checked non-empty, single consumer
                    xQueueReceive(s_loopback_queue, &echo, 0);
                    ESP_LOGI(TAG, "echo queued: %d bytes", echo.len);
                    tud_hid_n_report(instance, 0, echo.data, echo.len);
                } else if (s_device_mode == MOCK_HID_DEVICE_MODE_MOUSE_REPORT) {
                    tud_hid_n_report(instance, 0, report, report_len_per_itf[instance]);
                }
                continue;
            }
            tud_hid_n_report(instance, 0, report, report_len_per_itf[instance]);
        }
        mock_hid_queue_strict_report();
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

    // The loopback queue must exist before the device can receive OUT reports
    s_loopback_queue = xQueueCreateStatic(LOOPBACK_QUEUE_LEN, sizeof(loopback_report_t), s_loopback_queue_storage, &s_loopback_queue_struct);
    TEST_ASSERT_NOT_NULL(s_loopback_queue);

    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(mock_hid_mouse_task, "mock_hid_mouse", 4096, NULL, 5, NULL));
    ESP_LOGI(TAG, "USB initialization DONE");
}

/* Following test case implements a composite HID mouse USB device that can be used as a mock device for USB Host tests */

/**
 * @brief Run mock HID mouse device: composite with 4 HID interfaces
 *
 *  - Interface 0 (EP 0x81): 5-byte report, MPS 8 (DWORD)      - report < MPS, short packet
 *    (+ EP 0x01 OUT, MPS 8: loopback, received OUT reports are echoed on EP 0x81)
 *  - Interface 1 (EP 0x82): 4-byte report, MPS 4, bInterval 1 - report == MPS, multi-packet fill, strict (never NAKs)
 *  - Interface 2 (EP 0x83): 4-byte report, MPS 5 (non-DWORD)  - report < MPS, short packet
 *  - Interface 3 (EP 0x84): 5-byte report, MPS 5 (non-DWORD)  - report == MPS, full packets
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

#endif // (CONFIG_TINYUSB_HID_COUNT > 0)
