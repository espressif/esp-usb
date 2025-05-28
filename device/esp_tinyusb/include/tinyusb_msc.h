/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include "soc/soc_caps.h"
#include "esp_err.h"
#include "wear_levelling.h"
#include "esp_vfs_fat.h"
#if SOC_SDMMC_HOST_SUPPORTED
#include "driver/sdmmc_host.h"
#endif

/**
 * @brief TinyUSB MSC Storage handle
 */
typedef struct tinyusb_msc_storage_handle_s *tinyusb_msc_storage_handle_t;

/**
 * @brief Types of storage media supported by TinyUSB MSC
 */
typedef enum {
    TINYUSB_MSC_STORAGE_TYPE_SPIFLASH = 0,             /*!< SPI Flash storage type */
#if (SOC_SDMMC_HOST_SUPPORTED)
    TINYUSB_MSC_STORAGE_TYPE_SDMMC,                /*!< SDMMC storage type */
#endif // (SOC_SDMMC_HOST_SUPPORTED)
    TINYUSB_MSC_STORAGE_TYPE_MAX,                  /*!< Maximum value for storage type */
} tinyusb_msc_storage_type_t;

/**
 * @brief Storage mount point types
 */
typedef enum {
    TINYUSB_MSC_STORAGE_MOUNT_USB = 0,             /*!< Storage is used by USB host */
    TINYUSB_MSC_STORAGE_MOUNT_APP,                 /*!< Storage is used by the application */
} tinyusb_msc_mount_point_t;

/**
 * @brief MSC event IDs for mount and unmount operations
 *
 * These events are used to notify the application about the status of the storage mount or unmount operations.
 */
typedef enum {
    TINYUSB_MSC_EVENT_MOUNT_START,               /*!< Called BEFORE mounting or unmounting the filesystem */
    TINYUSB_MSC_EVENT_MOUNT_COMPLETE,            /*!< Called AFTER the mount or unmount operation is complete */
} tinyusb_msc_event_id_t;

/**
 * @brief Describes an event passing to the input of a callbacks
 */
typedef struct {
    tinyusb_msc_event_id_t id;                      /*!< Event id */
    tinyusb_msc_mount_point_t mount_point;          /*!< Mount point type */
} tinyusb_msc_event_t;

/**
 * @brief MSC event callback function type
 *
 * This callback is invoked when a storage mount or unmount operation is initiated or completed.
 */
typedef void(*tusb_msc_callback_t)(tinyusb_msc_event_t *event);

typedef struct {
    tinyusb_msc_storage_type_t storage_type;            /*!< Type of storage media */
    void *context;                                      /*!< Storage context:
                                                            - SDMMC card configuration pointer
                                                            - SPI Flash wear-levelling handle
                                                        */
    const char *base_path;                            /*!< Base path where the filesystem is mounted, can be NULL to use default path */
    const esp_vfs_fat_mount_config_t mount_config;      /*!< FATFS mount config */
    tinyusb_msc_mount_point_t mount_point;              /*!< Initial mount point type, can be either application or USB host */
    tusb_msc_callback_t callback;                       /*!< Pointer to the function callback that will be delivered when a storage mount/unmount operation is started or finished */
    void *callback_arg;                                 /*!< Pointer to the argument passed to the callback */
} tinyusb_msc_config_t;

// ----------------------------- Driver API ---------------------------------

/**
 * @brief Initialize TinyUSB MSC storage
 *
 * This function initializes the TinyUSB MSC storage interface with the provided configuration.
 * It sets up the storage media, mounts the filesystem, and registers the necessary callbacks.
 *
 * @param[in] config Pointer to the configuration structure for TinyUSB MSC storage
//  * @param[out] storage_hdl Pointer to the handle that will be initialized with the storage interface
 *
 * @return
 *    - ESP_OK: Initialization successful
 *    - ESP_ERR_INVALID_ARG: Invalid input argument
 *    - ESP_ERR_NO_MEM: Not enough memory to initialize storage
 */
esp_err_t tinyusb_msc_storage_init(const tinyusb_msc_config_t *config  /*, tinyusb_msc_storage_handle_t *storage_hdl */ );

/**
 * @brief Deinitialize TinyUSB MSC storage
 *
 * This function deinitializes the TinyUSB MSC storage interface.
 * It releases any resources allocated during initialization.
 */
void tinyusb_msc_storage_deinit(void /* tinyusb_msc_storage_handle_t storage_hdl */);

/**
 * @brief Set a callback function for MSC storage events
 *
 * This function allows the user to set a callback that will be invoked
 * when a storage event occurs, such as a mount or unmount operation.
 */
esp_err_t tinyusb_msc_set_callback(tinyusb_msc_storage_handle_t storage_hdl,
                                   tusb_msc_callback_t callback,
                                   void *arg);

/**
 * @brief Mounts the storage media to the application
 *
 * @note This function is called when the USB device is detached from the USB Host.
 */
void tinyusb_msc_storage_to_app(void);

/**
 * @brief Unmounts the storage media from the application and exposes it to the USB Host
 *
 * @note This function is called when the USB device is mounted to the USB Host.
 */
void tinyusb_msc_storage_to_usb(void);

// ------------------------------------ Getters ------------------------------------

/**
 * @brief Get number of sectors in storage media
 *
 * @return
 *    - usable size, in bytes
 */
uint32_t tinyusb_msc_storage_get_sector_count(void);

/**
 * @brief Get sector size of storage media
 *
 * @return
 *    - sector count
 */
uint32_t tinyusb_msc_storage_get_sector_size(void);

/**
 * @brief Get status if storage media is exposed over USB to USB Host
 *
 * @return
 *    - true: The storage media is exposed to USB Host
 *    - false: The storage media is mounted on application (not exposed to USB Host)
 */
tinyusb_msc_mount_point_t tinyusb_msc_storage_get_mount_point(void);

#ifdef __cplusplus
}
#endif
