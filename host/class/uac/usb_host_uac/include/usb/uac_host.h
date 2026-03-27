/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <wchar.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "esp_err.h"
#include "uac.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Maximum string descriptor length returned by the UAC host driver.
 *
 * The maximum possible number of characters in an embedded string is device
 * specific. For USB devices, the maximum string length is 126 wide characters,
 * not including the terminating NUL character.
 *
 * This driver stores at most 32 wide characters per string to reduce memory
 * usage when filling uac_host_dev_info_t via uac_host_get_device_info().
 */
#define UAC_STR_DESC_MAX_LENGTH              (32)

/**
 * @brief Flag that starts the stream in a suspended state.
 *
 * When set in uac_host_stream_config_t::flags, uac_host_device_start() claims
 * the interface and prepares the resources without submitting stream transfers.
 *
 * @note Call uac_host_device_resume() later to start the transfers.
 */
#define FLAG_STREAM_SUSPEND_AFTER_START      (1 << 0)

typedef struct uac_interface *uac_host_device_handle_t;    /*!< Handle to a particular UAC interface. */

// ------------------------ USB UAC Host events --------------------------------
/**
 * @brief USB UAC host driver event identifier.
 */
typedef enum {
    UAC_HOST_DRIVER_EVENT_RX_CONNECTED = 0x00,           /*!< UAC RX Device has been found in connected USB device */
    UAC_HOST_DRIVER_EVENT_TX_CONNECTED,                  /*!< UAC TX Device has been found in connected USB device */
} uac_host_driver_event_t;

/**
 * @brief USB UAC interface event identifier.
 */
typedef enum {
    UAC_HOST_DEVICE_EVENT_RX_DONE = 0x00,                /*!< RX buffer data size exceeded the configured threshold. */
    UAC_HOST_DEVICE_EVENT_TX_DONE,                       /*!< TX buffer data size fell below its threshold. */
    UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR,                /*!< UAC Device transfer error */
    UAC_HOST_DRIVER_EVENT_DISCONNECTED,                  /*!< UAC Device has been disconnected */
} uac_host_device_event_t;

// ------------------------ USB UAC Host events callbacks -----------------------------
/**
 * @brief USB UAC driver event callback.
 *
 * @param[in] addr USB address of the connected UAC device.
 * @param[in] iface_num UAC interface number.
 * @param[in] event UAC driver event.
 * @param[in] arg User argument from the UAC driver configuration structure.
 */
typedef void (*uac_host_driver_event_cb_t)(uint8_t addr, uint8_t iface_num,
                                           const uac_host_driver_event_t event, void *arg);

/**
 * @brief USB UAC logic device/interface event callback.
 *
 * @param[in] uac_device_handle UAC device handle.
 * @param[in] event UAC device event.
 * @param[in] arg User argument.
 */
typedef void (*uac_host_device_event_cb_t)(uac_host_device_handle_t uac_device_handle,
                                           const uac_host_device_event_t event, void *arg);

/**
 * @brief Interface parameters passed to the UAC class descriptor print callback.
 */
typedef struct {
    uint8_t bClass;         /*!< Interface Class of the UAC device */
    uint8_t bSubclass;      /*!< Interface Subclass of the UAC device */
    uint8_t bProtocol;      /*!< Protocol of the UAC device */
} iface_params_t;

/**
 * @brief USB UAC host class descriptor print callback.
 *
 * @param[in] desc Pointer to the USB configuration descriptor.
 * @param[in] iface_params Pointer to the interface parameters structure.
 */
typedef void (*print_class_descriptor_with_context_cb)(const usb_standard_desc_t *desc, iface_params_t *iface_params);

/**
 * @brief UAC stream type.
 */
typedef enum {
    UAC_STREAM_TX = 0,     /*!< usb audio TX (eg. speaker stream) */
    UAC_STREAM_RX,         /*!< usb audio RX (eg. microphone stream) */
    UAC_STREAM_MAX,        /*!< max stream type */
} uac_host_stream_t;

/**
 * @brief USB UAC logic device or interface information.
 */
