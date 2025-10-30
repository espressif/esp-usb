/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>
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
    //  ____ ___  ___________________    __                   __
    // |    |   \/   _____/\______   \ _/  |_  ____   _______/  |_
    // |    |   /\_____  \  |    |  _/ \   __\/ __ \ /  ___/\   __\.
    // |    |  / /        \ |    |   \  |  | \  ___/ \___ \  |  |
    // |______/ /_______  / |______  /  |__|  \___  >____  > |__|
    //                  \/         \/             \/     \/
    printf(" ____ ___  ___________________    __                   __   \r\n");
    printf("|    |   \\/   _____/\\______   \\ _/  |_  ____   _______/  |_ \r\n");
    printf("|    |   /\\_____  \\  |    |  _/ \\   __\\/ __ \\ /  ___/\\   __\\\r\n");
    printf("|    |  / /        \\ |    |   \\  |  | \\  ___/ \\___ \\  |  |  \r\n");
    printf("|______/ /_______  / |______  /  |__|  \\___  >____  > |__|  \r\n");
    printf("                 \\/         \\/             \\/     \\/        \r\n");

    unity_utils_setup_heap_record(80);
    /* Memory leakage level is different in different esp-idf releases */
    /* <= 6.0: 530 */
    /*    6.1: 548 */
    unity_utils_set_leak_level(548);
    unity_run_menu();
}
