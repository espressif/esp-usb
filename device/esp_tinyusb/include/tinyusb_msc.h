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
#if (SOC_SDMMC_HOST_SUPPORTED)
#include "driver/sdmmc_host.h"
#endif // SOC_SDMMC_HOST_SUPPORTED

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
    TINYUSB_MSC_EVENT_MOUNT_FAILED,              /*!< Called if the mount operation failed */
    TINYUSB_MSC_EVENT_FORMAT_REQUIRED,           /*!< Called when the storage needs to be formatted */
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

/**
 * @brief Configuration structure for TinyUSB MSC (Mass Storage Class).
 */
typedef struct {
    void *context;                          /*!< Pointer to the storage context.
                                             *   - For SD/MMC: Pointer to `sdmmc_card_t`
                                             *   - For SPI Flash wear-leveling handle: `wl_handle_t`
                                             *   This value must match the selected `storage_type`.
                                             */

    struct {
        const char *base_path;              /*!< Filesystem mount path.
                                             *   - If NULL, a default mount path from the Kconfig is used.
                                             */
        const esp_vfs_fat_mount_config_t config; /*!< FAT filesystem mount configuration.
                                                  *   Controls auto-formatting, max open files, allocation unit size, etc.
                                                  */
        const bool do_not_format;          /*!< If true, do not format the drive if filesystem is not present.
                                             *   - Default is false, meaning the drive will be formatted if no filesystem is found.
                                             */
        const BYTE format_flags;           /*!< Flags for FAT formatting.
                                             *   - A bitwise combination of:
                                             *       - FM_FAT (FAT12/16)
                                             *       - FM_FAT32
                                             *       - FM_EXFAT (ignored if exFAT not enabled)
                                             *       - FM_ANY (default; auto-select based on volume size)
                                             *       - FM_SFD (Single FAT partition)
                                             *   - Set to 0 to use the default (FM_ANY).
                                             */
    } fat_fs;

    tinyusb_msc_mount_point_t mount_point;  /*!< Specifies who initially owns access to the storage:

                                             *   - TINYUSB_MSC_STORAGE_MOUNT_USB: USB host initially owns the storage (MSC mode).
                                             *   - TINYUSB_MSC_STORAGE_MOUNT_APP: Application code initially owns and accesses the storage.
                                             *
                                             *  This affects whether the filesystem is mounted for local use or exposed over USB on startup.
                                             *  Default value is TINYUSB_MSC_STORAGE_MOUNT_USB.
                                             */

    tusb_msc_callback_t callback;           /*!< Callback function invoked on storage events.
                                             *   - Called before and after switching access to the storage between USB and App.
                                             *   - Called when a mount or unmount operation has not been completed.
                                             */

    void *callback_arg;                     /*!< Argument passed to the user callback.
                                             *   - Can be used to pass user context or additional data.
                                             */
} tinyusb_msc_config_t;

// ----------------------------- Driver API ---------------------------------

/**
 * @brief Initialize TinyUSB MSC storage
 *
 * This function initializes the TinyUSB MSC storage interface with the provided configuration.
 * It sets up the storage media, mounts the filesystem, and registers the necessary callbacks.
 *
 * @param[in] config Pointer to the configuration structure for TinyUSB MSC storage
 *
 * @return
 *    - ESP_OK: Initialization successful
 *    - ESP_ERR_INVALID_ARG: Invalid input argument
 *    - ESP_ERR_NO_MEM: Not enough memory to initialize storage
 */
esp_err_t tinyusb_msc_storage_new_spiflash(const tinyusb_msc_config_t *config);

#if (SOC_SDMMC_HOST_SUPPORTED)
/**
 * @brief Initialize TinyUSB MSC storage with SDMMC or EMMC
 *
 * This function initializes the TinyUSB MSC storage interface with MMC as the storage medium.
 * It sets up the card, mounts the filesystem, and registers the necessary callbacks.
 *
 * @param[in] config Pointer to the configuration structure for TinyUSB MSC storage with SDMMC or EMMC
 *
 * @return
 *    - ESP_OK: Initialization successful
 *    - ESP_ERR_INVALID_ARG: Invalid input argument
 *    - ESP_ERR_NO_MEM: Not enough memory to initialize storage
 */
esp_err_t tinyusb_msc_storage_new_mmc(const tinyusb_msc_config_t *config);
#endif // SOC_SDMMC_HOST_SUPPORTED

/**
 * @brief Delete TinyUSB MSC storage
 *
 * This function deinitializes the TinyUSB MSC storage interface.
 * It releases any resources allocated during initialization.
 */
void tinyusb_msc_storage_delete(void);

/**
 * @brief Set a callback function for MSC storage events
 *
 * This function allows the user to set a callback that will be invoked
 * when a storage event occurs, such as a mount or unmount operation.
 *
 * @param[in] callback Pointer to the callback function to be invoked on storage events
 * @param[in] arg Pointer to an argument that will be passed to the callback function
 *
 * @return
 *   - ESP_OK: Callback set successfully
 */
esp_err_t tinyusb_msc_set_callback(tusb_msc_callback_t callback, void *arg);

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


/**
 * @brief Format the storage media
 *
 * This function formats the storage media with a FAT filesystem.
 *
 * @note This function should be called with caution, as it will erase all data on the storage media.
 * @note Could be called when only when the storage media is mounted to the application.
 *
 * @return
 *   - ESP_ERR_NOT_SUPPORTED: Formatting is not supported
 */
esp_err_t tinyusb_msc_storage_format_drive(void);

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
 *    - Current mount point
 */
tinyusb_msc_mount_point_t tinyusb_msc_storage_get_mount_point(void);

#ifdef __cplusplus
}
#endif