typedef struct {
    uac_host_stream_t type;                             /*!< Stream type */
    uint8_t iface_num;                                  /*!< UAC Interface Number */
    uint8_t iface_alt_num;                              /*!< UAC Interface Alternate Setting Total Number */
    uint8_t addr;                                       /*!< USB Address of connected UAC device */
    uint16_t VID;                                       /*!< Vendor ID */
    uint16_t PID;                                       /*!< Product ID */
    wchar_t iManufacturer[UAC_STR_DESC_MAX_LENGTH];     /*!< Manufacturer string */
    wchar_t iProduct[UAC_STR_DESC_MAX_LENGTH];          /*!< Product string */
    wchar_t iSerialNumber[UAC_STR_DESC_MAX_LENGTH];     /*!< Serial Number string */
} uac_host_dev_info_t;

/**
 * @brief USB UAC alternate interface parameters.
 */
typedef struct {
    uint8_t format;                                  /*!< Audio stream format. Currently only PCM (1) is supported. */
    uint8_t channels;                                /*!< Number of audio channels. */
    uint8_t bit_resolution;                          /*!< Audio bit resolution. */
    uint8_t sample_freq_type;                        /*!< 0 for continuous range, non-zero for discrete values. */
    union {
        uint32_t sample_freq[UAC_FREQ_NUM_MAX];      /*!< First UAC_FREQ_NUM_MAX discrete sample frequencies. */
        struct {
            uint32_t sample_freq_lower;              /*!< Lower bound of the continuous sample frequency range. */
            uint32_t sample_freq_upper;              /*!< Upper bound of the continuous sample frequency range. */
        };
    };
} uac_host_dev_alt_param_t;

/**
 * @brief UAC driver configuration structure.
 */
typedef struct {
    bool create_background_task;            /*!< When set to true, background task handling USB events is created.
                                             Otherwise user has to periodically call uac_host_handle_events(). */
    size_t task_priority;                   /*!< Task priority of the created background task. */
    size_t stack_size;                      /*!< Stack size of the created background task. */
    BaseType_t core_id;                     /*!< Core affinity of the created background task, or tskNO_AFFINITY. */
    uac_host_driver_event_cb_t callback;    /*!< Callback invoked when UAC driver event occurs. Must not be NULL. */
    void *callback_arg;                     /*!< User-provided argument passed to callback. */
} uac_host_driver_config_t;

/**
 * @brief UAC logic device or interface configuration structure.
 */
typedef struct {
    uint8_t addr;                                       /*!< USB Address of connected physical device */
    uint8_t iface_num;                                  /*!< UAC Interface Number */
    uint32_t buffer_size;                               /*!< Audio buffer size */
    uint32_t buffer_threshold;                          /*!< Audio buffer threshold */
    uac_host_device_event_cb_t callback;                /*!< Callback invoked when UAC device event occurs */
    void *callback_arg;                                 /*!< User provided argument passed to callback */
} uac_host_device_config_t;

/**
 * @brief UAC stream configuration structure.
 */
typedef struct {
    uint8_t channels;                                    /*!< Audio channel number */
    uint8_t bit_resolution;                              /*!< Audio bit resolution */
    uint32_t sample_freq;                                /*!< Audio sample frequency in Hz. */
    uint16_t flags;                                      /*!< Control flags */
} uac_host_stream_config_t;

// ----------------------------- Public ---------------------------------------
/**
 * @brief Install the USB Host UAC class driver.
 *
 * @note Call this function after usb_host_install().
 *
 * @param[in] config UAC driver configuration structure.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the UAC driver is already installed
 *      - ESP_ERR_INVALID_ARG if config is NULL or invalid
 *      - ESP_ERR_NO_MEM if memory allocation fails
 *      - Other error codes from the USB Host library
 */
esp_err_t uac_host_install(const uac_host_driver_config_t *config);

/**
 * @brief Uninstall the USB Host UAC class driver.
 *
 * @note All opened devices must be closed before calling this function.
 *
 * @return
 *      - ESP_OK on success, or if the driver is already uninstalled
 *      - ESP_ERR_INVALID_STATE if a device or interface is still open
 */
