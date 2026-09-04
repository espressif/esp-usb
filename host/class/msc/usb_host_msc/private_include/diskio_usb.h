/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "msc_common.h" // usb_disk_t

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register SCSI-backed FatFS diskio callbacks for an MSC disk
 *
 * Used only on ESP-IDF < 6.1. Obtains pdrv from ff_diskio_get_drive().
 *
 * @param[in] pdrv Number of free drive obtained from ff_diskio_get_drive()
 * @param[in] disk usb_disk_t structure (embedded in msc_device_t)
 */
void ff_diskio_register_msc(uint8_t pdrv, usb_disk_t *disk);

/**
 * @brief Drive number assigned by ff_diskio_register_msc()
 *
 * @param[in] disk usb_disk_t structure
 * @return Drive number, or 0xFF if not registered
 */
uint8_t ff_diskio_get_pdrv_disk(const usb_disk_t *disk);

#ifdef __cplusplus
}
#endif //__cplusplus
