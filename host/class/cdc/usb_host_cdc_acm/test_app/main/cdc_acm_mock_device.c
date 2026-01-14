/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include "sdkconfig.h"
#include "unity.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cdc_acm_mock_device.h"

static enum test_cdc_acm_mock_device _test_cdc_acm_mock_device_mode = TEST_CDC_ACM_MOCK_DEVICE_MAX;
static TaskHandle_t main_task_hdl = NULL;

const char *CDC_DEV_TAG = "CDC device";
static uint8_t buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE + 1];
static void tinyusb_cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    size_t rx_size = 0;
    /* read and write back */
    ESP_ERROR_CHECK(tinyusb_cdcacm_read(itf, buf, CONFIG_TINYUSB_CDC_RX_BUFSIZE, &rx_size));
    tinyusb_cdcacm_write_queue(itf, buf, rx_size);
    tinyusb_cdcacm_write_flush(itf, 0);
}

static const tusb_desc_device_t cdc_device_descriptor = {
    .bLength = sizeof(cdc_device_descriptor),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = TINYUSB_ESPRESSIF_VID,
    .idProduct = 0x4002,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01
};

static const uint16_t cdc_desc_config_len = TUD_CONFIG_DESC_LEN + CFG_TUD_CDC * TUD_CDC_DESC_LEN;
static const uint8_t cdc_fs_desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 4, 0, cdc_desc_config_len, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(0, 4, 0x81, 8, 0x02, 0x82, 64),
    TUD_CDC_DESCRIPTOR(2, 4, 0x83, 8, 0x04, 0x84, 64),
};

#if (TUD_OPT_HIGH_SPEED)
static const uint8_t cdc_hs_desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 4, 0, cdc_desc_config_len, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
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

static void device_cb_handler(void)
{
    if (pdTRUE == ulTaskNotifyTake(pdTRUE, portMAX_DELAY)) {
        switch (_test_cdc_acm_mock_device_mode) {
        case TEST_CDC_ACM_MOCK_DEVICE_REMOTE_WAKE_BASIC:
            vTaskDelay(pdMS_TO_TICKS(1000));
            ESP_LOGI(CDC_DEV_TAG, "Triggering remote wakeup");
            tud_remote_wakeup();
            break;
        case TEST_CDC_ACM_MOCK_DEVICE_REMOTE_WAKE_SUDDEN_DISCONNECT:
            vTaskDelay(pdMS_TO_TICKS(1000));
            ESP_LOGI(CDC_DEV_TAG, "Triggering remote wakeup and disconnect");
            tud_remote_wakeup();
            tud_disconnect();

            vTaskDelay(pdMS_TO_TICKS(1000));
            ESP_LOGI(CDC_DEV_TAG, "Triggering connect");
            tud_connect();
            break;
        default:
            // No action
            break;
        }
    }
}

void cdc_acm_mock_device_set_mode(const enum test_cdc_acm_mock_device mode)
{
    _test_cdc_acm_mock_device_mode = mode;
}

void cdc_acm_mock_device_run(void)
{
    ESP_LOGI(CDC_DEV_TAG, "Initialization");
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = &cdc_device_descriptor;
    tusb_cfg.descriptor.full_speed_config = cdc_fs_desc_configuration;
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.qualifier = &device_qualifier;
    tusb_cfg.descriptor.high_speed_config = cdc_hs_desc_configuration;
#endif // TUD_OPT_HIGH_SPEED

    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    tinyusb_config_cdcacm_t amc_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = &tinyusb_cdc_rx_callback,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL
    };

    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&amc_cfg));
#if (CONFIG_TINYUSB_CDC_COUNT > 1)
    amc_cfg.cdc_port = TINYUSB_CDC_ACM_1;
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&amc_cfg));
#endif

    main_task_hdl = xTaskGetCurrentTaskHandle();
    printf("USB initialization DONE\n");

    while (1) {
        device_cb_handler();
    }
}

// Called when USB bus is suspended
void tud_suspend_cb(bool remote_wakeup_en)
{
    switch (_test_cdc_acm_mock_device_mode) {
    case TEST_CDC_ACM_MOCK_DEVICE_WITH_TWO_IFACES:
        // No action
        break;
    case TEST_CDC_ACM_MOCK_DEVICE_REMOTE_WAKE_BASIC:
    case TEST_CDC_ACM_MOCK_DEVICE_REMOTE_WAKE_SUDDEN_DISCONNECT:
        TEST_ASSERT_NOT_NULL_MESSAGE(main_task_hdl, "Main task handle is NULL");
        xTaskNotifyGive(main_task_hdl);
        break;
    default:
        TEST_FAIL_MESSAGE("Invalid mock device mode");
    }
}

// Called when USB bus resumes
void tud_resume_cb(void)
{
    ESP_LOGI(CDC_DEV_TAG, "resumed");
}
