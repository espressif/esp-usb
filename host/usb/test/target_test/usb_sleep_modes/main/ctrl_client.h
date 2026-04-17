/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include "esp_sleep.h"
#include "freertos/event_groups.h"

#define EVT_ALLOW_SLEEP      BIT0
#define EVT_TEST_FINISH      BIT1
#define EVT_CLIENT_CLOSE     BIT2

typedef struct {
    EventGroupHandle_t test_event_group;    /**< Event group handle for synchronizing with main test task */
    esp_sleep_mode_t sleep_mode;            /**< Sleep mode to be tested */
} test_context_t;

/**
 * @brief CTRL client verifying light and deep sleep modes
 */
void ctrl_client_sleep_task(void *arg);
