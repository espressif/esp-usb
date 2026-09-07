/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <wchar.h>
#include <stdint.h>
#include "esp_err.h"
#include "usb/usb_host.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_ERR_MSC_HOST_BASE        0x1700                      /*!< MSC host error code base */
#define ESP_ERR_MSC_MOUNT_FAILED    (ESP_ERR_MSC_HOST_BASE + 1)  /*!< Failed to mount storage */
#define ESP_ERR_MSC_FORMAT_FAILED   (ESP_ERR_MSC_HOST_BASE + 2)  /*!< Failed to format storage */
#define ESP_ERR_MSC_INTERNAL        (ESP_ERR_MSC_HOST_BASE + 3)  /*!< MSC host internal error */
#define ESP_ERR_MSC_STALL           (ESP_ERR_MSC_HOST_BASE + 4)  /*!< USB transfer stalled */

/** @brief Maximum string descriptor length returned by the MSC host driver. */
#define MSC_STR_DESC_SIZE 32

// For backward compatibility with IDF versions which do not have suspend/resume api
#ifdef USB_HOST_LIB_EVENT_FLAGS_AUTO_SUSPEND
/** @brief Indicates that suspend and resume events are available in this build. */
#define MSC_HOST_SUSPEND_RESUME_API_SUPPORTED
#endif

typedef struct msc_host_device *msc_host_device_handle_t;     /*!< Handle to a Mass Storage Device */

/**
 * @brief USB Mass Storage event containing event type and associated device handle.
 */
typedef struct {
    enum {
        MSC_DEVICE_CONNECTED,       /*!< MSC device has been connected to the system. */
        MSC_DEVICE_DISCONNECTED,    /*!< MSC device has been disconnected from the system. */
#ifdef MSC_HOST_SUSPEND_RESUME_API_SUPPORTED
        MSC_DEVICE_SUSPENDED,       /*!< MSC device has been suspended. */
        MSC_DEVICE_RESUMED,         /*!< MSC device has been resumed. */
#endif // MSC_HOST_SUSPEND_RESUME_API_SUPPORTED
    } event;                     /*!< Event type. */
    union {
        uint8_t address;                 /*!< Address of the connected MSC device. */
        msc_host_device_handle_t handle; /*!< Handle of the disconnected, suspended, or resumed device. */
    } device;                    /*!< Event-specific device information. */
} msc_host_event_t;

/**
 * @brief USB Mass Storage event callback.
 *
 * @param[in] event Mass storage event.
 * @param[in] arg User argument from the MSC driver configuration structure.
 */
typedef void (*msc_host_event_cb_t)(const msc_host_event_t *event, void *arg);

/**
 * @brief MSC driver configuration structure.
 */
typedef struct {
    bool create_backround_task;     /*!< When set to true, a background task handles USB events.
                                         Otherwise the application must periodically call msc_host_handle_events(). */
    size_t task_priority;           /*!< Task priority of the created background task. */
    size_t stack_size;              /*!< Stack size of the created background task. */
    BaseType_t core_id;             /*!< Core affinity of the created background task, or tskNO_AFFINITY. */
    msc_host_event_cb_t callback;   /*!< Callback invoked when MSC event occurs. Must not be NULL. */
    void *callback_arg;             /*!< User-provided argument passed to callback. */
} msc_host_driver_config_t;

/**
 * @brief MSC device info.
 */
typedef struct {
    uint32_t sector_count;                     /*!< Number of addressable sectors on the device. */
    uint32_t sector_size;                      /*!< Sector size in bytes. */
    uint16_t idProduct;                        /*!< USB product ID. */
    uint16_t idVendor;                         /*!< USB vendor ID. */
    wchar_t iManufacturer[MSC_STR_DESC_SIZE];  /*!< Manufacturer string. */
    wchar_t iProduct[MSC_STR_DESC_SIZE];       /*!< Product string. */
    wchar_t iSerialNumber[MSC_STR_DESC_SIZE];  /*!< Serial number string. */
} msc_host_device_info_t;

/**
 * @brief Result of a completed logical-unit probe.
 *
 * Bit n in either mask describes LUN n. A ready LUN passed TEST UNIT READY
 * and READ CAPACITY with a supported sector size. A failed LUN failed INQUIRY
 * or READ CAPACITY, reported a non-retryable readiness error, or returned an
 * unsupported sector size. LUNs in
 * 0..max_lun with neither bit set remained unready (including empty slots).
 * USB transport errors fail the probe instead of producing a partial result.
 */
