/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "soc/soc_caps.h"

#if SOC_USB_OTG_SUPPORTED

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "unity.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "tusb_config.h"
#include "sdkconfig.h"
#include "common_pm.h"

#if (CONFIG_PM_ENABLE)
#include "esp_pm.h"
#endif // CONFIG_PM_ENABLE

// ----------------------------------------------- Static declarations -------------------------------------------------

static const char *TAG = "PM_Common";

static char err_msg_buf[128];

// Static event group for device event delivery
static EventGroupHandle_t device_event_group = NULL;
static StaticEventGroup_t device_event_group_buffer;
// Static binary semaphore for RX data callback
static SemaphoreHandle_t rx_data_sem = NULL;
static StaticSemaphore_t rx_data_sem_buffer;

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

static tinyusb_config_cdcacm_t acm_cfg = {
    .cdc_port = TINYUSB_CDC_ACM_0,
    .callback_rx = NULL,
    .callback_rx_wanted_char = NULL,
    .callback_line_state_changed = NULL,
    .callback_line_coding_changed = NULL
};

static const uint16_t cdc_desc_config_len = TUD_CONFIG_DESC_LEN + CFG_TUD_CDC * TUD_CDC_DESC_LEN;
static const uint8_t cdc_desc_configuration_remote_wakeup[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, cdc_desc_config_len, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(0, 4, 0x81, 8, 0x02, 0x82, (TUD_OPT_HIGH_SPEED ? 512 : 64)),
};

// ---------------------------------------------------- Private API -----------------------------------------------------

/**
 * @brief CDC Device RX callback
 */
static void tinyusb_cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    if (rx_data_sem != NULL) {
        ESP_LOGI(TAG, "RX data cb");
        xSemaphoreGive(rx_data_sem);
    }
}

/**
 * @brief Device event handler providing event group bits
 */
static void device_event_handler(tinyusb_event_t *event, void *arg)
{
    uint32_t event_bits = UINT32_MAX;

    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        printf("TINYUSB_EVENT_ATTACHED\n");
        event_bits = EVENT_BITS_ATTACHED;
        break;
    case TINYUSB_EVENT_DETACHED:
        printf("TINYUSB_EVENT_DETACHED\n");
        event_bits = EVENT_BITS_DETACHED;
        break;
#if CONFIG_TINYUSB_SUSPEND_CALLBACK
    case TINYUSB_EVENT_SUSPENDED:
        if (event->suspended.remote_wakeup) {
            printf("TINYUSB_EVENT_SUSPENDED_REMOTE_WAKE_EN\n");
            event_bits = EVENT_BITS_SUSPENDED_REMOTE_WAKE_EN;
        } else {
            printf("TINYUSB_EVENT_SUSPENDED_REMOTE_WAKE_DIS\n");
            event_bits = EVENT_BITS_SUSPENDED_REMOTE_WAKE_DIS;
        }
        break;
#endif // CONFIG_TINYUSB_SUSPEND_CALLBACK
#if CONFIG_TINYUSB_RESUME_CALLBACK
    case TINYUSB_EVENT_RESUMED:
        printf("TINYUSB_EVENT_RESUMED\n");
        event_bits = EVENT_BITS_RESUMED;
        break;
#endif // CONFIG_TINYUSB_RESUME_CALLBACK
    default:
        return;
    }

    if (device_event_group != NULL) {
        xEventGroupSetBits(device_event_group, event_bits);
    }
}

/**
 * @brief Expect device event
 *
 * @param[in] expected_event Expected device event
 * @param[in] ticks time to expect the event
 * @param[in] file file from which the function was called
 * @param[in] line line from which the function was called
 */
void expect_device_event_impl(const uint32_t expected_event, TickType_t ticks, const char *file, int line)
{
    const EventBits_t bits = xEventGroupWaitBits(device_event_group, expected_event, pdTRUE, pdTRUE, ticks);
    if ((bits & expected_event) == expected_event) {
        return;
    }

    if (bits != 0) {
        snprintf(err_msg_buf, sizeof(err_msg_buf),
                 "Unexpected event at %s:%d\n %ld expected, %ld delivered\n",
                 file, line, expected_event, (uint32_t)bits);
        TEST_FAIL_MESSAGE(err_msg_buf);
    }

    snprintf(err_msg_buf, sizeof(err_msg_buf),
             "Event %ld at %s:%d\n was not delivered on time",
             expected_event, file, line);
    TEST_FAIL_MESSAGE(err_msg_buf);
}

