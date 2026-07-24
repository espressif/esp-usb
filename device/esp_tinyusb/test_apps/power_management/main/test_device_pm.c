/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "soc/soc_caps.h"
#if SOC_USB_OTG_SUPPORTED

#include <stdio.h>
#include <string.h>
#include "esp_system.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_bit_defs.h"

#include "unity.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "tusb_config.h"
#include "sdkconfig.h"

#if (SOC_PM_SUPPORT_USB_WAKEUP && SOC_PM_SUPPORT_CNNT_PD)
#include "esp_rom_serial_output.h"
#endif

#define TINYUSB_CDC_RX_BUFSIZE                  CONFIG_TINYUSB_CDC_RX_BUFSIZE
#define SUSPEND_RESUME_TEST_ITERATIONS          5
#define DEVICE_EVENT_WAIT_MS                    5000

#define EVENT_BITS_ATTACHED                     BIT0   /**< Device attached event */
#define EVENT_BITS_SUSPENDED_REMOTE_WAKE_EN     BIT1   /**< Device suspended with remote wakeup enabled event */
#define EVENT_BITS_SUSPENDED_REMOTE_WAKE_DIS    BIT2   /**< Device suspended with remote wakeup disabled event */
#define EVENT_BITS_RESUMED                      BIT3   /**< Device resumed event */
#define DEVICE_EVENT_BITS_ALL   (EVENT_BITS_ATTACHED | EVENT_BITS_SUSPENDED_REMOTE_WAKE_EN | \
                                 EVENT_BITS_SUSPENDED_REMOTE_WAKE_DIS | EVENT_BITS_RESUMED)

static char err_msg_buf[128];
const static char *TAG = "PM_Device";

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

/**
 * @brief CDC Device RX callback for tinyusb_suspend_resume_events test case
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
        // No event bits for detached event
        return;
    case TINYUSB_EVENT_SUSPENDED:
        if (event->suspended.remote_wakeup) {
            printf("TINYUSB_EVENT_SUSPENDED_REMOTE_WAKE_EN\n");
            event_bits = EVENT_BITS_SUSPENDED_REMOTE_WAKE_EN;
        } else {
            printf("TINYUSB_EVENT_SUSPENDED_REMOTE_WAKE_DIS\n");
            event_bits = EVENT_BITS_SUSPENDED_REMOTE_WAKE_DIS;
        }
        break;
    case TINYUSB_EVENT_RESUMED:
        printf("TINYUSB_EVENT_RESUMED\n");
        event_bits = EVENT_BITS_RESUMED;
        break;
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
static void expect_device_event_impl(const uint32_t expected_event, TickType_t ticks, const char *file, int line)
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
#define expect_device_event(expected_event, ticks) expect_device_event_impl((expected_event), (ticks), __FILE__, __LINE__)

/**
 * @brief Install TinyUSB driver and initialize CDC ACM for PM tests
 *
 * @param[in] rx_cb     Optional CDC RX callback (may be NULL)
 *
 * @return tinyusb_config_t used for driver install (for reuse on reinstall)
 */
static tinyusb_config_t test_pm_init_tinyusb_cdc(tusb_cdcacm_callback_t rx_cb)
{
    // Initialize freertos primitives
    test_pm_sync_init();

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(device_event_handler);
    tusb_cfg.descriptor.device = &cdc_device_descriptor;
    tusb_cfg.descriptor.full_speed_config = cdc_desc_configuration_remote_wakeup;
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.qualifier = &device_qualifier;
    tusb_cfg.descriptor.high_speed_config = cdc_desc_configuration_remote_wakeup;
#endif // TUD_OPT_HIGH_SPEED

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    acm_cfg.callback_rx = rx_cb;
    TEST_ASSERT_FALSE(tinyusb_cdcacm_initialized(TINYUSB_CDC_ACM_0));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_cdcacm_init(&acm_cfg));
    TEST_ASSERT_TRUE(tinyusb_cdcacm_initialized(TINYUSB_CDC_ACM_0));

    return tusb_cfg;
}

/**
 * @brief Tinyusb power management suspend/resume events
 *
 * Tests TINYUSB_EVENT_SUSPENDED and TINYUSB_EVENT_RESUMED esp_tinyusb events
 *
 * Pytest expects TINYUSB_EVENT_SUSPENDED event - because of auto suspend
 * Pytest sends data to device to resume it
 * Device resumes, receives and validates the data, sends a response and goes to suspended state (auto suspend)
 * Pytest expect TINYUSB_EVENT_SUSPENDED ...
 */
