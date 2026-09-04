/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the HAL mock callbacks
 */
void hcd_mock_register_hal_callbacks(void);

#ifdef __cplusplus
}
#endif