esp_err_t uac_host_uninstall(void);

/**
 * @brief Open a UAC logic device or interface for a connected device.
 *
 * @note Retrieve `config->addr` and `config->iface_num` from the UAC driver
 *       event callback after UAC_HOST_DRIVER_EVENT_RX_CONNECTED or
 *       UAC_HOST_DRIVER_EVENT_TX_CONNECTED.
 *
 * @param[in] config Pointer to the UAC device configuration structure.
 * @param[out] uac_dev_handle Pointer to the UAC device handle.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the UAC driver is not installed
 *      - ESP_ERR_INVALID_ARG if config is NULL, uac_dev_handle is NULL, or the configuration is invalid
 *      - ESP_ERR_NO_MEM if memory allocation fails
 *      - ESP_ERR_NOT_SUPPORTED if the UAC version is not supported
 *      - ESP_ERR_INVALID_SIZE if the calculated packet size exceeds the endpoint maximum packet size
 *      - Other error codes from the USB Host library
 */
esp_err_t uac_host_device_open(const uac_host_device_config_t *config, uac_host_device_handle_t *uac_dev_handle);

/**
 * @brief Open a UAC logic device or interface by VID, PID, and interface number.
 *
 * @note `config->addr` is ignored. Provide the VID, PID, and `config->iface_num`
 *       explicitly. If multiple devices with the same VID and PID are connected
 *       through a hub, the first matching device is opened.
 *
 * @param[in] vid Vendor ID.
 * @param[in] pid Product ID.
 * @param[in] config Pointer to the UAC device configuration structure.
 * @param[out] uac_dev_handle Pointer to the UAC device handle.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the UAC driver is not installed
 *      - ESP_ERR_INVALID_ARG if config is NULL, uac_dev_handle is NULL, or the arguments are invalid
 *      - ESP_ERR_NO_MEM if memory allocation fails
 *      - ESP_ERR_NOT_SUPPORTED if the UAC version is not supported
 *      - ESP_ERR_NOT_FOUND if no matching device is found
 *      - ESP_ERR_INVALID_SIZE if the calculated packet size exceeds the endpoint maximum packet size
 *      - Other error codes from the USB Host library
 */
esp_err_t uac_host_device_open_with_vid_pid(uint16_t vid, uint16_t pid, const uac_host_device_config_t *config,
                                            uac_host_device_handle_t *uac_dev_handle);

/**
 * @brief Close a UAC logic device or interface.
 *
 * @param[in] uac_dev_handle UAC device handle.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if uac_dev_handle is invalid
 *      - Other error codes from the USB Host library
 */
esp_err_t uac_host_device_close(uac_host_device_handle_t uac_dev_handle);

/**
 * @brief Get UAC device information.
 *
 * @param[in] uac_dev_handle UAC device handle.
 * @param[out] uac_dev_info Pointer to the UAC device information structure.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the UAC device is not opened
 *      - ESP_ERR_INVALID_ARG if uac_dev_info is NULL
 */
esp_err_t uac_host_get_device_info(uac_host_device_handle_t uac_dev_handle, uac_host_dev_info_t *uac_dev_info);

/**
 * @brief Get UAC alternate setting parameters by alternate interface index.
 *
 * @param[in] uac_dev_handle UAC device handle.
 * @param[in] iface_alt Alternate interface number. The first alternate setting is 1.
 * @param[out] uac_alt_param Pointer to the alternate setting parameters.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the UAC device is not opened
 *      - ESP_ERR_INVALID_ARG if iface_alt or uac_alt_param is invalid
 */
esp_err_t uac_host_get_device_alt_param(uac_host_device_handle_t uac_dev_handle,
                                        uint8_t iface_alt,
                                        uac_host_dev_alt_param_t *uac_alt_param);

/**
 * @brief Print UAC device information and alternate parameters.
 *
 * @param[in] uac_dev_handle UAC device handle.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the UAC device is not opened
 *      - Other error codes from the UAC host driver
 */
esp_err_t uac_host_printf_device_param(uac_host_device_handle_t uac_dev_handle);