TEST_CASE("tinyusb_suspend_resume_events", "[esp_tinyusb][device_pm_suspend_resume]")
{
    // Install and initialize cdc
    test_pm_init_tinyusb_cdc(tinyusb_cdc_rx_callback);

    uint8_t buf[TINYUSB_CDC_RX_BUFSIZE + 1];
    const char expect_reply[] = "Time to resume\r\n";
    const char send_message[] = "Time to suspend\r\n";

    // Expect attach event
    expect_device_event(EVENT_BITS_ATTACHED, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));

    int test_iterations = 0;
    do {
        expect_device_event(EVENT_BITS_SUSPENDED_REMOTE_WAKE_EN, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));
        // Wait for new data from the host (Sent by pytest)
        if (pdTRUE == xSemaphoreTake(rx_data_sem, pdMS_TO_TICKS(10000))) {

            expect_device_event(EVENT_BITS_RESUMED, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));
            size_t rx_size = 0;
            TEST_ASSERT_EQUAL(ESP_OK, tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, buf, TINYUSB_CDC_RX_BUFSIZE, &rx_size));
            if (rx_size > 0) {
                ESP_LOGI(TAG, "Intf %d, RX %d bytes", TINYUSB_CDC_ACM_0, rx_size);
                // Check if received string is equal to expect_reply string
                TEST_ASSERT_EQUAL_UINT8_ARRAY(expect_reply, buf, sizeof(expect_reply) - 1);

                // Reply to the host with send_message string
                strncpy((char *)buf, send_message, sizeof(send_message) - 1);
                tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, buf, sizeof(send_message) - 1);
                tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
                test_iterations++;
            }
        } else {
            TEST_FAIL_MESSAGE("RX Data CB not received on time");
        }
    } while (test_iterations < SUSPEND_RESUME_TEST_ITERATIONS);

    // Wait for the last auto suspend to finish the pytest
    expect_device_event(EVENT_BITS_SUSPENDED_REMOTE_WAKE_EN, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));
}

/**
 * @brief Tinyusb power management remote wakeup
 *
 * Tests device reporting remote wakeup capability
 *
 * - Install device with remote wakeup allowed in it's configuration descriptor, but disabled (by default after reset)
 * - Expect auto suspend device event with remote wakeup disabled
 * - Pytest enables the remote wakeup feature by a ctrl transfer
 * - Expect device resume event (because of ctrl transfer)
 * - Expect auto suspend device event with remote wakeup enabled
 * - Signalize remote wakeup and expect resume event
 */
TEST_CASE("tinyusb_remote_wakeup_reporting", "[esp_tinyusb][device_pm_remote_wake]")
{
    // Call esp_tinyusb public API before installing the Tinyusb
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, tinyusb_remote_wakeup());

    // Install and initialize cdc
    test_pm_init_tinyusb_cdc(NULL);

    // Expect attach event
    expect_device_event(EVENT_BITS_ATTACHED, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));
    // Expect auto suspend event with remote wakeup disabled by default
    // pytest never opend tty device, just uses pyusb to interact with the device thus Linux host cdc-acm driver does not automatically enable remote wakeup
    expect_device_event(EVENT_BITS_SUSPENDED_REMOTE_WAKE_DIS, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));

    // Try to signalize remote wakeup, when the host did not enable it
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, tinyusb_remote_wakeup());

    // Pytest enables remote wakeup on the device by sending a ctrl transfer to the the device
    // Expect the device to:
    //  - resumed (because of the ctrl transfer)
    //  - auto suspended with remote wakeup enabled

    expect_device_event(EVENT_BITS_RESUMED, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));
    expect_device_event(EVENT_BITS_SUSPENDED_REMOTE_WAKE_EN, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));

    // Signalize remote wakeup and expect resume event
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_remote_wakeup());
    expect_device_event(EVENT_BITS_RESUMED, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));
}

/**
 * @brief Tinyusb USB wakeup wrapper API validation
 *
 * Verifies that the public wrappers reject invalid states
 */
TEST_CASE("tinyusb_usb_wakeup_api", "[esp_tinyusb][device_esp_sleep][device_pm]")
{
    // Try to call esp_tinyusb public API without installing the driver, expect invalid state
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, tinyusb_set_otg_suspend_state(true));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, tinyusb_clear_otg_wakeup_status());

    // Install tinyusb
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));

