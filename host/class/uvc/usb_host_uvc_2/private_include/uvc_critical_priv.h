/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

extern portMUX_TYPE uvc_lock;
#define UVC_ENTER_CRITICAL()   portENTER_CRITICAL(&uvc_lock)
#define UVC_EXIT_CRITICAL()    portEXIT_CRITICAL(&uvc_lock)
