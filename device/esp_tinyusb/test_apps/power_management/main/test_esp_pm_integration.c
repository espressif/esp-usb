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
#include "driver/uart.h"
#include "driver/uart_wakeup.h"
#include "esp_private/sleep_event.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tusb_config.h"
#include "common_pm.h"

static const char *TAG = "PM_EspPM";

/**
 * @brief Set when a light sleep exit was caused by the console UART
 */
static volatile bool s_uart_light_sleep_wakeup;

/**
 * @brief Light sleep exit callback
 *
 * Runs on every light sleep exit and gets wake-up cause. If the wake-up source is UART, sets the wakeup flag
 *
 * @param[in] user_arg Unused
 * @param[in] ext_arg  Unused
 *
 * @return ESP_OK always
 */
static esp_err_t pm_exit_light_sleep_cb(void *user_arg, void *ext_arg)
{
    (void)user_arg;
    (void)ext_arg;
    if (esp_sleep_get_wakeup_causes() & BIT(ESP_SLEEP_WAKEUP_UART)) {
        s_uart_light_sleep_wakeup = true;
    }
    return ESP_OK;
}

static const esp_sleep_event_cb_config_t pm_exit_light_sleep_cb_cfg = {
    .cb = pm_exit_light_sleep_cb,
    .user_arg = NULL,
    // Lower priority value runs earlier; keep it distinct from the tinyusb wakeup callback (prior 2)
    .prior = 1,
    .next = NULL,
};

/**
 * @brief Enable the console UART as a light sleep wakeup source
 *
 * Uses the active-threshold mode, which is supported on every target that runs these PM tests.
 * The host (pytest) wakes the SoC from light sleep by sending characters over the console UART.
 */
static void pm_console_uart_wakeup_enable(void)
{
    const uart_wakeup_cfg_t uart_wakeup_cfg = {
        .wakeup_mode = UART_WK_MODE_ACTIVE_THRESH,
        .rx_edge_threshold = PM_UART_WAKEUP_EDGE_THRESHOLD,
    };
    TEST_ASSERT_EQUAL(ESP_OK, uart_wakeup_setup(CONFIG_ESP_CONSOLE_UART_NUM, &uart_wakeup_cfg));
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_enable_uart_wakeup(CONFIG_ESP_CONSOLE_UART_NUM));

    s_uart_light_sleep_wakeup = false;
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_register_event_callback(SLEEP_EVENT_SW_EXIT_SLEEP, &pm_exit_light_sleep_cb_cfg));
}

/**
 * @brief Disable the console UART light sleep wakeup source
 */
static void pm_console_uart_wakeup_disable(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_unregister_event_callback(SLEEP_EVENT_SW_EXIT_SLEEP, pm_exit_light_sleep_cb));
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_UART));
    uart_wakeup_clear(CONFIG_ESP_CONSOLE_UART_NUM, UART_WK_MODE_ACTIVE_THRESH);
}

/**
 * @brief Wait for a non-USB (UART) light sleep wakeup while verifying the PM lock stays released
 *
 * While the USB device is suspended the PM lock is released and the SoC enters automatic light
 * sleep. The host wakes it over the console UART. On every non-USB light sleep exit the PM lock
 * must stay released.
 */
static void pm_wait_light_sleep_uart_wakeup(void)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(PM_LIGHT_SLEEP_WAKE_WAIT_MS);

    // Ignore any UART wakeup from previous iteration; only this wait's wakeups count
    s_uart_light_sleep_wakeup = false;

    while (xTaskGetTickCount() < deadline) {
        // Block so tickless idle can actually enter light sleep and the exit callback can run
        vTaskDelay(pdMS_TO_TICKS(50));

        // The lock must remain released across every (non-USB) light sleep wakeup
        test_pm_assert_lock_acquired(false);

        if (s_uart_light_sleep_wakeup) {
            printf("Woken from light sleep by UART\n");
            break;
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(s_uart_light_sleep_wakeup, "Timed out waiting for light sleep UART wakeup");
}

/**
 * @brief PM-only suspend/resume cycles using UART light sleep wakeup and USB remote wakeup
 *
 * After host suspend the PM lock is released and the SoC may enter automatic light sleep.
 * The host wakes the SoC over the console UART (a non-USB wakeup). The device verifies that the
 * PM lock is still released after the wakeup, then signals USB remote wakeup to resume the bus
 * and re-acquire the PM lock.
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

        pm_wait_light_sleep_uart_wakeup();

        // A non-USB light sleep wakeup must not re-acquire the PM lock
        test_pm_assert_lock_acquired(false);

        TEST_ASSERT_EQUAL(ESP_OK, tinyusb_remote_wakeup());
        test_pm_assert_lock_acquired(true);
        expect_device_event(EVENT_BITS_RESUMED, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));

        test_iterations++;
    } while (test_iterations < SUSPEND_RESUME_TEST_ITERATIONS);

    ESP_LOGI(TAG, "PM light test cleanup");
    vTaskDelay(10);
}

/**
 * @brief Verify PM lock acquire/release around automatic light sleep with a non-USB (UART) wakeup
 *
 * Runs on all PM-capable targets, including those with USB OTG light sleep wakeup enabled, to
 * verify that a non-USB light sleep wakeup keeps the ESP_PM_NO_LIGHT_SLEEP lock released so that
 * automatic light sleep is not blocked until the host resumes the USB bus.
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

    pm_console_uart_wakeup_enable();
    run_esp_pm_light_sleep_suspend_resume_cycles();
    pm_console_uart_wakeup_disable();

#if SOC_PM_SUPPORT_CNNT_PD
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_pd_config(ESP_PD_DOMAIN_CNNT, ESP_PD_OPTION_OFF));
#endif // SOC_PM_SUPPORT_CNNT_PD

    // esp32s2 does not support cpu retention
#if SOC_PM_SUPPORT_CPU_PD
    // esp_pm_configure({light_sleep_enable: true}) calls cpu retention init, we need to deinit manually to prevent memory leaks
    TEST_ASSERT_EQUAL(ESP_OK, esp_sleep_cpu_retention_deinit());
#endif // SOC_PM_SUPPORT_CPU_PD
}

#if CONFIG_TINYUSB_USB_OTG_WAKEUP

/**
 * @brief Host-driven suspend/resume cycles woken from light sleep by USB activity
 *
 * The USB bus is resumed by host CDC traffic. On targets with USB OTG light sleep wakeup the host
 * traffic wakes the SoC from automatic light sleep through the USB peripheral, and the resulting
 * USB wakeup re-acquires the PM lock so the bus can stay resumed.
 */
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

    // Wait for the last auto suspend to finish the pytest. This time the host closes the CDC port, so it may report
    // remote wakeup as either enabled (normal autosuspend) or disabled (port close) depending on host behavior
    expect_any_device_event(EVENT_BITS_SUSPENDED_REMOTE_WAKE_EN | EVENT_BITS_SUSPENDED_REMOTE_WAKE_DIS, pdMS_TO_TICKS(DEVICE_EVENT_WAIT_MS));
    test_pm_assert_lock_acquired(false);
}

/**
 * @brief Verify PM lock re-acquire when the USB bus is resumed by the host (USB light sleep wakeup)
 *
 * Complements `tinyusb_power_management` (non-USB UART wakeup): here a USB wakeup must re-acquire
 * the ESP_PM_NO_LIGHT_SLEEP lock, while there a non-USB wakeup must leave it released.
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
