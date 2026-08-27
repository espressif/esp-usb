/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "tinyusb_mtp.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Test-only helpers used by the in-tree esp_tinyusb MTP test application.
 * These symbols are intentionally not part of the public API and must not be
 * used by production code. They live in include_private/ so that only the
 * component itself and its bundled test app can reach them.
 */

/**
 * @brief Resolve a registered storage path to an object handle.
 *
 * @note Test-only helper.
 *
 * @param[in] storage Storage handle returned by tinyusb_mtp_register_storage().
 * @param[in] path Existing file or directory path inside the registered storage.
 * @param[out] object_handle Resolved MTP object handle.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if storage, path, or object_handle is invalid
 *      - ESP_ERR_NOT_FOUND if path does not exist
 *      - ESP_ERR_NO_MEM if memory allocation fails
 */
esp_err_t tinyusb_mtp_test_find_object(tinyusb_mtp_storage_handle_t storage, const char *path, uint32_t *object_handle);

/**
 * @brief Create a zero-size object through the SendObjectInfo backend path.
 *
 * @note Test-only helper.
 *
 * @param[in] storage Storage handle returned by tinyusb_mtp_register_storage().
 * @param[in] parent_handle Parent object handle, or MTP_ROOT_PARENT for the storage root.
 * @param[in] name Host-provided object file name.
 * @param[out] object_handle Created or replaced object handle.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if arguments are invalid
 *      - ESP_ERR_INVALID_STATE if the MTP driver is not installed
 *      - ESP_FAIL if the backend rejects the object info
 */
esp_err_t tinyusb_mtp_test_send_zero_size_object_info(tinyusb_mtp_storage_handle_t storage, uint32_t parent_handle, const char *name,
                                                      uint32_t *object_handle);

/**
 * @brief Send an empty SendObject after zero-size object info.
 *
 * @note Test-only helper.
 */
esp_err_t tinyusb_mtp_test_send_zero_size_object(void);

/**
 * @brief Read the cached parent object handle.
 *
 * @note Test-only helper.
 *
 * @param[in] object_handle MTP object handle.
 * @param[out] parent_handle Cached parent handle.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if arguments are invalid
 *      - ESP_ERR_NOT_FOUND if the object handle is not cached
 */
esp_err_t tinyusb_mtp_test_get_parent_handle(uint32_t object_handle, uint32_t *parent_handle);

/**
 * @brief Delete an object through the MTP backend.
 *
 * @note Test-only helper.
 *
 * @param[in] object_handle MTP object handle to delete.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_FAIL if the object cannot be deleted
 */
esp_err_t tinyusb_mtp_test_delete_object(uint32_t object_handle);

/**
 * @brief Set the MTP Name property without changing ObjectFileName.
 *
 * @note Test-only helper.
 *
 * @param[in] object_handle MTP object handle to update.
 * @param[in] name New Name property value.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if name is invalid
 *      - ESP_ERR_INVALID_STATE if the MTP driver is not installed
 *      - ESP_FAIL if the property cannot be set
 */
esp_err_t tinyusb_mtp_test_set_object_name(uint32_t object_handle, const char *name);

/**
 * @brief Set ObjectFileName and rename the backing file.
 *
 * @note Test-only helper.
 *
 * @param[in] object_handle MTP object handle to update.
 * @param[in] name New ObjectFileName property value.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if name is invalid
 *      - ESP_ERR_INVALID_STATE if the MTP driver is not installed
 *      - ESP_FAIL if the object cannot be renamed
 */
esp_err_t tinyusb_mtp_test_set_object_file_name(uint32_t object_handle, const char *name);

/**
 * @brief Open an object for Android direct-edit operations.
 *
 * @note Test-only helper.
 *
 * @param[in] object_handle MTP object handle to edit.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the MTP driver is not installed
 *      - ESP_FAIL if the object cannot be opened for editing
 */
esp_err_t tinyusb_mtp_test_begin_edit_object(uint32_t object_handle);

/**
 * @brief Write an object range through the direct-edit backend.
 *
 * @note Test-only helper.
 *
 * The object must have an active edit operation started by tinyusb_mtp_test_begin_edit_object().
 *
 * @param[in] object_handle MTP object handle to write.
 * @param[in] offset Byte offset where writing starts.
 * @param[in] data Data buffer to write. May be NULL only when size is 0.
 * @param[in] size Number of bytes to write.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if data is NULL and size is not 0
 *      - ESP_ERR_INVALID_STATE if the MTP driver is not installed
 *      - ESP_FAIL if the active edit operation cannot be written
 */
esp_err_t tinyusb_mtp_test_write_partial_object(uint32_t object_handle, uint64_t offset, const void *data, size_t size);

/**
 * @brief Truncate an object through the direct-edit backend.
 *
 * @note Test-only helper.
 *
 * The object must have an active edit operation started by tinyusb_mtp_test_begin_edit_object().
 *
 * @param[in] object_handle MTP object handle to truncate.
 * @param[in] size New object size in bytes.
 *
 * @return
 *      - ESP_ERR_INVALID_STATE if the MTP driver is not installed
 *      - ESP_FAIL if the active edit operation cannot be truncated
 */
esp_err_t tinyusb_mtp_test_truncate_object(uint32_t object_handle, uint64_t size);

/**
 * @brief Close an Android direct-edit operation.
 *
 * @note Test-only helper.
 *
 * @param[in] object_handle MTP object handle to close.
 *
 * @return
 *      - ESP_ERR_INVALID_STATE if the MTP driver is not installed
 *      - ESP_FAIL if the active edit operation cannot be closed
 */
esp_err_t tinyusb_mtp_test_end_edit_object(uint32_t object_handle);

#ifdef __cplusplus
}
#endif
