/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include "mtp/tinyusb_mtp_codec.h"
#include "unity.h"

TEST_CASE("MTP: payload builder supports responses above 64 KiB", "[mtp][codec][ci]")
{
    uint8_t chunk[1024] = { 0 };
    mtp_payload_builder_t measure = { .measure_only = true };
    for (size_t i = 0; i < 65; i++) {
        TEST_ASSERT_TRUE(mtp_builder_append_raw(&measure, chunk, sizeof(chunk)));
    }
    TEST_ASSERT_GREATER_THAN_UINT32(64U * 1024U, measure.len);

    mtp_payload_builder_t builder = {
        .data = malloc(measure.len),
        .cap = measure.len,
    };
    TEST_ASSERT_NOT_NULL(builder.data);
    for (size_t i = 0; i < 65; i++) {
        TEST_ASSERT_TRUE(mtp_builder_append_raw(&builder, chunk, sizeof(chunk)));
    }
    TEST_ASSERT_EQUAL_UINT32(measure.len, builder.len);
    free(builder.data);
}
