/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdatomic.h>

#include "freertos/FreeRTOS.h"
#include "esp_private/critical_section.h"

// On single-core targets the lock does not exist (esp_os_enter_critical() only disables interrupts),
// so the extern declaration must be guarded the same way DEFINE_CRIT_SECTION_LOCK() guards the definition.
#if OS_SPINLOCK == 1
extern esp_os_spinlock_t uvc_lock;
#endif
#define UVC_ENTER_CRITICAL()              esp_os_enter_critical(&uvc_lock)
#define UVC_EXIT_CRITICAL()               esp_os_exit_critical(&uvc_lock)


#define UVC_ATOMIC_LOAD(x)                __atomic_load_n(&x, __ATOMIC_SEQ_CST)
#define UVC_ATOMIC_SET_IF_NULL(x, new_x)  ({ \
                                              __typeof__(x) expected = NULL; \
                                              __atomic_compare_exchange_n(&(x), &expected, (new_x), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); \
                                          })
