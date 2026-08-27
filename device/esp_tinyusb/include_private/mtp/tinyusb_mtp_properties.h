/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "tusb.h"

int32_t mtp_op_get_device_property(tud_mtp_cb_data_t *cb_data);
int32_t mtp_op_get_object_props_supported(tud_mtp_cb_data_t *cb_data);
int32_t mtp_op_get_object_prop_desc(tud_mtp_cb_data_t *cb_data);
int32_t mtp_op_get_object_prop_value(tud_mtp_cb_data_t *cb_data);
int32_t mtp_op_set_object_prop_value(tud_mtp_cb_data_t *cb_data);
int32_t mtp_op_get_object_prop_list(tud_mtp_cb_data_t *cb_data);
int32_t mtp_op_get_object_references(tud_mtp_cb_data_t *cb_data);