/**
 * @brief Handle USB Host events for the UAC driver.
 *
 * If uac_host_install() is called with `create_background_task = false`, the
 * application must call this function periodically to dispatch USB Host events.
 * Do not call this function when `create_background_task = true`.
 *
 * @param[in] timeout Timeout in ticks. Use pdMS_TO_TICKS() to convert from milliseconds.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the UAC driver is not installed
 *      - ESP_FAIL if event handling is finished and the function should no longer be called
 *      - Other error codes returned by usb_host_client_handle_events()
 */
esp_err_t uac_host_handle_events(TickType_t timeout);

// ------------------------ USB UAC Host driver API ----------------------------
/**
 * @brief Start a UAC stream with a specific stream configuration.
 *
 * @note Set FLAG_STREAM_SUSPEND_AFTER_START to claim the interface and prepare
 *       the resources without starting the transfers immediately.
 *
 * @param[in] uac_dev_handle UAC device handle.
 * @param[in] stream_config Pointer to the UAC stream configuration structure.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if the device handle or stream configuration is invalid
 *      - ESP_ERR_NOT_FOUND if the stream configuration is not supported
 *      - ESP_ERR_INVALID_STATE if the device is not in the correct state
 *      - ESP_ERR_NO_MEM if memory allocation fails
 *      - ESP_ERR_TIMEOUT if a control transfer times out
 *      - ESP_ERR_INVALID_SIZE if the calculated packet size exceeds the endpoint maximum packet size
 *      - Other error codes from lower layers
 */
esp_err_t uac_host_device_start(uac_host_device_handle_t uac_dev_handle, const uac_host_stream_config_t *stream_config);

/**
 * @brief Suspend a UAC stream.
 *
 * @param[in] uac_dev_handle UAC device handle.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if the device handle is invalid
 *      - ESP_ERR_INVALID_STATE if the device is not in the correct state
 */
esp_err_t uac_host_device_suspend(uac_host_device_handle_t uac_dev_handle);

/**
 * @brief Resume a UAC stream with the existing stream configuration.
 *
 * @param[in] uac_dev_handle UAC device handle.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if the device handle is invalid
 *      - ESP_ERR_INVALID_STATE if the device is not in the correct state
 */
esp_err_t uac_host_device_resume(uac_host_device_handle_t uac_dev_handle);

/**
 * @brief Stop a UAC stream and release its streaming resources.
 *
 * @param[in] uac_dev_handle UAC device handle.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if the device handle is invalid
 *      - ESP_ERR_INVALID_STATE if the device is not in the correct state
 */
esp_err_t uac_host_device_stop(uac_host_device_handle_t uac_dev_handle);

/**
 * @brief Read data from the UAC stream buffer.
 *
 * This function is available only after the stream has started.
 *
 * @param[in] uac_dev_handle UAC device handle.
 * @param[out] data Buffer that receives the audio data.
 * @param[in] size Number of bytes to read.
 * @param[out] bytes_read Pointer to the number of bytes read.
 * @param[in] timeout Timeout in ticks. Use pdMS_TO_TICKS() to convert from milliseconds.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if the device handle, data buffer, or bytes_read pointer is invalid
 *      - ESP_ERR_INVALID_STATE if the device is not in the correct state
 *      - Other error codes returned by the ring buffer read operation
 */
esp_err_t uac_host_device_read(uac_host_device_handle_t uac_dev_handle, uint8_t *data, uint32_t size,
                               uint32_t *bytes_read, uint32_t timeout);

/**
 * @brief Write data to the UAC stream buffer.
 *
 * This function can be called only after the stream has started.
 *
 * @note The data is copied into the internal ring buffer before the function
 *       returns. The actual USB transfer is scheduled asynchronously.
 *
 * @param[in] uac_dev_handle UAC device handle.
 * @param[in] data Pointer to the data buffer.
 * @param[in] size Number of bytes to write.
 * @param[in] timeout Timeout in ticks. Use pdMS_TO_TICKS() to convert from milliseconds.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if the device handle or data buffer is invalid
 *      - ESP_ERR_INVALID_STATE if the device is not in the correct state
 *      - ESP_FAIL if the write cannot be queued
 *      - Other error codes returned by the ring buffer write operation
 */
