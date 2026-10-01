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
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "unity.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "tusb_config.h"
#include "common_pm.h"
#include "sdkconfig.h"

const static char *TAG = "PM_Device";

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
TEST_CASE("tinyusb_suspend_resume_events", "[device_events][tinyusb_suspend_resume_events]")
{
    // Install and initialize cdc
    const test_pm_install_opts_t opts = {
        .auto_light_sleep_enable = false,
        .pm_lock_enable = false,
    };
    test_pm_init_tinyusb_cdc(&opts);

    uint8_t buf[TINYUSB_CDC_RX_BUFSIZE + 1];
    const char expect_reply[] = "Time to resume\r\n";
    const char send_message[] = "Time to suspend\r\n";
    SemaphoreHandle_t rx_data_sem = test_pm_get_rx_sem();

    // Expect attach event
    expect_device_event(EVENT_BITS_ATTACHED, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));

    int test_iterations = 0;
    do {
        expect_device_event(EVENT_BITS_SUSPENDED_REMOTE_WAKE_EN, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));
        // Wait for new data from the host (Sent by pytest)
        if (pdTRUE == xSemaphoreTake(rx_data_sem, pdMS_TO_TICKS(DATA_RECEPTION_WAIT_MS))) {

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

    // Wait for the last auto suspend to finish the pytest. This time the host closes the CDC port, so it may report
    // remote wakeup as either enabled (normal autosuspend) or disabled (port close) depending on host behavior
    expect_any_device_event(EVENT_BITS_SUSPENDED_REMOTE_WAKE_EN | EVENT_BITS_SUSPENDED_REMOTE_WAKE_DIS, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));
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
TEST_CASE("tinyusb_remote_wakeup_reporting", "[device_events][tinyusb_remote_wakeup_reporting]")
{
    // Call esp_tinyusb public API before installing the Tinyusb
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, tinyusb_remote_wakeup());

    // Install and initialize cdc
    const test_pm_install_opts_t opts = {
        .pm_lock_enable = true,
        .auto_light_sleep_enable = true,
    };
    test_pm_init_tinyusb_cdc(&opts);

    // Call esp_tinyusb public API when not suspended
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_ALLOWED, tinyusb_remote_wakeup());

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
 * @brief TinyUSB public API behavior in suspended state
 *
 * Tests CDC ACM read/write/flush API error handling while the device is suspended
 *
 * - Install device and wait for attach followed by auto suspend
 * - Verify read returns no data while suspended
 * - Verify write queue accepts data while suspended
 * - Verify write flush returns ESP_OK and queues data while suspended
 * - Pytest resumes the device by accessing it
 * - Verify received data and flush queued data after resume
 * - Wait for auto suspend
 */
TEST_CASE("tinyusb_cdc_public_api_error_handling", "[esp_tinyusb][cdc_device_pm_public_api]")
{
    // Install and initialize cdc
    const test_pm_install_opts_t opts = {
        .pm_lock_enable = false,
        .auto_light_sleep_enable = false,
    };
    test_pm_init_tinyusb_cdc(&opts);

    // Wait for attach event
    expect_device_event(EVENT_BITS_ATTACHED, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));
    // Expect auto suspend event with remote wakeup enabled by default
    // pytest opens a tty device (not just pyusb) to interact with the device thus Linux host cdc-acm driver automatically enables remote wakeup
    expect_device_event(EVENT_BITS_SUSPENDED_REMOTE_WAKE_EN, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));

    size_t rx_size = 0;
    uint8_t buf[TINYUSB_CDC_RX_BUFSIZE + 1];
    const char send_message[] = "llo from suspended state\r\n";
    const char expect_reply[] = "Time to resume\r\n";

    // Device must be read from from suspended state
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, buf, TINYUSB_CDC_RX_BUFSIZE, &rx_size));
    TEST_ASSERT(!rx_size);

    // Buffer must be queued even if in suspended state
    TEST_ASSERT_EQUAL_INT16(1, tinyusb_cdcacm_write_queue_char(TINYUSB_CDC_ACM_0, 'H'));
    TEST_ASSERT_EQUAL_INT16(1, tinyusb_cdcacm_write_queue_char(TINYUSB_CDC_ACM_0, 'e'));
    TEST_ASSERT_EQUAL_INT16(sizeof(send_message) - 1, tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, ((uint8_t *)send_message), sizeof(send_message) - 1));
    // Buffer shall be be flushed in suspended state (in tinyusb v 0.20, ESP_ERR_NOT_FINISHED was expected)
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0));

    // Pytest resumes the device by accessing it
    expect_device_event(EVENT_BITS_RESUMED, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));

    SemaphoreHandle_t rx_data_sem = test_pm_get_rx_sem();
    // Expect data from the host
    if (pdTRUE == xSemaphoreTake(rx_data_sem, pdMS_TO_TICKS(10000))) {

        TEST_ASSERT_EQUAL(ESP_OK, tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, buf, TINYUSB_CDC_RX_BUFSIZE, &rx_size));
        TEST_ASSERT_MESSAGE(rx_size, "No data received from host");
        ESP_LOGI(TAG, "Intf %d, RX %d bytes", TINYUSB_CDC_ACM_0, rx_size);
        // Check if received string is equal to expect_reply string
        TEST_ASSERT_EQUAL_UINT8_ARRAY(expect_reply, buf, sizeof(expect_reply) - 1);
    } else {
        TEST_FAIL_MESSAGE("RX Data CB not received on time");
    }

    // Write queued data
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0));

    // Wait for auto suspend
    expect_device_event(EVENT_BITS_SUSPENDED_REMOTE_WAKE_EN, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));
    vTaskDelay(10);
}

