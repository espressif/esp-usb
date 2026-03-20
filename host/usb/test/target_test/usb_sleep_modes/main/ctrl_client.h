/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#define EVT_ALLOW_SLEEP      BIT0
#define EVT_TEST_FINISH      BIT1

/**
 * @brief Client task
 */
void ctrl_client_task(void *arg);
