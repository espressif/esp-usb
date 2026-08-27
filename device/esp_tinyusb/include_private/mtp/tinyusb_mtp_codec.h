/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "mtp/tinyusb_mtp_types.h"
#include "tusb.h"

typedef struct {
    uint8_t *data;
    uint32_t len;
    uint32_t cap;
    uint32_t count;
    bool measure_only;
} mtp_payload_builder_t;

const char *mtp_basename(const char *path);
void mtp_copy_display_name(const char *filename, bool directory, char *buffer, size_t buffer_size);
void mtp_make_persistent_uid(const mtp_object_t *object, uint8_t uid[16]);
bool mtp_path_is_child_of(const char *path, const char *parent);
char *mtp_join_path(const char *base, const char *name);
bool mtp_name_is_safe(const char *name);
uint16_t mtp_format_from_name(const char *name, bool directory);
uint32_t mtp_container_add_utf8_string(mtp_container_info_t *container, const char *value);
bool mtp_utf8_to_mtp_string_payload(const char *value, uint8_t *payload, size_t payload_size, uint32_t *payload_len);
bool mtp_utf16_to_utf8_name(const uint8_t *src, size_t src_size, char *dst, size_t dst_size);
void mtp_time_to_date_string(time_t value, char *buf, size_t buf_size);
bool mtp_builder_append_raw(mtp_payload_builder_t *builder, const void *data, uint32_t len);
bool mtp_builder_append_uint8(mtp_payload_builder_t *builder, uint8_t value);
bool mtp_builder_append_uint16(mtp_payload_builder_t *builder, uint16_t value);
bool mtp_builder_append_uint32(mtp_payload_builder_t *builder, uint32_t value);
bool mtp_builder_append_uint64(mtp_payload_builder_t *builder, uint64_t value);
bool mtp_builder_append_cstring(mtp_payload_builder_t *builder, const char *value);
bool mtp_builder_append_utf8_string(mtp_payload_builder_t *builder, const char *value);
