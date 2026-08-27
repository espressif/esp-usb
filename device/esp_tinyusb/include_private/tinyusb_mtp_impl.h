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
#include <sys/stat.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "tusb.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MTP_ROOT_PARENT                 0xFFFFFFFFU
#define MTP_OBJECT_HANDLE_INVALID       0U

/**
 * @brief Cached MTP object metadata.
 */
typedef struct {
    bool used;
    uint32_t handle;
    uint32_t parent;
    struct tinyusb_mtp_storage_s *storage;
    char *path;
    bool directory;
    uint64_t size;
    time_t mtime;
} mtp_object_t;

/**
 * @brief Registered MTP storage backed by a mounted VFS path.
 */
struct tinyusb_mtp_storage_s {
    bool used;
    uint32_t storage_id;
    char *base_path;
    char *display_name;
    bool removable;
};

/**
 * @brief State for SendObjectInfo/SendObject.
 */
typedef struct {
    bool active;
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

/**
 * @brief State for GetObject and partial object reads.
 */
typedef struct {
    bool active;
    FILE *file;
    struct tinyusb_mtp_storage_s *storage;
    uint32_t handle;
    uint64_t expected;
    uint64_t remaining;
    uint64_t sent;
    char *path;
} mtp_active_read_t;

/**
 * @brief Active Android direct-edit object.
 */
typedef struct {
    bool active;
    uint32_t handle;
} mtp_active_edit_t;

/**
 * @brief State for Android direct-edit partial writes.
 */
typedef struct {
    bool active;
    uint32_t handle;
    uint64_t offset;
    uint32_t expected_size;
    uint32_t written;
} mtp_partial_write_t;

/**
 * @brief Buffered response payload that spans multiple data packets.
 */
typedef struct {
    bool active;
    uint16_t op_code;
    uint8_t *data;
    uint32_t len;
} mtp_active_buffer_t;

/**
 * @brief TinyUSB MTP driver context.
 */
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
        struct tinyusb_mtp_storage_s *storages;
        mtp_object_t *objects;
        mtp_pending_write_t pending_write;
        mtp_active_read_t active_read;
        mtp_active_edit_t active_edit;
        mtp_partial_write_t partial_write;
        mtp_active_buffer_t active_buffer;
        bool zero_size_send_object_pending;
        bool deferred_response_active;
        uint16_t deferred_response_op;
        int32_t deferred_response_code;
        bool pending_prop_set_active;
        int32_t pending_prop_set_response;
    } mux_protected;
} tinyusb_mtp_ctx_t;

extern tinyusb_mtp_ctx_t s_mtp;

/**
 * @brief Lock the MTP context mutex.
 */
void mtp_lock(void);

/**
 * @brief Unlock the MTP context mutex.
 */
void mtp_unlock(void);

/**
 * @brief Check whether a storage handle belongs to the installed storage table.
 *
 * @note Must be called with the MTP context lock held.
 */
bool mtp_storage_handle_is_valid_locked(const struct tinyusb_mtp_storage_s *storage);

/**
 * @brief Clear active edit state.
 *
 * @note Must be called with the MTP context lock held.
 */
void mtp_clear_active_edit(void);

/**
 * @brief Clear active partial-write state.
 *
 * @note Must be called with the MTP context lock held.
 */
void mtp_clear_partial_write(void);

/**
 * @brief Check if a host-provided object name is safe as one path segment.
 */
bool mtp_name_is_safe(const char *name);

/**
 * @brief Encode UTF-8 text as an MTP string payload.
 */
bool mtp_utf8_to_mtp_string_payload(const char *value, uint8_t *payload, size_t payload_size, uint32_t *payload_len);

/**
 * @brief Refresh cached object stat fields.
 *
 * @note Must be called with the MTP context lock held.
 */
int32_t mtp_update_object_stat_locked(mtp_object_t *object);

/**
 * @brief Resolve the currently edited object.
 *
 * @note Must be called with the MTP context lock held.
 */
int32_t mtp_get_active_edit_object_locked(uint32_t handle, mtp_object_t **object);

/**
 * @brief Start Android direct-edit for an object.
 *
 * @note Must be called with the MTP context lock held.
 */
int32_t mtp_begin_edit_object_locked(uint32_t handle);

/**
 * @brief Write a byte range in an edited object.
 *
 * @note Must be called with the MTP context lock held.
 */
int32_t mtp_write_object_range_locked(mtp_object_t *object, uint64_t offset, const uint8_t *data, size_t len);

/**
 * @brief Truncate an edited object.
 *
 * @note Must be called with the MTP context lock held.
 */
int32_t mtp_truncate_object_locked(mtp_object_t *object, uint64_t length);

/**
 * @brief Complete MTP data-transfer state processing without sending a USB response.
 *
 * @note Must be called with the MTP context lock held.
 */
int32_t mtp_complete_data_locked(const tud_mtp_cb_data_t *cb_data, mtp_container_info_t *response);

/**
 * @brief Resolve or create a cached object for an existing path.
 *
 * @note Must be called with the MTP context lock held.
 */
mtp_object_t *mtp_get_or_create_object(struct tinyusb_mtp_storage_s *storage, uint32_t parent, const char *path, const struct stat *st);

/**
 * @brief Scan one MTP directory, optionally creating object handles.
 *
 * @note Must be called with the MTP context lock held. Passing NULL handles only counts entries.
 */
int32_t mtp_scan_children_locked(struct tinyusb_mtp_storage_s *storage, uint32_t parent_handle, uint32_t object_format, uint32_t *handles,
                                 size_t max_handles, uint32_t *count);

/**
 * @brief Handle MTP DeleteObject.
 */
int32_t mtp_delete_object(tud_mtp_cb_data_t *cb_data);

/**
 * @brief Handle MTP SetObjectPropValue.
 */
int32_t mtp_set_object_prop_value(tud_mtp_cb_data_t *cb_data);

#ifdef __cplusplus
}
#endif
