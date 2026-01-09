/*
 * SPDX-FileCopyrightText: 2015-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include "hcd.h"                // For remote waekup HAL support macro

typedef struct {
    int num_ctrl_xfer_to_send;
} ctrl_client_test_param_t;

void ctrl_client_async_seq_task(void *arg);

#ifdef REMOTE_WAKE_HAL_SUPPORTED
void ctrl_client_remote_wake_task(void *arg);
#endif // REMOTE_WAKE_HAL_SUPPORTED
