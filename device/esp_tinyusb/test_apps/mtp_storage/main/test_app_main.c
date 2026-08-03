/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "unity_test_utils_memory.h"

void setUp(void)
{
    unity_utils_record_free_mem();
}

void tearDown(void)
{
    unity_utils_evaluate_leaks();
}

void app_main(void)
{
    printf("TinyUSB MTP storage test app\n");
    unity_utils_setup_heap_record(80);
    unity_utils_set_leak_level(1600);
    unity_run_menu();
}
