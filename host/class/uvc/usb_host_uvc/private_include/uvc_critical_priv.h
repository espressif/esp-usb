/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdatomic.h>

#include "freertos/FreeRTOS.h"
#include "esp_private/critical_section.h"

// TODO: Nice to have dedicated critical section macro for declaring extern lock, eg: DECLARE_EXT_CRIT_SECTION_LOCK
extern DECLARE_CRIT_SECTION_LOCK_IN_STRUCT(uvc_lock);

#define UVC_ENTER_CRITICAL()              esp_os_enter_critical(&uvc_lock)
#define UVC_EXIT_CRITICAL()               esp_os_exit_critical(&uvc_lock)

#define UVC_ATOMIC_LOAD(x)                __atomic_load_n(&x, __ATOMIC_SEQ_CST)
#define UVC_ATOMIC_SET_IF_NULL(x, new_x)  ({ \
                                              __typeof__(x) expected = NULL; \
                                              __atomic_compare_exchange_n(&(x), &expected, (new_x), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); \
                                          })