esp_err_t uac_host_device_write(uac_host_device_handle_t uac_dev_handle, uint8_t *data, uint32_t size,
                                uint32_t timeout);

/**
 * @brief Mute or unmute the UAC device.
 *
 * @param[in] uac_dev_handle UAC device handle.
 * @param[in] mute Set to true to mute, or false to unmute.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the device is not ready or active
 *      - ESP_ERR_INVALID_ARG if the device handle is invalid
 *      - ESP_ERR_NOT_SUPPORTED if the device does not support mute control
 *      - ESP_ERR_TIMEOUT if the control transfer times out
 */
esp_err_t uac_host_device_set_mute(uac_host_device_handle_t uac_dev_handle, bool mute);

/**
 * @brief Get the mute status of the UAC device.
 *
 * @param[in] uac_dev_handle UAC device handle.
 * @param[out] mute Pointer that receives the mute status.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the device is not ready or active
 *      - ESP_ERR_INVALID_ARG if the device handle or mute pointer is invalid
 *      - ESP_ERR_NOT_SUPPORTED if the device does not support mute control
 *      - ESP_ERR_TIMEOUT if the control transfer times out
 */
esp_err_t uac_host_device_get_mute(uac_host_device_handle_t uac_dev_handle, bool *mute);

/**
 * @brief Set the normalized volume of the UAC device.
 *
 * @param[in] uac_dev_handle UAC device handle.
 * @param[in] volume Volume to set in the range 0 to 100.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the device is not ready or active
 *      - ESP_ERR_INVALID_ARG if the device handle is invalid or volume is out of range
 *      - ESP_ERR_NOT_SUPPORTED if the device does not support volume control
 *      - ESP_ERR_TIMEOUT if the control transfer times out
 */
esp_err_t uac_host_device_set_volume(uac_host_device_handle_t uac_dev_handle, uint8_t volume);

/**
 * @brief Get the normalized volume of the UAC device.
 *
 * @param[in] uac_dev_handle UAC device handle.
 * @param[out] volume Pointer that receives the volume in the range 0 to 100.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the device is not ready or active
 *      - ESP_ERR_INVALID_ARG if the device handle or volume pointer is invalid
 *      - ESP_ERR_NOT_SUPPORTED if the device does not support volume control
 *      - ESP_ERR_TIMEOUT if the control transfer times out
 */
esp_err_t uac_host_device_get_volume(uac_host_device_handle_t uac_dev_handle, uint8_t *volume);

/**
 * @brief Set the device volume in dB.
 *
 * @param[in] uac_dev_handle UAC device handle.
 * @param[in] volume_db Volume in units of 1/256 dB.
 *                      For example, 256 (0x0100) is 1 dB, 32767 (0x7FFF) is
 *                      127.996 dB, and -32767 (0x8001) is -127.996 dB.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the device is not ready or active
 *      - ESP_ERR_INVALID_ARG if the device handle is invalid
 *      - ESP_ERR_NOT_SUPPORTED if the device does not support volume control
 *      - ESP_ERR_TIMEOUT if the control transfer times out
 */
esp_err_t uac_host_device_set_volume_db(uac_host_device_handle_t uac_dev_handle, int16_t volume_db);

/**
 * @brief Get the device volume in dB.
 *
 * @param[in] uac_dev_handle UAC device handle.
 * @param[out] volume_db Pointer that receives the volume in units of 1/256 dB.
 *                       For example, 256 (0x0100) is 1 dB, 32767 (0x7FFF) is
 *                       127.996 dB, and -32767 (0x8001) is -127.996 dB.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the device is not ready or active
 *      - ESP_ERR_INVALID_ARG if the device handle or volume_db pointer is invalid
 *      - ESP_ERR_NOT_SUPPORTED if the device does not support volume control
 *      - ESP_ERR_TIMEOUT if the control transfer times out
 */
esp_err_t uac_host_device_get_volume_db(uac_host_device_handle_t uac_dev_handle, int16_t *volume_db);

#ifdef __cplusplus
}
#endif //__cplusplus
