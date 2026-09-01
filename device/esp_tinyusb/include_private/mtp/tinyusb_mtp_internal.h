/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "sdkconfig.h"

#if CONFIG_TINYUSB_MTP_ENABLED

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <time.h>
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "tinyusb_mtp.h"
#include "mtp/tinyusb_mtp_types.h"
#include "tusb.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MTP_DEFAULT_MANUFACTURER        CONFIG_TINYUSB_DESC_MANUFACTURER_STRING
#define MTP_DEFAULT_MODEL               CONFIG_TINYUSB_DESC_PRODUCT_STRING
#define MTP_DEFAULT_VERSION             "1.0"
#define MTP_DEFAULT_SERIAL              CONFIG_TINYUSB_DESC_SERIAL_STRING
#define MTP_DEFAULT_FRIENDLY_NAME       "ESP TinyUSB MTP"
#define MTP_STORAGE_ID(index)           ((((uint32_t)(index) + 1U) << 16) | 1U)
#define MTP_MAX_DATA_BYTES              (UINT32_MAX - sizeof(mtp_container_header_t))

#ifndef MTP_OP_ANDROID_GET_PARTIAL_OBJECT_64
#define MTP_OP_ANDROID_GET_PARTIAL_OBJECT_64   0x95C1U
#endif
#ifndef MTP_OP_ANDROID_SEND_PARTIAL_OBJECT
#define MTP_OP_ANDROID_SEND_PARTIAL_OBJECT     0x95C2U
#endif
#ifndef MTP_OP_ANDROID_TRUNCATE_OBJECT
#define MTP_OP_ANDROID_TRUNCATE_OBJECT         0x95C3U
#endif
#ifndef MTP_OP_ANDROID_BEGIN_EDIT_OBJECT
#define MTP_OP_ANDROID_BEGIN_EDIT_OBJECT       0x95C4U
#endif
#ifndef MTP_OP_ANDROID_END_EDIT_OBJECT
#define MTP_OP_ANDROID_END_EDIT_OBJECT         0x95C5U
#endif
#define MTP_OP_SET_OBJECT_REFERENCES_CODE      0x9811U

#if CONFIG_TINYUSB_MTP_TRACE_WRITES
#define MTP_TRACEI(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define MTP_TRACEI(...) do {} while (0)
#endif

#ifdef __cplusplus
}
#endif

#endif
