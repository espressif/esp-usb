/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "tinyusb_types.h"
#include "tinyusb_task.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Default TinyUSB Driver configuration structure initializer
 *
 * Default port:
 * - ESP32P4:       USB OTG 2.0 (High-speed)
 * - ESP32S2/S3:    USB OTG 1.1 (Full-speed)
 *
 * Default size:
 * - 4096 bytes
 * Default priority:
 * - 5
 *
 * Default task affinity:
 * - Multicore:     CPU1
 * - Unicore:       CPU0
 *
 */
#if CONFIG_IDF_TARGET_ESP32P4
#define TINYUSB_DEFAULT_CONFIG()       TINYUSB_CONFIG_HIGH_SPEED()
#else
#define TINYUSB_DEFAULT_CONFIG()       TINYUSB_CONFIG_FULL_SPEED()
#endif

#if CONFIG_FREERTOS_UNICORE
#define TINYUSB_DEFAULT_TASK_AFFINITY  (0U)
#else
#define TINYUSB_DEFAULT_TASK_AFFINITY  (1U)
#endif // CONFIG_FREERTOS_UNICORE

// Default size for task stack used in TinyUSB task creation
#define TINYUSB_DEFAULT_TASK_SIZE      4096
// Default priority for task used in TinyUSB task creation
#define TINYUSB_DEFAULT_TASK_PRIO      5

#define TINYUSB_CONFIG_FULL_SPEED() \
    (tinyusb_config_t) {                              \
        .skip_phy_setup = false,                      \
        .port = TINYUSB_PORT_0,                       \
        .task = TINYUSB_TASK_DEFAULT(),               \
        .self_powered = false,                        \
        .vbus_monitor_io = -1,                        \
        .device_descriptor = NULL,                    \
        .string_descriptor = NULL,                    \
        .string_descriptor_count = 0,                 \
        .configuration_descriptor = NULL,             \
    }

#define TINYUSB_CONFIG_HIGH_SPEED() \
    (tinyusb_config_t) {                              \
        .skip_phy_setup = false,                      \
        .port = TINYUSB_PORT_1,                       \
        .task = TINYUSB_TASK_DEFAULT(),               \
        .self_powered = false,                        \
        .vbus_monitor_io = -1,                        \
        .device_descriptor = NULL,                    \
        .string_descriptor = NULL,                    \
        .string_descriptor_count = 0,                 \
        .fs_configuration_descriptor = NULL,          \
        .hs_configuration_descriptor = NULL,          \
        .qualifier_descriptor = NULL,                 \
    }

#define TINYUSB_TASK_DEFAULT() \
    (tinyusb_task_config_t) {                         \
        .size = TINYUSB_DEFAULT_TASK_SIZE,            \
        .priority = TINYUSB_DEFAULT_TASK_PRIO,        \
        .xCoreID = TINYUSB_DEFAULT_TASK_AFFINITY,    \
    }

/**
 * @brief TinyUSB Task configuration structure initializer
 *
 * This macro is used to create a custom TinyUSB Task configuration structure.
 *
 * @param s Stack size of the task
 * @param p Task priority
 * @param a Task affinity (CPU core)
 */
#define TINYUSB_TASK_CUSTOM(s, p, a) \
    (tinyusb_task_config_t) {                    \
        .size = (s),                             \
        .priority = (p),                         \
        .xCoreID = (a),                         \
    }

#ifdef __cplusplus
}
#endif
