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

#define DEV_CB_EVT_SUSPEND_REMOTE_WAKE_DIS  (1U << 0)   /**< Device suspend callback with remote wakeup disabled */
#define DEV_CB_EVT_SUSPEND_REMOTE_WAKE_EN   (1U << 1)   /**< Device suspend callback with remote wakeup enabled */
#define DEV_CB_EVT_RESUME                   (1U << 2)   /**< Device resume callback */

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

/**
 * @brief Device callback handler
 *
 * Executes test actions based on the current device test mode and type of device callback
 */
static void device_cb_handler(const enum cdc_acm_mock_dev_mode test_dev_mode)
{
    uint32_t notify_bits;
    if (pdTRUE == xTaskNotifyWait(pdFALSE, UINT32_MAX, &notify_bits, portMAX_DELAY)) {
        switch (test_dev_mode) {
        case MOCK_DEV_DUAL_IFACE:
            // No action for this device mode
            break;

        case MOCK_DEV_EARLY_REMOTE_WAKE: {

            if (notify_bits & DEV_CB_EVT_RESUME) {
                TEST_FAIL_MESSAGE("We are not expecting the device to deliver resume callback in this test mode");
            }

            if (notify_bits & DEV_CB_EVT_SUSPEND_REMOTE_WAKE_DIS) {
                TEST_FAIL_MESSAGE("Device does not have remote wakeup feature enabled");
            }

            if (notify_bits & DEV_CB_EVT_SUSPEND_REMOTE_WAKE_EN) {
                // Device is now in suspended state with remote wakeup enabled
                // Start remote wakeup signaling immediately after delivering the suspend callback
                // The host should reject the remote wakeup within 5ms from issuing the suspend sequence
                esp_rom_delay_us(1000 * 5);
                tud_remote_wakeup();
                ESP_LOGI(CDC_DEV_TAG, "Triggering early remote wakeup");

                //esp_rom_delay_us(1000 * 100 * 5);
                //tud_remote_wakeup();
                //ESP_LOGI(CDC_DEV_TAG, "Triggering remote wakeup");
                break;
            }

            // Should not happen
            break;
        }

        case MOCK_DEV_REMOTE_WAKE:

            if (notify_bits & DEV_CB_EVT_RESUME) {
                // The host resumed the device, no action
                break;
            }

            if (notify_bits & DEV_CB_EVT_SUSPEND_REMOTE_WAKE_DIS) {
                TEST_FAIL_MESSAGE("Device does not have remote wakeup feature enabled");
            }

            if (notify_bits & DEV_CB_EVT_SUSPEND_REMOTE_WAKE_EN) {
                // Device is now in suspended state with remote wakeup enabled
                // Start remote wakeup signaling
                vTaskDelay(pdMS_TO_TICKS(1000));
                ESP_LOGI(CDC_DEV_TAG, "Triggering remote wakeup");
                tud_remote_wakeup();
                break;
            }

            // Should not happen
            break;

        case MOCK_DEV_REMOTE_WAKE_DISCONNECT:

            if (notify_bits & DEV_CB_EVT_RESUME) {
                TEST_FAIL_MESSAGE("We are not expecting the device to deliver resume callback in this test mode");
            }

            if (notify_bits & DEV_CB_EVT_SUSPEND_REMOTE_WAKE_DIS) {
                TEST_FAIL_MESSAGE("Device does not have remote wakeup feature enabled");
            }

            if (notify_bits & DEV_CB_EVT_SUSPEND_REMOTE_WAKE_EN) {
                // Device is now in suspended state with remote wakeup enabled
                // Start remote wakeup signaling followed by sudden disconnect
                vTaskDelay(pdMS_TO_TICKS(1000));
                ESP_LOGI(CDC_DEV_TAG, "Triggering remote wakeup and sudden disconnect");
                tud_remote_wakeup();
                tud_disconnect();

                // Stay disconnected for a while and trigger reconnect
                vTaskDelay(pdMS_TO_TICKS(1000));
                ESP_LOGI(CDC_DEV_TAG, "Triggering connect");
                tud_connect();
                break;
            }

            // Should not happen
            break;

        default:
            TEST_FAIL_MESSAGE("Unsupported test device mode");
            break;
        }
    }
}

void cdc_acm_mock_device_run(const enum cdc_acm_mock_dev_mode test_dev_mode)
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
        device_cb_handler(test_dev_mode);
    }
}

// Called when USB bus is suspended
void tud_suspend_cb(bool remote_wakeup_en)
{
    // Give notification to the main task, that the device callback has fired
    if (! main_task_hdl) {
        return;
    }

    const uint32_t notify_bits = (remote_wakeup_en) ?
                                 (DEV_CB_EVT_SUSPEND_REMOTE_WAKE_EN) :
                                 (DEV_CB_EVT_SUSPEND_REMOTE_WAKE_DIS);

    xTaskNotify(main_task_hdl, notify_bits, eSetBits);
    ESP_LOGI(CDC_DEV_TAG, "Suspended with remote wakeup %s", ( (remote_wakeup_en) ? ("enabled") : ("disabled") ));
}

// Called when USB bus resumes
void tud_resume_cb(void)
{
    if (! main_task_hdl) {
        return;
    }

    xTaskNotify(main_task_hdl, DEV_CB_EVT_RESUME, eSetBits);
    ESP_LOGI(CDC_DEV_TAG, "resumed");
}
