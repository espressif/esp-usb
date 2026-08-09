/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "tinyusb.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    tinyusb_desc_config_t desc_cfg;
    bool owns_desc;
} tinyusb_runtime_desc_t;

esp_err_t tinyusb_runtime_descriptor_build(tinyusb_port_t port,
                                           const tinyusb_runtime_config_t *runtime,
                                           tinyusb_runtime_desc_t *out_desc);

void tinyusb_runtime_descriptor_free(tinyusb_runtime_desc_t *desc);

esp_err_t tinyusb_runtime_activate(tinyusb_port_t port,
                                   const tinyusb_runtime_config_t *runtime);

void tinyusb_runtime_deactivate(void);

bool tinyusb_runtime_is_active(void);

void tinyusb_runtime_set_active(bool active);

void *tinyusb_runtime_get_callback_ctx(tinyusb_runtime_class_t type, uint8_t instance);

bool tinyusb_runtime_has_class(tinyusb_runtime_class_t type);

uint8_t tinyusb_runtime_get_net_mac_string_id(void);

#ifdef __cplusplus
}
#endif