typedef struct {
    uint16_t ready_lun_mask;  /*!< Ready block devices; no filesystem or write-access check is performed. */
    uint16_t failed_lun_mask; /*!< LUNs rejected by SCSI initialization or sector-size validation. */
    uint8_t max_lun;         /*!< Highest LUN reported by GET_MAX_LUN, not a count of inserted media. */
} msc_host_lun_info_t;

/**
 * @brief Install the USB Host Mass Storage Class driver.
 *
 * @param[in] config MSC driver configuration.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if config is NULL or invalid
 *      - ESP_ERR_INVALID_STATE if the driver is already installed
 *      - ESP_ERR_NO_MEM if memory allocation fails
 *      - Other error codes from the USB Host library
 */
esp_err_t msc_host_install(const msc_host_driver_config_t *config);

/**
 * @brief Uninstall the Mass Storage Class driver.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the driver is not installed or a device is still open
 */
esp_err_t msc_host_uninstall(void);

/**
 * @brief Initialize an MSC device after connection.
 *
 * @note Installs LUN 0, preserving the original single-LUN behavior.
 *       Use msc_host_install_device_lun() to select a different LUN.
 *
 * @param[in] device_address Device address obtained from the MSC connection callback.
 * @param[out] device Mass storage device handle to use for subsequent API calls. Must not be NULL.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the driver is not installed
 *      - ESP_ERR_NO_MEM if memory allocation fails
 *      - Other error codes from the USB Host library or MSC transport layer
 */
esp_err_t msc_host_install_device(uint8_t device_address, msc_host_device_handle_t *device);

/**
 * @brief Initialize exactly one logical unit of an MSC device.
 *
 * Starts a new BOT session with a Mass Storage Reset and clears both bulk
 * endpoint halts before initializing the selected LUN.
 *
 * @note No other LUN is probed or used as a fallback. The binding remains
 *       fixed for I/O and reset recovery until uninstall. LUN 0 does not
 *       require GET_MAX_LUN; nonzero LUNs are checked against its response.
 * @note Only one LUN of the selected MSC interface can be installed at once.
 *       Unmount and uninstall the current device before selecting another.
 *       Insert the medium before installation; changing media while installed
 *       is not supported.
 * @note This is a blocking operation. USB event processing must continue in
 *       another task; do not call it from the MSC event callback. Serialize
 *       installation, probing and uninstallation in the application.
 *
 * @param[in] device_address Address obtained from the MSC connection callback.
 * @param[in] lun Logical unit number in 0..15.
 * @param[out] device Installed device handle. Set to NULL on failure.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if device is NULL or lun exceeds 15
 *      - ESP_ERR_NOT_FOUND if lun exceeds the reported maximum
 *      - ESP_ERR_INVALID_STATE if the driver is unavailable or the device is already open
 *      - Other errors from device initialization or the USB Host library
 */
esp_err_t msc_host_install_device_lun(uint8_t device_address, uint8_t lun, msc_host_device_handle_t *device);

/**
 * @brief Discover logical units before installing an MSC device.
 *
 * Opens a temporary session on the same interface used by device installation,
 * resets the BOT transport, queries GET_MAX_LUN, and probes all advertised
 * LUNs. Releases the session before returning. No LUN is selected or mounted;
 * the application chooses a candidate and calls msc_host_install_device_lun(),
 * which revalidates it. GET_MAX_LUN STALL is treated as a single-LUN device.
 *
 * @note LUNs reporting NOT READY / MEDIUM NOT PRESENT (02/3A/xx) are skipped
 *       without retrying and have neither result bit set. Other retryable
 *       readiness failures share one retry window. The initial pass covers
 *       every LUN, even with timeout_ms == 0; finding one ready LUN does not
 *       end the scan. Individual USB transfers keep their own timeouts, so
 *       this is not a strict total execution deadline.
 * @note Results describe observations during this call, not persistent media
 *       identity. The reader must remain connected through selection and
 *       installation. Do not change cards during these operations.
 * @note Call only before installation. This function is blocking and must not
 *       run in the MSC event callback. USB events must be processed in another
 *       task. Serialize probing, installation and uninstallation in the
 *       application. Temporary handles are not delivered in MSC events.
 *
 * @param[in] device_address Address obtained from the MSC connection callback.
 * @param[in] timeout_ms Readiness retry window in milliseconds; 0 scans once.
 * @param[out] info Complete result on success; cleared on failure. Must not be NULL.
 *
 * @return
 *      - ESP_OK on a complete probe, including when no LUN is ready
 *      - ESP_ERR_INVALID_ARG if info is NULL
 *      - ESP_ERR_INVALID_STATE if the driver is unavailable or the device is already open
 *      - ESP_ERR_INVALID_SIZE or ESP_ERR_INVALID_RESPONSE for malformed GET_MAX_LUN
 *      - Other errors from resource management or the USB transport
 */
