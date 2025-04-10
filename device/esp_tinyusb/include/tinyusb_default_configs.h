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

// Default size for task stack used in TinyUSB task creation
#define TINYUSB_DEFAULT_TASK_SIZE      4096
// Default priority for task used in TinyUSB task creation
#define TINYUSB_DEFAULT_TASK_PRIO      5

/**
 * @brief Default TinyUSB Driver configuration structure initializer
 *
 * Default port:
 * - ESP32P4:       USB OTG 2.0 (High-speed)
 * - ESP32S2/S3:    USB OTG 1.1 (Full-speed)
 *
 * Default task affinity:
 * - ESP32P4:       CPU1
 * - ESP32S2/S3:    CPU0
 */
#if CONFIG_IDF_TARGET_ESP32P4
#define TINYUSB_DEFAULT_CONFIG()       TINYUSB_CONFIG_HIGH_SPEED()
#else
#define TINYUSB_DEFAULT_CONFIG()       TINYUSB_CONFIG_FULL_SPEED()
#endif

#define TINYUSB_CONFIG_FULL_SPEED() \
    (tinyusb_config_t) {                              \
        .skip_phy_setup = false,                      \
        .port = TINYUSB_PORT_0,                       \
        .task = TINYUSB_TASK_CPU0(),                  \
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
        .task = TINYUSB_TASK_CPU1(),                  \
        .self_powered = false,                        \
        .vbus_monitor_io = -1,                        \
        .device_descriptor = NULL,                    \
        .string_descriptor = NULL,                    \
        .string_descriptor_count = 0,                 \
        .fs_configuration_descriptor = NULL,          \
        .hs_configuration_descriptor = NULL,          \
        .qualifier_descriptor = NULL,                 \
    }

#define TINYUSB_TASK_CPU0()  \
    (tinyusb_task_config_t) {                               \
        .size = TINYUSB_DEFAULT_TASK_SIZE,                  \
        .priority = TINYUSB_DEFAULT_TASK_PRIO,              \
        .affinity = TINYUSB_TASK_AFFINITY_CPU0,             \
    }

#define TINYUSB_TASK_CPU1()  \
    (tinyusb_task_config_t) {                               \
        .size = TINYUSB_DEFAULT_TASK_SIZE,                  \
        .priority = TINYUSB_DEFAULT_TASK_PRIO,              \
        .affinity = TINYUSB_TASK_AFFINITY_CPU1,             \
    }


#ifdef __cplusplus
}
#endif