/**
 * @brief TinyUSB driver init/deinit while device is suspended
 *
 * Tests that the TinyUSB driver can be safely deinitialized and reinitialized
 * while the USB device remains in suspended state
 *
 * - Install device and wait for attach followed by auto suspend
 * - Deinit CDC and uninstall TinyUSB driver while suspended
 * - Reinstall TinyUSB driver and reinit CDC while still suspended
 */
TEST_CASE("tinyusb_init_deinit_from_suspended", "[esp_tinyusb][driver_init_deinit]")
{
    // Install and initialize cdc
    const test_pm_install_opts_t opts = {
        .pm_lock_enable = false,
        .auto_light_sleep_enable = false,
    };
    test_pm_init_tinyusb_cdc(&opts);

    // Wait for attach event
    expect_device_event(EVENT_BITS_ATTACHED, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));
    // Expect auto suspend event with remote wakeup disabled by default
    // pytest never opend tty device, just uses pyusb to interact with the device thus Linux host cdc-acm driver does not automatically enable remote wakeup
    expect_device_event(EVENT_BITS_SUSPENDED_REMOTE_WAKE_DIS, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));

    // Deinit and uninstall the driver while the device is in suspended state
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_cdcacm_deinit(TINYUSB_CDC_ACM_0));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());

    // Let the driver to be uninstalled
    vTaskDelay(10);

    // Install the driver and init the device while in suspended state
    test_pm_init_tinyusb_cdc(&opts);

    // Let the driver to be installed
    vTaskDelay(10);
}

#if CONFIG_TINYUSB_USB_OTG_WAKEUP
TEST_CASE("tinyusb_light_sleep_usb_wakeup", "[device_pm][tinyusb_light_sleep_otg_wake]")
{
    // Enable HS Connect power domain to stay ON during light sleep
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_pd_config(ESP_PD_DOMAIN_CNNT, ESP_PD_OPTION_ON));

    // Install and initialize cdc
    const test_pm_install_opts_t opts = {
        .pm_lock_enable = false,
        .auto_light_sleep_enable = false,
    };
    test_pm_init_tinyusb_cdc(&opts);

    // Expect attach and suspend events
    expect_device_event(EVENT_BITS_ATTACHED, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));
    expect_device_event(EVENT_BITS_SUSPENDED_REMOTE_WAKE_EN, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));

    // Prepare for light sleep entry
    printf("LIGHT_SLEEP_ENTER\n");
    fflush(stdout);
    vTaskDelay(10);

    // Enter light sleep (OTG suspend state is prepared by the light-sleep enter callback)
    TEST_ASSERT_EQUAL(ESP_OK, esp_light_sleep_start());

    // Pytest wakes up the SoC, find out the wakeup reason
    const uint32_t wakeup_causes = esp_sleep_get_wakeup_causes();
    TEST_ASSERT_TRUE_MESSAGE(wakeup_causes & BIT(ESP_SLEEP_WAKEUP_USB), "Expected to wake from USB during light sleep");

    // Expect resume event (OTG state is restored by the light-sleep exit callback)
    expect_device_event(EVENT_BITS_RESUMED, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));

    // Disable HS Connect power domain
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_pd_config(ESP_PD_DOMAIN_CNNT, ESP_PD_OPTION_OFF));

    SemaphoreHandle_t rx_data_sem = test_pm_get_rx_sem();
    // Wait for the data from the Pytest to be received
    TEST_ASSERT_EQUAL_MESSAGE(pdTRUE, xSemaphoreTake(rx_data_sem, pdMS_TO_TICKS(DATA_RECEPTION_WAIT_MS)), "Wake-up string not received on time from host");

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
}
#endif // CONFIG_TINYUSB_USB_OTG_WAKEUP

#endif // SOC_USB_OTG_SUPPORTED
