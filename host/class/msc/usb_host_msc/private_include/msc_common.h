/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <sys/queue.h>
#include "esp_err.h"
#include "esp_check.h"
#include "usb/usb_host.h"
#include "usb/usb_types_stack.h"
#include "usb/msc_host.h"
#include "freertos/semphr.h"
#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief MSC disk geometry, shared by the pre-6.1 diskio driver (diskio_usb.c)
 * and the 6.1+ BDL adapter (msc_bdl.c), which derives esp_blockdev_geometry_t
 * from it.
 */
typedef struct {
    uint32_t block_size;    /**< Block size */
    uint32_t block_count;   /**< Block count */
} usb_disk_t;

#ifndef MSC_HOST_BDL_API_SUPPORTED
#include "diskio_usb.h"
#endif // MSC_HOST_BDL_API_SUPPORTED

typedef enum {
    MSC_EP_OUT,
    MSC_EP_IN
} msc_endpoint_t;

typedef struct {
    uint16_t bulk_in_mps;
    uint8_t bulk_in_ep;
    uint8_t bulk_out_ep;
    uint8_t iface_num;
} msc_config_t;

typedef struct msc_host_device {
    STAILQ_ENTRY(msc_host_device) tailq_entry;
    SemaphoreHandle_t transfer_done;
    usb_device_handle_t handle;
    usb_transfer_t *xfer;
    msc_config_t config;
    usb_disk_t disk;
#ifdef MSC_HOST_BDL_API_SUPPORTED
    esp_blockdev_handle_t bdl;
#endif // MSC_HOST_BDL_API_SUPPORTED
} msc_device_t;

/**
 * @brief Trigger a BULK transfer to device
 *
 * Data buffer ownership is transferred to the MSC driver and the application cannot access it before the transfer finishes.
 *
 * @param[in]    device_handle MSC device handle
 * @param[inout] data          Data buffer. Direction depends on 'ep'.
 * @param[in]    size          Size of buffer in bytes
 * @param[in]    ep            Direction of the transfer
 * @return esp_err_t
 */
esp_err_t msc_bulk_transfer(msc_device_t *device_handle, uint8_t *data, size_t size, msc_endpoint_t ep);

/**
 * @brief Trigger a CTRL transfer to device
 *
 * The request and data must be filled by accessing private device_handle->xfer before calling this function
 *
 * @param[in] device_handle MSC device handle
 * @param[in] len           Length of the transfer
 * @return esp_err_t
 */
esp_err_t msc_control_transfer(msc_device_t *device_handle, size_t len);

/**
 * @brief Reset endpoint and clear feature
 *
 * @param[in] device   MSC device handle
 * @param[in] endpoint Endpoint number
 * @return esp_err_t
 */
esp_err_t clear_feature(msc_device_t *device, uint8_t endpoint);

#define MSC_GOTO_ON_ERROR(exp) ESP_GOTO_ON_ERROR(exp, fail, TAG, "")

#define MSC_GOTO_ON_FALSE(exp, err) ESP_GOTO_ON_FALSE( (exp), err, fail, TAG, "" )

#define MSC_RETURN_ON_ERROR(exp) ESP_RETURN_ON_ERROR((exp), TAG, "")

#define MSC_RETURN_ON_FALSE(exp, err) ESP_RETURN_ON_FALSE( (exp), (err), TAG, "")

#define MSC_RETURN_ON_INVALID_ARG(exp) ESP_RETURN_ON_FALSE((exp) != NULL, ESP_ERR_INVALID_ARG, TAG, "")

#ifdef __cplusplus
}
#endif
