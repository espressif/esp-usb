/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
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
#include "tusb_cdc_acm.h"

static const char *TAG = "teardown";

SemaphoreHandle_t wait_mount = NULL;
SemaphoreHandle_t wait_terminal = NULL;
SemaphoreHandle_t wait_command = NULL;

static uint8_t rx_buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE + 1];
static uint8_t tx_buf[CONFIG_TINYUSB_CDC_TX_BUFSIZE + 1] = { 0 };

#define TEARDOWN_CMD_KEY                0xAA
#define TEARDOWN_RPL_KEY                0x55
#define TEARDOWN_CMD_RPL_SIZE           ((TUD_OPT_HIGH_SPEED ? 512 : 64))
#define TEARDOWN_ATTACH_TIMEOUT_MS      2000
#define TEARDOWN_COMMAND_TIMEOUT_MS     3000
#define TEARDOWN_AMOUNT                 4

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
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, cdc_desc_config_len, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
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

static void tinyusb_cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    // Something was received
    xSemaphoreGive(wait_command);
}

/**
 * @brief CDC device line change callback
 *
 * CDC device signals, that the DTR, RTS states changed
 *
 * @param[in] itf   CDC device index
 * @param[in] event CDC event type
 */
void tinyusb_cdc_line_state_changed_callback(int itf, cdcacm_event_t *event)
{
    int dtr = event->line_state_changed_data.dtr;
    int rts = event->line_state_changed_data.rts;
    ESP_LOGD(TAG, "Line state changed on channel %d: DTR:%d, RTS:%d", itf, dtr, rts);

    // Terminal:
    // dtr==1 && rts==1 - connected
    // dtr==0 && rts==0 - disconnected
    xSemaphoreGive(wait_terminal);
}

// Invoked when device is mounted
void tud_mount_cb(void)
{
    xSemaphoreGive(wait_mount);
}

/**
 * @brief TinyUSB Teardown specific testcase
 *
 * Scenario:
 * - Installs the tinyUSB driver via esp-tinyusb wrapper with 1xCDC class device
 * - Awaits device configuration be the Host (TEARDOWN_ATTACH_TIMEOUT_MS)
 * - Awaits the terminal connection (TEARDOWN_ATTACH_TIMEOUT_MS)
 * - Expects the command sequence from the Host (TEARDOWN_COMMAND_TIMEOUT_MS)
 * - Replies with the response sequence to the Host
 * - Awaits terminal disconnection (TEARDOWN_ATTACH_TIMEOUT_MS)
 * - Teardowns the tinyUSB driver via esp-tinyusb wrapper
 * - Repeats the steps from the step.1 N times (where N = TEARDOWN_AMOUNT)
 * - Verifies amount of attempts and memory leakage (attempts should be 0)
 *
 * command sequence[] = ep_size * 0xAA
 * response sequence[] = ep_size * 0x55
 *
 * Hint: Values 0xAA and 0x55 were selected to verify the buffer memory integrity,
 * as the 0xAA and 0x55 are the inversion of each other and the data bits in the same position changes from 1 to 0 in every transaction.
 */
TEST_CASE("tinyusb_teardown", "[esp_tinyusb][teardown]")
{
    size_t rx_size = 0;

    wait_mount = xSemaphoreCreateBinary();
    TEST_ASSERT_NOT_EQUAL(NULL, wait_mount);
    wait_command = xSemaphoreCreateBinary();
    TEST_ASSERT_NOT_EQUAL(NULL, wait_command);
    wait_terminal = xSemaphoreCreateBinary();
    TEST_ASSERT_NOT_EQUAL(NULL, wait_terminal);

    // Prep reply
    for (int i = 0; i < TEARDOWN_CMD_RPL_SIZE; i++) {
        tx_buf[i] = TEARDOWN_RPL_KEY;
    }

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
    const tinyusb_config_cdcacm_t acm_cfg = {
        .usb_dev = TINYUSB_USBDEV_0,
        .cdc_port = TINYUSB_CDC_ACM_0,
        .rx_unread_buf_sz = 64,
        .callback_rx = &tinyusb_cdc_rx_callback,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = &tinyusb_cdc_line_state_changed_callback,
        .callback_line_coding_changed = NULL
    };

    int attempts = TEARDOWN_AMOUNT;
    while (attempts) {
        TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
        // Init CDC 0
        TEST_ASSERT_FALSE(tusb_cdc_acm_initialized(TINYUSB_CDC_ACM_0));
        TEST_ASSERT_EQUAL(ESP_OK, tusb_cdc_acm_init(&acm_cfg));
        TEST_ASSERT_TRUE(tusb_cdc_acm_initialized(TINYUSB_CDC_ACM_0));
        // Wait for the usb event
        ESP_LOGD(TAG, "wait dev mounted...");
        TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(wait_mount, pdMS_TO_TICKS(TEARDOWN_ATTACH_TIMEOUT_MS)));
        ESP_LOGD(TAG, "wait terminal connection...");
        TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(wait_terminal, pdMS_TO_TICKS(TEARDOWN_ATTACH_TIMEOUT_MS)));
        // Wait for the command
        ESP_LOGD(TAG, "wait command...");
        TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(wait_command, pdMS_TO_TICKS(TEARDOWN_COMMAND_TIMEOUT_MS)));
        TEST_ASSERT_EQUAL(ESP_OK, tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, rx_buf, CONFIG_TINYUSB_CDC_RX_BUFSIZE, &rx_size));
        for (int i = 0; i < TEARDOWN_CMD_RPL_SIZE; i++) {
            TEST_ASSERT_EQUAL(TEARDOWN_CMD_KEY, rx_buf[i]);
        }
        ESP_LOGD(TAG, "command received");
        // Reply the response sequence
        ESP_LOGD(TAG, "send response...");
        TEST_ASSERT_EQUAL(TEARDOWN_CMD_RPL_SIZE, tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, tx_buf, TEARDOWN_CMD_RPL_SIZE));
        TEST_ASSERT_EQUAL(ESP_OK, tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(1000)));
        ESP_LOGD(TAG, "wait for terminal disconnection");
        TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(wait_terminal, pdMS_TO_TICKS(TEARDOWN_ATTACH_TIMEOUT_MS)));
        // Teardown
        attempts--;
        TEST_ASSERT_EQUAL(ESP_OK, tinyusb_cdcacm_unregister_callback(TINYUSB_CDC_ACM_0, CDC_EVENT_RX));
        TEST_ASSERT_EQUAL(ESP_OK, tinyusb_cdcacm_unregister_callback(TINYUSB_CDC_ACM_0, CDC_EVENT_LINE_STATE_CHANGED));
        TEST_ASSERT_EQUAL(ESP_OK, tusb_cdc_acm_deinit(TINYUSB_CDC_ACM_0));
        TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
    }
    // Remove primitives
    vSemaphoreDelete(wait_mount);
    vSemaphoreDelete(wait_command);
    // All attempts should be completed
    TEST_ASSERT_EQUAL(0, attempts);
}

#endif