#if SOC_PM_SUPPORT_USB_WAKEUP && SOC_USB_UTMI_PHY_NUM
    // Cal esp_sleep for HS port capable targets
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_enable_usb_wakeup());
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_disable_usb_wakeup());
    // Call esp_tinyusb public API, when HS port is in use
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_set_otg_suspend_state(true));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_set_otg_suspend_state(false));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_clear_otg_wakeup_status());

#if CONFIG_IDF_TARGET_ESP32P4
    // For esp32p4 uninstall the driver with the default config and install it again with FS port enabled
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());

    tusb_cfg = TINYUSB_CONFIG_FULL_SPEED(NULL, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    // Call esp_tinyusb public API, expect not supported error for esp32p4 FS port
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, tinyusb_set_otg_suspend_state(true));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, tinyusb_clear_otg_wakeup_status());
#endif // CONFIG_IDF_TARGET_ESP32P4
#else
    // Call esp_tinyusb public API, expect not supported error for FS port targets
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, tinyusb_set_otg_suspend_state(true));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, tinyusb_clear_otg_wakeup_status());
#endif // SOC_PM_SUPPORT_USB_WAKEUP && SOC_USB_UTMI_PHY_NUM
}

/**
 * @brief Tinyusb light sleep wakeup integration test
 *
 * Waits for USB suspend, enters light sleep with UTMI helpers enabled, and
 * expects the host to wake the SoC by accessing the CDC port.
 */
TEST_CASE("tinyusb_light_sleep_usb_wakeup", "[esp_tinyusb][device_esp_sleep_light_sleep]")
{
    // Ignore the test for USB FS port only targets
#if !(SOC_PM_SUPPORT_USB_WAKEUP && SOC_PM_SUPPORT_CNNT_PD)
    TEST_IGNORE_MESSAGE("USB light sleep wakeup is supported only on USB HS-port capable targets");
#else
    // Enable ESP sleep wakeup source and power domains
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_cpu_retention_init());
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_enable_usb_wakeup());
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_pd_config(ESP_PD_DOMAIN_CNNT, ESP_PD_OPTION_ON));

    // Install and initialize cdc
    test_pm_init_tinyusb_cdc(tinyusb_cdc_rx_callback);

    // Expect attach and suspend events
    expect_device_event(EVENT_BITS_ATTACHED, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));
    expect_device_event(EVENT_BITS_SUSPENDED_REMOTE_WAKE_EN, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));

    // Prepare for light sleep entry
    printf("LIGHT_SLEEP_ENTER\n");
    fflush(stdout);
    esp_rom_output_tx_wait_idle(CONFIG_ESP_CONSOLE_UART_NUM);

    // Enter light light sleep
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_set_otg_suspend_state(true));
    TEST_ASSERT_EQUAL(ESP_OK, esp_light_sleep_start());

    // Pytest wakes up the SoC, find out the wakeup reason
    const uint32_t wakeup_causes = esp_sleep_get_wakeup_causes();
    TEST_ASSERT_TRUE_MESSAGE(wakeup_causes & BIT(ESP_SLEEP_WAKEUP_USB),
                             "Expected to wake from USB during light sleep");

    // Expect resume event
    expect_device_event(EVENT_BITS_RESUMED, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));

    // Disable all light sleep related settings
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_set_otg_suspend_state(false));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_clear_otg_wakeup_status());
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_disable_usb_wakeup());
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_cpu_retention_deinit());

    // Wait for the data from the Pytest to be received
    if (pdTRUE != xSemaphoreTake(rx_data_sem, 0)) {
        TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(rx_data_sem, pdMS_TO_TICKS(10000)));
    }

    uint8_t buf[TINYUSB_CDC_RX_BUFSIZE + 1];
    size_t rx_size = 0;
    const char expect_message[] = "Light sleep wake\r\n";
    const char send_message[] = "Light sleep ok\r\n";

    // Read the data and validate
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, buf, TINYUSB_CDC_RX_BUFSIZE, &rx_size));
    TEST_ASSERT_GREATER_THAN(0, rx_size);
    ESP_LOGI(TAG, "Intf %d, RX %d bytes", TINYUSB_CDC_ACM_0, rx_size);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expect_message, buf, sizeof(expect_message) - 1);

    // Reply back
    strncpy((char *)buf, send_message, sizeof(send_message) - 1);
    TEST_ASSERT_EQUAL(sizeof(send_message) - 1, tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, buf, sizeof(send_message) - 1));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0));
    printf("LIGHT_SLEEP_DATA_RX\n");

#endif // SOC_PM_SUPPORT_USB_WAKEUP && SOC_PM_SUPPORT_CNNT_PD
}

#endif
