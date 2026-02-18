/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "unity_test_utils_memory.h"

extern void test_pd_fusb302_setup(void);
extern void test_pd_fusb302_teardown(void);

void app_main(void)
{
    unity_utils_setup_heap_record(80);
    unity_utils_set_leak_level(530);
    unity_run_menu();
}

/* setUp runs before every test */
void setUp(void)
{
    unity_utils_record_free_mem();
    test_pd_fusb302_setup();
}

/* tearDown runs after every test */
void tearDown(void)
{
    test_pd_fusb302_teardown();
    unity_utils_evaluate_leaks();
}