void expect_any_device_event_impl(const uint32_t expected_events, TickType_t ticks, const char *file, int line)
{
    // Wait for ANY of the expected event bits (unlike expect_device_event_impl, which requires all).
    const EventBits_t bits = xEventGroupWaitBits(device_event_group, expected_events, pdTRUE, pdFALSE, ticks);
    if ((bits & expected_events) != 0) {
        return;
    }

    snprintf(err_msg_buf, sizeof(err_msg_buf), "None of events 0x%lx at %s:%d\n was delivered on time", (uint32_t)expected_events, file, line);
    TEST_FAIL_MESSAGE(err_msg_buf);
}

SemaphoreHandle_t test_pm_get_rx_sem(void)
{
    return rx_data_sem;
}

/**
 * @brief Initialize freertos power management
 */
static void test_esp_pm_init(const bool light_sleep_enable)
{
#if CONFIG_TINYUSB_PM
    esp_pm_config_t pm_config = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .light_sleep_enable = light_sleep_enable,
    };
    TEST_ASSERT_EQUAL(ESP_OK, esp_pm_configure(&pm_config));
#endif // CONFIG_TINYUSB_PM
}

/**
 * @brief Initialize freertos primitives for the test
 *
 * Event group bits for device events delivery (suspend, resume, attach, detach)
 * Semaphore for RX callback notifications
 */
static void test_pm_sync_init(void)
{
    if (device_event_group == NULL) {
        device_event_group = xEventGroupCreateStatic(&device_event_group_buffer);
    }
    xEventGroupClearBits(device_event_group, DEVICE_EVENT_BITS_ALL);

    if (rx_data_sem == NULL) {
        rx_data_sem = xSemaphoreCreateBinaryStatic(&rx_data_sem_buffer);
    }
    while (xSemaphoreTake(rx_data_sem, 0) == pdTRUE) {
    }
}

// ---------------------------------------------------- Public API -----------------------------------------------------

/**
 * @brief Install TinyUSB driver and initialize CDC ACM for PM tests
 *
 * @param[in] rx_cb     Optional CDC RX callback (may be NULL)
 *
 * @return tinyusb_config_t used for driver install (for reuse on reinstall)
 */
tinyusb_config_t test_pm_init_tinyusb_cdc(const test_pm_install_opts_t *opts)
{
    // Initialize freertos primitives
    test_pm_sync_init();

    // Initialize ESP_PM module
    test_esp_pm_init(opts->auto_light_sleep_enable);

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(device_event_handler);
    tusb_cfg.descriptor.device = &cdc_device_descriptor;
    tusb_cfg.descriptor.full_speed_config = cdc_desc_configuration_remote_wakeup;
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.qualifier = &device_qualifier;
    tusb_cfg.descriptor.high_speed_config = cdc_desc_configuration_remote_wakeup;
#endif // TUD_OPT_HIGH_SPEED

    tusb_cfg.pm_lock_enable = opts->pm_lock_enable;

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    acm_cfg.callback_rx = tinyusb_cdc_rx_callback;
    TEST_ASSERT_FALSE(tinyusb_cdcacm_initialized(TINYUSB_CDC_ACM_0));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_cdcacm_init(&acm_cfg));
    TEST_ASSERT_TRUE(tinyusb_cdcacm_initialized(TINYUSB_CDC_ACM_0));

    return tusb_cfg;
}

void test_pm_assert_lock_acquired(bool expected_acquired)
{
#if CONFIG_TINYUSB_PM
    bool lock_held;
    const esp_err_t err = tinyusb_pm_get_lock_status(&lock_held);

    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, err, "Tinyusb PM lock read error");
    TEST_ASSERT_EQUAL_MESSAGE(expected_acquired, lock_held, "Tinyusb PM lock assert error");
#else
    (void)expected_acquired;
    TEST_FAIL_MESSAGE("TinyUSB PM lock Not available");
#endif // CONFIG_TINYUSB_PM
}

#endif // SOC_USB_OTG_SUPPORTED
