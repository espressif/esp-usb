/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <catch2/catch_test_macros.hpp>

#include "usb/cdc_acm_host.h"
#include "esp_system.h"

extern "C" {
#include "Mockcdc_host_common.h"
#include "Mockqueue.h"
#include "Mockportmacro.h"
}

static int sem_backing;
static QueueHandle_t sem = reinterpret_cast<QueueHandle_t>(&sem_backing);

/*
 * After the CDC-ACM refactor, `cdc_acm_host_install()` / `cdc_acm_host_uninstall()`
 * are thin wrappers on top of the common CDC host driver. They are exercised here
 * against a CMock-generated mock of `cdc_host_common.h` so this test stays focused
 * on the CDC-ACM layer's own bookkeeping (allocation, own mutex, `p_cdc_acm_obj`
 * lifecycle) and does not have to track the internal FreeRTOS/USB Host calls
 * performed inside `cdc_host_common`.
 */
SCENARIO("CDC-ACM Host install/uninstall")
{
    GIVEN("driver not installed") {

        SECTION("Uninstall while not installed returns INVALID_STATE") {
            vPortEnterCritical_Expect();
            vPortExitCritical_Expect();
            REQUIRE(ESP_ERR_INVALID_STATE == cdc_acm_host_uninstall());
        }

        SECTION("Install fails when cdc_host_common_acquire fails") {
            xQueueCreateMutex_ExpectAnyArgsAndReturn(sem);
            cdc_host_common_acquire_ExpectAnyArgsAndReturn(ESP_ERR_NO_MEM);
            vQueueDelete_Expect(sem);
            REQUIRE(ESP_ERR_NO_MEM == cdc_acm_host_install(nullptr));
        }

        SECTION("Install fails when cdc_host_common_register_dev_event_cb fails") {
            xQueueCreateMutex_ExpectAnyArgsAndReturn(sem);
            cdc_host_common_acquire_ExpectAnyArgsAndReturn(ESP_OK);
            cdc_host_common_register_dev_event_cb_ExpectAnyArgsAndReturn(ESP_ERR_NO_MEM);
            cdc_host_common_release_ExpectAnyArgsAndReturn(ESP_OK);
            vQueueDelete_Expect(sem);
            REQUIRE(ESP_ERR_NO_MEM == cdc_acm_host_install(nullptr));
        }

        // Must be the LAST section in this GIVEN so that `p_cdc_acm_obj` stays
        // set for the "driver installed" GIVEN below.
        SECTION("Install succeeds") {
            xQueueCreateMutex_ExpectAnyArgsAndReturn(sem);
            cdc_host_common_acquire_ExpectAnyArgsAndReturn(ESP_OK);
            cdc_host_common_register_dev_event_cb_ExpectAnyArgsAndReturn(ESP_OK);
            vPortEnterCritical_Expect();
            vPortExitCritical_Expect();
            REQUIRE(ESP_OK == cdc_acm_host_install(nullptr));
        }
    }

    GIVEN("driver installed") {

        SECTION("Re-install returns INVALID_STATE") {
            REQUIRE(ESP_ERR_INVALID_STATE == cdc_acm_host_install(nullptr));
        }

        SECTION("Uninstall succeeds") {
            vPortEnterCritical_Expect();
            vPortExitCritical_Expect();
            xQueueSemaphoreTake_ExpectAndReturn(sem, portMAX_DELAY, pdTRUE);
            vPortEnterCritical_Expect();
            vPortExitCritical_Expect();
            cdc_host_common_unregister_dev_event_cb_ExpectAnyArgsAndReturn(ESP_OK);
            cdc_host_common_release_ExpectAnyArgsAndReturn(ESP_OK);
            xQueueGenericSend_ExpectAnyArgsAndReturn(pdTRUE);
            vQueueDelete_Expect(sem);
            REQUIRE(ESP_OK == cdc_acm_host_uninstall());
        }
    }
}
