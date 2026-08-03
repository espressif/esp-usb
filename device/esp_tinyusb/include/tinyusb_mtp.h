/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque handle for a TinyUSB MTP storage instance.
 */
typedef struct tinyusb_mtp_storage_s *tinyusb_mtp_storage_handle_t;

/**
 * @brief TinyUSB MTP driver configuration.
 *
 * All string pointers are copied by tinyusb_mtp_install_driver(), so callers may
 * pass stack literals or temporary configuration objects safely.
 */
typedef struct {
    const char *manufacturer;        /*!< DeviceInfo manufacturer string. NULL uses the default. */
    const char *model;               /*!< DeviceInfo model string. NULL uses the default. */
    const char *version;             /*!< DeviceInfo version string. NULL uses the default. */
    const char *serial;              /*!< DeviceInfo serial string. NULL uses the default descriptor serial. */
    const char *friendly_name;       /*!< Device friendly name property. NULL uses the default. */
} tinyusb_mtp_driver_config_t;

/**
 * @brief TinyUSB MTP storage configuration.
 *
 * The storage must already be mounted in ESP-IDF VFS. MTP accesses files via
 * POSIX APIs and does not mount, unmount, or take block-device ownership.
 */
typedef struct {
    const char *base_path;           /*!< Mounted filesystem base path, for example "/fatfs", "/sdcard", or "/nand". */
    const char *display_name;        /*!< Host-visible storage name. NULL uses base_path. */
    bool removable;                  /*!< Whether the storage should be reported as removable. */
} tinyusb_mtp_storage_config_t;

/**
 * @brief Install the TinyUSB MTP class driver glue.
 *
 * This does not start the USB device stack. Call tinyusb_driver_install() after
 * installing this driver and registering the desired storage paths.
 *
 * @param[in] config Optional string configuration. NULL uses defaults.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if already installed
 *      - ESP_ERR_NO_MEM if memory allocation fails
 */
esp_err_t tinyusb_mtp_install_driver(const tinyusb_mtp_driver_config_t *config);

/**
 * @brief Uninstall the TinyUSB MTP class driver glue.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the driver is not installed
 */
esp_err_t tinyusb_mtp_uninstall_driver(void);

/**
 * @brief Register an already-mounted VFS path as an MTP storage.
 *
 * @param[in] config Storage configuration. Must not be NULL.
 * @param[out] handle Optional handle for later unregistration.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if config or base_path is invalid
 *      - ESP_ERR_INVALID_STATE if the MTP driver is not installed
 *      - ESP_ERR_NOT_FOUND if base_path is not a mounted FATFS path
 *      - ESP_ERR_NO_MEM if memory allocation fails
 *      - ESP_FAIL if the maximum storage count is reached
 */
esp_err_t tinyusb_mtp_register_storage(const tinyusb_mtp_storage_config_t *config, tinyusb_mtp_storage_handle_t *handle);

/**
 * @brief Unregister an MTP storage.
 *
 * @param[in] handle Storage handle returned by tinyusb_mtp_register_storage().
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if handle is NULL or invalid
 *      - ESP_ERR_INVALID_STATE if the MTP driver is not installed
 */
esp_err_t tinyusb_mtp_unregister_storage(tinyusb_mtp_storage_handle_t handle);

#ifdef __cplusplus
}
#endif
