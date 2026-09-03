/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"
#include "esp_log.h"
#include "hcd_common.h"

static const char *TAG = "API";

/*
Simulate interrupt allocation fail inside hcd_port_init()

Purpose:
    - Test HCD port public API
    - Test hcd_port_init() cleanup path

Procedure:
    - Setup HCD and wait for connection
    - Disconnect the device and teardown the HCD
    - Init HCD port init with an custom port config init, that would fail the interrupt allocation
    - Expect the hcd_port_init() to return error code, instead of a hard fault
    - Teardown
*/
TEST_CASE("Test HCD public API: Interrupt alloc failed", "[api][low_speed][full_speed][high_speed]")
{
    test_hcd_wait_for_conn(port_hdl);       // Trigger a connection
    vTaskDelay(pdMS_TO_TICKS(500));         // Short delay send of SOF (for FS) or EOPs (for LS)

    // Manually teardown the port, so we can initialize it with a custom config later
    test_hcd_wait_for_disconn(port_hdl, false);
    test_hcd_teardown(port_hdl);
    ESP_LOGI(TAG, "Port deinitialized");
    // Manually clear the public port handler
    port_hdl = NULL;

    // Initialize a port with the Highest interrupt flag priority
    // where the interrupt allocation would return INVALID_ARG error
    const hcd_port_config_t port_config = {
        .callback = NULL,
        .callback_arg = NULL,
        .context = NULL,
        .fifo_config = NULL,
        .intr_flags = ESP_INTR_FLAG_NMI,
    };
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, hcd_port_init(TEST_PORT_NUM, &port_config, &port_hdl));
    TEST_ASSERT_NULL(port_hdl);

    // Setup the port again, to test the stack recovery after a failed initialization
    port_hdl = test_hcd_setup();
    test_hcd_wait_for_conn(port_hdl);       // Trigger a connection
    ESP_LOGI(TAG, "Port initialized");
    vTaskDelay(pdMS_TO_TICKS(500));         // Short delay send of SOF (for FS) or EOPs (for LS)

    // Teardown the port
    test_hcd_wait_for_disconn(port_hdl, false);
}
