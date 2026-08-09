/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity_test_runner.h"
#include "unity_test_utils_memory.h"

void setUp(void)
{
    unity_utils_record_free_mem();
}

void tearDown(void)
{
    vTaskDelay(10);
    unity_utils_evaluate_leaks();
}

void app_main(void)
{
    printf("esp_tinyusb runtime composite test\n");
    unity_utils_setup_heap_record(80);
    unity_utils_set_leak_level(256);
    unity_run_menu();
}