esp_err_t msc_host_probe_luns(uint8_t device_address, uint32_t timeout_ms, msc_host_lun_info_t *info);

/**
 * @brief Deinitialize an MSC device.
 *
 * @note Stop I/O before calling. USB event processing must continue until
 *       pending transfer callbacks have finished and this call returns.
 *       No commands are sent to the device, which may already be disconnected.
 *
 * @param[in] device Device handle obtained from either installation API.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if device is NULL
 *      - Other error codes from the USB Host library or MSC transport layer
 */
esp_err_t msc_host_uninstall_device(msc_host_device_handle_t device);

/**
 * @brief Helper function for reading sector from mass storage device.
 *
 * @warning This call is not thread safe and should not be combined
 *          with accesses to storage through the file system.
 *
 * @note Provided sector and size cannot exceed the limits reported by
 *       msc_host_device_info_t.
 *
 * @param[in] device Device handle.
 * @param[in] sector Sector number to read.
 * @param[out] data Buffer that receives the sector data.
 * @param[in] size Number of bytes to read.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if device is NULL
 *      - Other error codes from the MSC transport layer
 */
esp_err_t msc_host_read_sector(msc_host_device_handle_t device, size_t sector, void *data, size_t size)
__attribute__((deprecated("use API from esp_private/msc_scsi_bot.h")));

/**
 * @brief Helper function for writing sector to mass storage device.
 *
 * @warning This call is not thread safe and should not be combined
 *          with accesses to storage through the file system.
 *
 * @note Provided sector and size cannot exceed the limits reported by
 *       msc_host_device_info_t.
 *
 * @param[in] device Device handle.
 * @param[in] sector Sector number to write.
 * @param[in] data Data to write to the sector.
 * @param[in] size Number of bytes to write.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if device is NULL
 *      - Other error codes from the MSC transport layer
 */
esp_err_t msc_host_write_sector(msc_host_device_handle_t device, size_t sector, const void *data, size_t size)
__attribute__((deprecated("use API from esp_private/msc_scsi_bot.h")));

/**
 * @brief Handle USB Host events for the MSC driver.
 *
 * If msc_host_install() is called with `create_backround_task = false`, the
 * application must call this function periodically to dispatch USB Host events.
 * Do not call this function when `create_backround_task = true`.
 *
 * @param[in] timeout Timeout in FreeRTOS ticks.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the driver is not installed
 *      - ESP_FAIL if event handling finished because the driver is being uninstalled
 *      - Other error codes returned by usb_host_client_handle_events()
 */
esp_err_t msc_host_handle_events(TickType_t timeout);

/**
 * @brief Get MSC device information.
 *
 * @param[in] device Device handle.
 * @param[out] info Structure to populate with device information.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if device or info is NULL
 *      - Other error codes from the USB Host library
 */
esp_err_t msc_host_get_device_info(msc_host_device_handle_t device, msc_host_device_info_t *info);

/**
 * @brief Print USB descriptors for an MSC device.
 *
 * @param[in] device Handle of the MSC device.
 *
 * @return
 *      - ESP_OK on success
 *      - Other error codes from the USB Host library
 */
esp_err_t msc_host_print_descriptors(msc_host_device_handle_t device);

/**
 * @brief Perform MSC Bulk-Only Transport reset recovery.
 *
 * @see USB Mass Storage Class – Bulk Only Transport, Chapter 5.3.4
 *
 * @param[in] device Handle of the MSC device.
 *
 * @return
 *      - ESP_OK on success
 *      - Other error codes from the MSC transport layer
 */
esp_err_t msc_host_reset_recovery(msc_host_device_handle_t device);

#ifdef __cplusplus
}
#endif //__cplusplus
