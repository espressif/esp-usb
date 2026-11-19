/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "sdkconfig.h"

/*
 * Section attribute for functions that run in the USB Host interrupt handler.
 *
 * Put this on the defining declaration only. IRAM_ATTR expands with __COUNTER__,
 * so repeating it on a forward declaration selects a different section.
 */
#if CONFIG_USB_HOST_ISR_HANDLER_IN_IRAM
#include "esp_attr.h"
#define USB_HOST_ISR_ATTR IRAM_ATTR
#else
#define USB_HOST_ISR_ATTR
#endif
