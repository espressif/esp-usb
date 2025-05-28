/*
 * SPDX-FileCopyrightText: 2023-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "tinyusb_msc.h"

#warning "This header file is deprecated since v2.0.0. Callbacks are not working. Use tinyusb_msc.h for the new development and refer to the migration guide for more details."

/**
 * @brief Configuration structure for spiflash initialization
 *
 * User configurable parameters that are used while
 * initializing the SPI Flash media.
 */
typedef struct {
    wl_handle_t wl_handle;                                  /*!< Pointer to spiflash wear-levelling handle */
    tusb_msc_callback_t callback_mount_changed;             /*!< Pointer to the function callback that will be delivered AFTER storage mount/unmount operation is successfully finished */
    tusb_msc_callback_t callback_premount_changed;          /*!< Pointer to the function callback that will be delivered BEFORE storage mount/unmount operation is started */
    tusb_msc_callback_t callback_device_mount_changed;      /*!< Pointer to the function callback that will be delivered when a device is unmounted, from tud_umount_cb()  */
    const esp_vfs_fat_mount_config_t mount_config;          /*!< FATFS mount config */
} tinyusb_msc_spiflash_config_t;

/**
 * @brief Register storage type SPI Flash with tinyusb driver
 *
 * @param config pointer to the SPI Flash configuration
 * @return
 *    - ESP_OK: SPI Flash storage initialized successfully
 *    - ESP_ERR_NO_MEM: There was no memory to allocate storage components;
 */
esp_err_t tinyusb_msc_storage_init_spiflash(const tinyusb_msc_spiflash_config_t *config)
{
    return tinyusb_msc_storage_new_spiflash(&(tinyusb_msc_config_t) {
        .context = (void *)config->wl_handle,
        .fat_fs = {
            .base_path = NULL,
            .do_not_format = false,
            .config = config->mount_config,
        },
        .callback = NULL,
        .callback_arg = NULL,
    });
}

#if SOC_SDMMC_HOST_SUPPORTED
/**
 * @brief Configuration structure for sdmmc initialization
 *
 * User configurable parameters that are used while
 * initializing the sdmmc media.
 */
typedef struct {
    sdmmc_card_t *card;                                     /*!< Pointer to sdmmc card configuration structure */
    tusb_msc_callback_t callback_mount_changed;             /*!< Pointer to the function callback that will be delivered AFTER storage mount/unmount operation is successfully finished */
    tusb_msc_callback_t callback_premount_changed;          /*!< Pointer to the function callback that will be delivered BEFORE storage mount/unmount operation is started */
    tusb_msc_callback_t callback_device_mount_changed;      /*!< Pointer to the function callback that will be delivered when a device mounted/unmounted */
    const esp_vfs_fat_mount_config_t mount_config;          /*!< FATFS mount config */
} tinyusb_msc_sdmmc_config_t;

/**
 * @brief Register storage type sd-card with tinyusb driver
 *
 * @param config pointer to the sd card configuration
 * @return
 *    - ESP_OK: SDMMC storage initialized successfully
 *    - ESP_ERR_NO_MEM: There was no memory to allocate storage components;
 */
esp_err_t tinyusb_msc_storage_init_sdmmc(const tinyusb_msc_sdmmc_config_t *config)
{
    return tinyusb_msc_storage_new_mmc(&(tinyusb_msc_config_t) {
        .context = (void *)config->card,
        .fat_fs = {
            .base_path = NULL,
            .do_not_format = false,
            .config = config->mount_config,
        },
        .callback = NULL,
        .callback_arg = NULL,
    });
}
#endif

/**
 * @brief Deinitialize TinyUSB MSC storage
 *
 * This function deinitializes the TinyUSB MSC storage interface.
 * It releases any resources allocated during initialization.
 */
void tinyusb_msc_storage_deinit(void)
{
    return tinyusb_msc_storage_delete();
}
