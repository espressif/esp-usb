/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "sdkconfig.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define MTP_OBJECT_HANDLE_INVALID       0U
#define MTP_STORAGE_ID_UNSPECIFIED      0U
#define MTP_STORAGE_ID_ALL              UINT32_MAX
#define MTP_OBJECT_HANDLE_ALL           UINT32_MAX
#define MTP_PARENT_ROOT                 UINT32_MAX
#define MTP_DEPTH_ALL                   UINT32_MAX
#define MTP_ROOT_PARENT                 MTP_PARENT_ROOT
#define MTP_MAX_NAME_CHARS              255U
#define MTP_MAX_NAME_BYTES              (MTP_MAX_NAME_CHARS * 3U)
#define MTP_TEMP_NAME_PREFIX            ".mtp_tmp_"
#define MTP_BACKUP_NAME_PREFIX          ".mtp_bak_"

typedef struct tinyusb_mtp_storage_s tinyusb_mtp_storage_t;

typedef struct {
    bool used;
    uint32_t scan_generation;
    uint32_t handle;
    uint32_t parent;
    tinyusb_mtp_storage_t *storage;
    char *path;
    bool directory;
    uint64_t size;
    time_t mtime;
} mtp_object_t;

struct tinyusb_mtp_storage_s {
    bool used;
    uint32_t storage_id;
    char *base_path;
    char *display_name;
    bool removable;
};

typedef enum {
    MTP_WRITE_IDLE,
    MTP_WRITE_INFO_PREPARED,
    MTP_WRITE_WAITING_DATA,
    MTP_WRITE_RECEIVING_DATA,
    MTP_WRITE_ZERO_SIZE_COMPLETE,
} mtp_write_state_t;

typedef struct {
    mtp_write_state_t state;
    uint32_t handle;
    uint32_t storage_id;
    uint32_t parent_handle;
    uint64_t expected_size;
    uint64_t written;
    char *path;
    char *write_path;
    char *backup_path;
    FILE *file;
    int32_t error_response;
    bool created_path;
    bool created_object;
    bool replace_existing;
    bool directory;
} mtp_pending_write_t;

typedef struct {
    bool active;
    FILE *file;
    tinyusb_mtp_storage_t *storage;
    uint32_t handle;
    uint64_t expected;
    uint64_t remaining;
    uint64_t sent;
    char *path;
} mtp_active_read_t;

typedef struct {
    bool active;
    uint32_t handle;
} mtp_active_edit_t;

typedef struct {
    bool active;
    FILE *file;
    uint32_t handle;
    uint64_t offset;
    uint32_t expected_size;
    uint32_t written;
} mtp_partial_write_t;

typedef struct {
    bool active;
    uint16_t op_code;
    uint8_t *data;
    uint32_t len;
} mtp_active_buffer_t;

typedef struct {
    bool installed;
    struct {
        SemaphoreHandle_t lock;
        char *manufacturer;
        char *model;
        char *version;
        char *serial;
        char *friendly_name;
    } constant;
    struct {
        bool session_open;
        uint32_t next_object_handle;
        uint32_t next_scan_generation;
        tinyusb_mtp_storage_t *storages;
        mtp_object_t *objects;
        mtp_pending_write_t pending_write;
        mtp_active_read_t active_read;
        mtp_active_edit_t active_edit;
        mtp_partial_write_t partial_write;
        mtp_active_buffer_t active_buffer;
        bool deferred_response_active;
        uint16_t deferred_response_op;
        int32_t deferred_response_code;
        bool pending_prop_set_active;
        int32_t pending_prop_set_response;
        bool cancelled_transaction_pending;
        uint32_t cancelled_transaction_id;
    } mux_protected;
} tinyusb_mtp_ctx_t;
