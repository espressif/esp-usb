/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "soc/soc_caps.h"
#include "sdkconfig.h"

#if SOC_USB_OTG_SUPPORTED && CONFIG_TINYUSB_PM

#include <stdio.h>
#include <string.h>
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "unity.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tusb_config.h"
#include "common_pm.h"

static const char *TAG = "PM_EspPM";

#if !CONFIG_TINYUSB_USB_OTG_WAKEUP

static void pm_wait_light_sleep_timer_wakeup(void)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(PM_LIGHT_SLEEP_WAKE_WAIT_MS);

    while (xTaskGetTickCount() < deadline) {
        const uint32_t causes = esp_sleep_get_wakeup_causes();
        if (causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) {
            printf("Woken form light sleep by timer\n");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    TEST_FAIL_MESSAGE("Timed out waiting for light sleep timer wakeup");
}

/**
 * @brief PM-only suspend/resume cycles using timer light sleep wakeup and USB remote wakeup
 *
 * After host suspend the PM lock is released and the SoC may enter automatic light sleep.
 * USB host traffic cannot wake FS targets from light sleep, so a sleep timer wakes the SoC.
 * The device then signals remote wakeup to resume the USB bus and re-acquire the PM lock.
 */
static void run_esp_pm_light_sleep_suspend_resume_cycles(void)
{
    expect_device_event(EVENT_BITS_ATTACHED, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));
    test_pm_assert_lock_acquired(true);

    int test_iterations = 0;
    do {
        ESP_LOGI(TAG, "PM light sleep iteration %d", test_iterations);
        expect_device_event(EVENT_BITS_SUSPENDED_REMOTE_WAKE_EN, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));
        test_pm_assert_lock_acquired(false);

        pm_wait_light_sleep_timer_wakeup();

        TEST_ASSERT_EQUAL(ESP_OK, tinyusb_remote_wakeup());
        test_pm_assert_lock_acquired(true);
        expect_device_event(EVENT_BITS_RESUMED, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));

        TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_enable_timer_wakeup(PM_LIGHT_SLEEP_TIMER_US));
        test_iterations++;
    } while (test_iterations < SUSPEND_RESUME_TEST_ITERATIONS);

    ESP_LOGI(TAG, "PM light test cleanup");
    vTaskDelay(10);
}

/**
 * @brief Verify PM lock acquire/release during host-driven suspend/resume (PM only, no USB wakeup)
 */
TEST_CASE("tinyusb_power_management", "[device_pm][tinyusb_pm]")
{
    const test_pm_install_opts_t opts = {
        .pm_lock_enable = true,
        .auto_light_sleep_enable = true,
    };
    test_pm_init_tinyusb_cdc(&opts);

#if SOC_PM_SUPPORT_CNNT_PD
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_pd_config(ESP_PD_DOMAIN_CNNT, ESP_PD_OPTION_ON));
#endif // SOC_PM_SUPPORT_CNNT_PD

    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_enable_timer_wakeup(PM_LIGHT_SLEEP_TIMER_US));
    run_esp_pm_light_sleep_suspend_resume_cycles();
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER));
#if SOC_PM_SUPPORT_CNNT_PD
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_pd_config(ESP_PD_DOMAIN_CNNT, ESP_PD_OPTION_OFF));
#endif // SOC_PM_SUPPORT_CNNT_PD

    // esp32s2 does not support cpu retention
#if SOC_PM_SUPPORT_CPU_PD
    // esp_pm_configure({light_sleep_enable: true}) calls cpu retention init, we need to deinit manually to prevent memory leaks
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_cpu_retention_deinit());
#endif // SOC_PM_SUPPORT_CPU_PD
}

#endif // !CONFIG_TINYUSB_USB_OTG_WAKEUP

#if CONFIG_TINYUSB_USB_OTG_WAKEUP

static void run_esp_pm_suspend_resume_cycles(void)
{
    const uint32_t suspend_event = EVENT_BITS_SUSPENDED_REMOTE_WAKE_EN;
    const uint32_t resume_event = EVENT_BITS_RESUMED;

    uint8_t buf[TINYUSB_CDC_RX_BUFSIZE + 1];
    const char expect_reply[] = "Time to resume\r\n";
    const char send_message[] = "Time to suspend\r\n";
    SemaphoreHandle_t rx_data_sem = test_pm_get_rx_sem();

    expect_device_event(EVENT_BITS_ATTACHED, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));
    test_pm_assert_lock_acquired(true);

    int test_iterations = 0;
    do {
        expect_device_event(suspend_event, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));
        test_pm_assert_lock_acquired(false);

        if (pdTRUE == xSemaphoreTake(rx_data_sem, pdMS_TO_TICKS(DATA_RECEPTION_WAIT_MS))) {

            expect_device_event(resume_event, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));
            test_pm_assert_lock_acquired(true);

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

    expect_device_event(suspend_event, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));
    test_pm_assert_lock_acquired(false);
}

/**
 * @brief Verify PM lock acquire/release during host-driven suspend/resume (esp_tinyusb callbacks)
 */
TEST_CASE("tinyusb_power_management_usb_wakeup", "[device_pm][tinyusb_pm_otg_wake]")
{
    const test_pm_install_opts_t opts = {
        .pm_lock_enable = true,
        .auto_light_sleep_enable = true,
    };
    test_pm_init_tinyusb_cdc(&opts);

    // Enable HS Connect power domain to stay ON during light sleep
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_pd_config(ESP_PD_DOMAIN_CNNT, ESP_PD_OPTION_ON));

    run_esp_pm_suspend_resume_cycles();

    // Disable HS Connect power domain
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_pd_config(ESP_PD_DOMAIN_CNNT, ESP_PD_OPTION_OFF));
    // esp_pm_configure({light_sleep_enable: true}) calls cpu retention init, we need to deinit manually to prevent memory leaks
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_cpu_retention_deinit());
    ESP_LOGI(TAG, "PM usb wakeup test cleanup");
}
#endif // CONFIG_TINYUSB_USB_OTG_WAKEUP

#endif // SOC_USB_OTG_SUPPORTED && CONFIG_TINYUSB_PM
