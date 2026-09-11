/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "soc/soc_caps.h"

#if SOC_USB_OTG_SUPPORTED

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_bit_defs.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tusb_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_EVENT_WAIT_MS                    5000    /** Maximum time to wait for a device event, in milliseconds. */
#define DATA_RECEPTION_WAIT_MS                  7000    /** Maximum time to wait for CDC RX data, in milliseconds. */
#define SUSPEND_RESUME_TEST_ITERATIONS          5       /** Number of host-driven suspend/resume iterations in PM loop tests. */
#define PM_LIGHT_SLEEP_WAKE_WAIT_MS             10000   /** Maximum time to wait for a light sleep wakeup, in milliseconds. */
#define PM_UART_WAKEUP_EDGE_THRESHOLD           6       /**< The active-threshold minimum is 3 on most targets but 6 on the ESP32-P4 HP UART */
#define TINYUSB_CDC_RX_BUFSIZE                  CONFIG_TINYUSB_CDC_RX_BUFSIZE

#define EVENT_BITS_ATTACHED                     BIT0    /**< Device attached event */
#define EVENT_BITS_SUSPENDED_REMOTE_WAKE_EN     BIT1    /**< Device suspended with remote wakeup enabled event */
#define EVENT_BITS_SUSPENDED_REMOTE_WAKE_DIS    BIT2    /**< Device suspended with remote wakeup disabled event */
#define EVENT_BITS_RESUMED                      BIT3    /**< Device resumed event */
#define EVENT_BITS_DETACHED                     BIT4    /**< Device detached event */

/** @brief Bit mask of all device event bits used by the PM test helpers. */
#define DEVICE_EVENT_BITS_ALL   (EVENT_BITS_ATTACHED |                  \
                                 EVENT_BITS_SUSPENDED_REMOTE_WAKE_EN  | \
                                 EVENT_BITS_SUSPENDED_REMOTE_WAKE_DIS | \
                                 EVENT_BITS_RESUMED |                   \
                                 EVENT_BITS_DETACHED)

/**
 * @brief Runtime options for installing TinyUSB in PM test cases
 */
typedef struct {
    bool pm_lock_enable;              /*!< Enable ESP_PM_NO_LIGHT_SLEEP lock in `tinyusb_config_t`. */
    bool auto_light_sleep_enable;     /*!< Call `esp_pm_configure()` with automatic light sleep enabled. */
} test_pm_install_opts_t;

/**
 * @brief Wait for an expected device event bit mask
 *
 * Fails the Unity test case when the expected bits are not received within the timeout
 * or when a different event bit mask is delivered first.
 *
 * @param[in] expected_event Event bit mask from `EVENT_BITS_*`
 * @param[in] ticks          FreeRTOS timeout in ticks
 * @param[in] file           File from which the function was called
 * @param[in] line           Line from which the function was called
 */
void expect_device_event_impl(uint32_t expected_event, TickType_t ticks, const char *file, int line);
#define expect_device_event(expected_event, ticks) expect_device_event_impl((expected_event), (ticks), __FILE__, __LINE__)

/**
 * @brief Wait for any one of an expected device event bit mask
 *
 * Like `expect_device_event`, but succeeds as soon as *any* of the bits in `expected_events` is delivered
 *
 * @param[in] expected_events Event bit mask from `EVENT_BITS_*` (any one of them matches)
 * @param[in] ticks           FreeRTOS timeout in ticks
 * @param[in] file            File from which the function was called
 * @param[in] line            Line from which the function was called
 */
void expect_any_device_event_impl(uint32_t expected_events, TickType_t ticks, const char *file, int line);
#define expect_any_device_event(expected_events, ticks) expect_any_device_event_impl((expected_events), (ticks), __FILE__, __LINE__)

/**
 * @brief Get the RX synchronization semaphore used by PM tests
 *
 * @return Binary semaphore signaled by `tinyusb_cdc_rx_callback()`
 */
SemaphoreHandle_t test_pm_get_rx_sem(void);

/**
 * @brief Install TinyUSB and CDC ACM using PM test helpers
 *
 * Initializes synchronization primitives, optionally configures esp_pm, installs the
 * driver with the shared descriptors, and initializes CDC ACM port 0.
 *
 * @param[in] opts Install options for PM lock and esp_pm configuration
 */
tinyusb_config_t test_pm_init_tinyusb_cdc(const test_pm_install_opts_t *opts);

/**
 * @brief Assert TinyUSB PM lock acquisition state
 *
 * Queries `tinyusb_pm_get_lock_status()` and verifies the lock state. Prints
 * `PM_LOCK_ACQUIRED=0/1` markers for pytest, or `PM_LOCK_UNAVAILABLE` when PM is
 * disabled in sdkconfig.
 *
 * @param[in] acquired Expected lock state: `true` if the lock should be held
 */
void test_pm_assert_lock_acquired(bool acquired);

#ifdef __cplusplus
}
#endif

#endif // SOC_USB_OTG_SUPPORTED
