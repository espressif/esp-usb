/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "soc/soc_caps.h"
// #if SOC_USB_OTG_SUPPORTED

//
#include <stdio.h>
#include <string.h>
//
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
//
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
//
#include "unity.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"

static const char *TEARDOWN_CMD = "teardown";
// Delay between device connection, required for the Host to handle device disconnection/connection without errors
#define TEARDOWN_DELAY_MS       2000
#define TEARDOWN_AMOUNT         10

static const tusb_desc_device_t cdc_device_descriptor = {
    .bLength = sizeof(cdc_device_descriptor),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_ESPRESSIF_VID,
    .idProduct = 0x4002,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01
};

static const uint16_t cdc_desc_config_len = TUD_CONFIG_DESC_LEN + CFG_TUD_CDC * TUD_CDC_DESC_LEN;
static const uint8_t cdc_desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 4, 0, cdc_desc_config_len, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(0, 4, 0x81, 8, 0x02, 0x82, (TUD_OPT_HIGH_SPEED ? 512 : 64)),
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

static QueueHandle_t rx_queue;
static uint8_t rx_buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE + 1];

typedef struct {
    int itf;                                          // Interface number
    uint8_t data[CONFIG_TINYUSB_CDC_RX_BUFSIZE + 1];  // Data buffer
    size_t len;                                       // Number of bytes received
} rx_pkt_t;

static void test_rx_enqueue_pkt(int itf, uint8_t *buf, size_t size)
{
    rx_pkt_t rx_pkt = {
        .itf = itf,
        .len = size,
    };
    memcpy(rx_pkt.data, buf, size);
    xQueueSend(rx_queue, &rx_pkt, 0);
}

static void tinyusb_cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    size_t rx_size = 0;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_cdcacm_read(itf, rx_buf, CONFIG_TINYUSB_CDC_RX_BUFSIZE, &rx_size));
    test_rx_enqueue_pkt(itf, rx_buf, rx_size);
}

/**
 * @brief TinyUSB Teardown specific testcase
 */
TEST_CASE("tinyusb_teardown", "[esp_tinyusb][teardown]")
{
    rx_pkt_t rx_pkt;
    // Create FreeRTOS primitives
    rx_queue = xQueueCreate(5, sizeof(rx_pkt_t));
    TEST_ASSERT(rx_queue);

    // TinyUSB driver configuration
    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = &cdc_device_descriptor,
        .string_descriptor = NULL,
        .string_descriptor_count = 0,
        .external_phy = false,
#if (TUD_OPT_HIGH_SPEED)
        .fs_configuration_descriptor = cdc_desc_configuration,
        .hs_configuration_descriptor = cdc_desc_configuration,
        .qualifier_descriptor = &device_qualifier,
#else
        .configuration_descriptor = cdc_desc_configuration,
#endif // TUD_OPT_HIGH_SPEED
    };

    // TinyUSB ACM Driver configuration
    tinyusb_config_cdcacm_t acm_cfg = {
        .usb_dev = TINYUSB_USBDEV_0,
        .cdc_port = TINYUSB_CDC_ACM_0,
        .rx_unread_buf_sz = 64,
        .callback_rx = &tinyusb_cdc_rx_callback,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL
    };
    int attempts = TEARDOWN_AMOUNT;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(TEARDOWN_DELAY_MS));
        TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
        // Init CDC 0
        TEST_ASSERT_FALSE(tusb_cdc_acm_initialized(TINYUSB_CDC_ACM_0));
        TEST_ASSERT_EQUAL(ESP_OK, tusb_cdc_acm_init(&acm_cfg));
        TEST_ASSERT_TRUE(tusb_cdc_acm_initialized(TINYUSB_CDC_ACM_0));

        // Wait for the pytest poke
        while (1) {
            if (xQueueReceive(rx_queue, &rx_pkt, portMAX_DELAY)) {
                if (rx_pkt.len) {
                    /* echoed back */
                    TEST_ASSERT_EQUAL(rx_pkt.len, tinyusb_cdcacm_write_queue(rx_pkt.itf, rx_pkt.data, rx_pkt.len));
                    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_cdcacm_write_flush(rx_pkt.itf, pdMS_TO_TICKS(100)));
                    if (strncmp(TEARDOWN_CMD, (const char *) rx_pkt.data, rx_pkt.len) == 0) {
                        // Wait for the host
                        attempts--;
                        break;
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(TEARDOWN_DELAY_MS));
        TEST_ASSERT_EQUAL(ESP_OK, tinyusb_cdcacm_unregister_callback(TINYUSB_CDC_ACM_0, CDC_EVENT_RX));
        TEST_ASSERT_EQUAL(ESP_OK, tusb_cdc_acm_deinit(TINYUSB_CDC_ACM_0));
        TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());

        if (attempts == 0) {
            break;
        }
    }
    // Remove primitives
    vQueueDelete(rx_queue);
    // All attempts should be completed
    TEST_ASSERT_EQUAL(0, attempts);
}

// #endif
