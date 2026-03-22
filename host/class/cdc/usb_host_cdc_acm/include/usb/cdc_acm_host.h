/*
 * SPDX-FileCopyrightText: 2015-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "usb/usb_host.h"
#include "usb/usb_types_cdc.h"
#include "usb/cdc_acm_host_ops.h"
#include "usb/cdc_host_types.h"

/**
 * @brief Match any USB vendor ID when opening a CDC device.
 */
#define CDC_HOST_ANY_VID (0)

/**
 * @brief Match any USB product ID when opening a CDC device.
 */
#define CDC_HOST_ANY_PID (0)

// For backward compatibility with IDF versions which do not have suspend/resume api
#ifdef USB_HOST_LIB_EVENT_FLAGS_AUTO_SUSPEND
#define CDC_HOST_SUSPEND_RESUME_API_SUPPORTED
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief New USB device callback.
 *
 * Provides an already opened `usb_dev`, which is closed again after this callback
 * returns. This is useful for inspecting the device descriptors, such as the VID
 * and PID, before selecting a driver.
 *
 * @param[in] usb_dev Open USB device handle valid for the duration of the callback.
 *
 * @attention This callback is called from USB Host context, so the CDC device
 *            cannot be opened here.
 */
typedef void (*cdc_acm_new_dev_callback_t)(usb_device_handle_t usb_dev);

/**
 * @brief CDC-ACM host driver configuration.
 */
typedef struct {
    size_t driver_task_stack_size;         /**< Stack size of the driver's task */
    unsigned driver_task_priority;         /**< Driver task priority. This should be higher than the priority of the
                                                application task using the driver. */
    int  xCoreID;                          /**< Core affinity of the driver's task */
    cdc_acm_new_dev_callback_t new_dev_cb; /**< New USB device connected callback. Can be NULL. */
} cdc_acm_host_driver_config_t;

/**
 * @brief Install the CDC-ACM host driver.
 *
 * - The USB Host Library must already be installed with `usb_host_install()`.
 * - Call this function before any other CDC-ACM host API.
 *
 * @param[in] driver_config Driver configuration. If set to NULL, the default
 *                          configuration is used.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the driver is already installed or the USB Host
 *        Library is not ready
 *      - ESP_ERR_NO_MEM if there is not enough memory to install the driver
 *      - Other error codes from USB Host client registration
 */
esp_err_t cdc_acm_host_install(const cdc_acm_host_driver_config_t *driver_config);

/**
 * @brief Uninstall the CDC-ACM host driver.
 *
 * All CDC devices must be closed with `cdc_acm_host_close()` before calling
 * this function.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the driver is not installed or if any CDC
 *        device is still open
 *      - ESP_ERR_NOT_FINISHED if the driver teardown does not complete
 */
esp_err_t cdc_acm_host_uninstall(void);

/**
 * @brief Register a callback for new USB devices.
 *
 * The callback is invoked for every new USB device, not only CDC-ACM devices.
 *
 * @note The CDC-ACM host driver must already be installed.
 *
 * @param[in] new_dev_cb Callback function to register. Set to NULL to disable
 *                       the notification.
 *
 * @return
 *      - ESP_OK on success
 */
esp_err_t cdc_acm_host_register_new_dev_callback(cdc_acm_new_dev_callback_t new_dev_cb);

/**
 * @brief Open a CDC-ACM device.
 *
 * The driver first looks for CDC-compliant descriptors. If none are found, it
 * falls back to interfaces that expose two bulk endpoints suitable for data
 * transfers.
 *
 * Use `CDC_HOST_ANY_VID` and `CDC_HOST_ANY_PID` when the vendor or product ID
 * does not matter. In that case, the first matching USB device is opened. This
 * is recommended only when at most one USB device can be present.
 *
 * @param[in] vid Device vendor ID, or `CDC_HOST_ANY_VID` to match any vendor ID.
 * @param[in] pid Device product ID, or `CDC_HOST_ANY_PID` to match any
 *                product ID.
 * @param[in] interface_idx Device interface index used for CDC-ACM
 *                          communication.
 * @param[in] dev_config Device configuration structure.
 * @param[out] cdc_hdl_ret Returned CDC device handle.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the CDC-ACM host driver is not installed
 *      - ESP_ERR_INVALID_ARG if `dev_config` or `cdc_hdl_ret` is NULL
 *      - ESP_ERR_NO_MEM if there is not enough memory to open the device
 *      - ESP_ERR_NOT_FOUND if no matching USB device or interface is found
 *      - Other error codes from interface parsing or device initialization
 */
esp_err_t cdc_acm_host_open(uint16_t vid, uint16_t pid, uint8_t interface_idx,
                            const cdc_acm_host_device_config_t *dev_config,
                            cdc_acm_dev_hdl_t *cdc_hdl_ret);

/**
 * @brief Backward-compatible alias for cdc_acm_host_open().
 *
 * @deprecated Use cdc_acm_host_open() instead.
 *
 * @param[in] vid Device vendor ID.
 * @param[in] pid Device product ID.
 * @param[in] interface_num Device interface index.
 * @param[in] dev_config Device configuration structure.
 * @param[out] cdc_hdl_ret Returned CDC device handle.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the CDC-ACM host driver is not installed
 *      - ESP_ERR_INVALID_ARG if `dev_config` or `cdc_hdl_ret` is NULL
 *      - ESP_ERR_NO_MEM if there is not enough memory to open the device
 *      - ESP_ERR_NOT_FOUND if no matching USB device or interface is found
 *      - Other error codes from interface parsing or device initialization
 */
static inline esp_err_t cdc_acm_host_open_vendor_specific(uint16_t vid, uint16_t pid,
                                                          uint8_t interface_num,
                                                          const cdc_acm_host_device_config_t *dev_config,
                                                          cdc_acm_dev_hdl_t *cdc_hdl_ret)
{
    return cdc_acm_host_open(vid, pid, interface_num, dev_config, cdc_hdl_ret);
}

/**
 * @brief Close a CDC device and release its resources.
 *
 * @note All in-flight transfers are cancelled before the device is closed.
 *
 * @param[in] cdc_hdl CDC handle obtained from cdc_acm_host_open().
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if `cdc_hdl` is NULL
 *      - ESP_ERR_INVALID_STATE if the CDC-ACM host driver is not installed
 */
esp_err_t cdc_acm_host_close(cdc_acm_dev_hdl_t cdc_hdl);

/**
 * @brief Transmit data in blocking mode.
 *
 * @param[in] cdc_hdl CDC handle obtained from cdc_acm_host_open().
 * @param[in] data Data to send.
 * @param[in] data_len Length of `data` in bytes.
 * @param[in] timeout_ms Timeout in milliseconds.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if `cdc_hdl` is NULL, or if `data` is NULL or
 *        `data_len` is zero
 *      - ESP_ERR_NOT_SUPPORTED if the device was opened as read-only
 *      - ESP_ERR_TIMEOUT if the transfer does not complete before the timeout
 *      - ESP_ERR_INVALID_RESPONSE if the USB transfer completes with an invalid
 *        response
 *      - Other error codes from USB transfer submission
 */
esp_err_t cdc_acm_host_data_tx_blocking(cdc_acm_dev_hdl_t cdc_hdl, const uint8_t *data,
                                        size_t data_len, uint32_t timeout_ms);

/**
 * @brief Print the device descriptors.
 *
 * Prints the device descriptor and the full active configuration descriptor in a
 * human-readable format to stdout.
 *
 * @param[in] cdc_hdl CDC handle obtained from cdc_acm_host_open().
 */
void cdc_acm_host_desc_print(cdc_acm_dev_hdl_t cdc_hdl);

/**
 * @brief Get the protocols reported by the CDC interface descriptors.
 *
 * Either output pointer may be NULL if that protocol value is not needed.
 *
 * @param[in] cdc_hdl CDC handle obtained from cdc_acm_host_open().
 * @param[out] comm Returned communication protocol, or NULL to ignore it.
 * @param[out] data Returned data protocol, or NULL to ignore it.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if `cdc_hdl` is NULL
 */
esp_err_t cdc_acm_host_protocols_get(cdc_acm_dev_hdl_t cdc_hdl, cdc_comm_protocol_t *comm, cdc_data_protocol_t *data);

/**
 * @brief Get a CDC functional descriptor.
 *
 * @param[in] cdc_hdl CDC handle obtained from cdc_acm_host_open().
 * @param[in] desc_type Functional descriptor subtype to retrieve.
 * @param[out] desc_out Returned descriptor pointer. This pointer must not be
 *                      NULL.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if `cdc_hdl` is NULL or `desc_type` is invalid
 *      - ESP_ERR_NOT_FOUND if the requested descriptor is not present
 */
esp_err_t cdc_acm_host_cdc_desc_get(cdc_acm_dev_hdl_t cdc_hdl, cdc_desc_subtype_t desc_type,
                                    const usb_standard_desc_t **desc_out);

/**
 * @brief Send a control request to the device.
 *
 * Sends a control transfer as described in chapter 9 of the USB specification.
 * This function is intended for device drivers that use custom or vendor-specific
 * requests in addition to, or instead of, the requests defined by the USB
 * CDC-PSTN specification rev. 1.2.
 *
 * For OUT requests, `data` provides the payload to send. For IN requests, the
 * received payload is copied into `data`.
 *
 * @param[in] cdc_hdl CDC handle obtained from cdc_acm_host_open().
 * @param[in] bmRequestType `bmRequestType` field of the USB setup packet.
 * @param[in] bRequest `bRequest` field of the USB setup packet.
 * @param[in] wValue `wValue` field of the USB setup packet.
 * @param[in] wIndex `wIndex` field of the USB setup packet.
 * @param[in] wLength Payload length in bytes.
 * @param[inout] data Transfer payload buffer. Must not be NULL when `wLength`
 *                    is greater than 0.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if `cdc_hdl` is NULL, or if `data` is NULL while
 *        `wLength` is greater than 0
 *      - ESP_ERR_INVALID_SIZE if the request payload does not fit into the
 *        internal control buffer
 *      - ESP_ERR_TIMEOUT if the control transfer times out
 *      - ESP_ERR_INVALID_RESPONSE if the device returns an invalid response
 *      - Other error codes from USB control transfer submission
 */
esp_err_t cdc_acm_host_send_custom_request(cdc_acm_dev_hdl_t cdc_hdl, uint8_t bmRequestType,
                                           uint8_t bRequest, uint16_t wValue,
                                           uint16_t wIndex, uint16_t wLength, uint8_t *data);

#ifdef CDC_HOST_REMOTE_WAKE_SUPPORTED
/**
 * @brief Enable or disable device remote wakeup.
 *
 * @note This API is available only when the underlying ESP-IDF exposes remote
 *       wakeup support, and is guarded by `CDC_HOST_REMOTE_WAKE_SUPPORTED`.
 *
 * @param[in] cdc_hdl CDC device handle.
 * @param[in] enable Set to true to enable remote wakeup, or false to disable it.
 *
 * @return
 *      - ESP_OK on success, or if the requested state is already configured
 *      - ESP_ERR_INVALID_ARG if `cdc_hdl` is invalid
 *      - ESP_ERR_INVALID_STATE if the CDC-ACM host driver is not installed
 *      - ESP_ERR_NOT_FOUND if the device is no longer tracked by the driver
 *      - ESP_ERR_NOT_SUPPORTED if the device does not support remote wakeup
 *      - ESP_ERR_TIMEOUT if the control transfer times out or the control mutex
 *        cannot be acquired
 *      - ESP_ERR_INVALID_RESPONSE if the control transfer completes with an
 *        invalid response
 *      - Other error codes from USB descriptor queries or control requests
 */
esp_err_t cdc_acm_host_enable_remote_wakeup(cdc_acm_dev_hdl_t cdc_hdl, bool enable);
#endif // CDC_HOST_REMOTE_WAKE_SUPPORTED

#ifdef __cplusplus
}
class CdcAcmDevice {
public:
    // Operators
    CdcAcmDevice() : cdc_hdl(NULL) {};
    virtual ~CdcAcmDevice()
    {
        // Close CDC-ACM device, if it wasn't explicitly closed
        if (this->cdc_hdl != NULL) {
            this->close();
        }
    }

    inline esp_err_t tx_blocking(uint8_t *data, size_t len, uint32_t timeout_ms = 100)
    {
        return cdc_acm_host_data_tx_blocking(this->cdc_hdl, data, len, timeout_ms);
    }

    inline esp_err_t open(uint16_t vid, uint16_t pid, uint8_t interface_idx, const cdc_acm_host_device_config_t *dev_config)
    {
        return cdc_acm_host_open(vid, pid, interface_idx, dev_config, &this->cdc_hdl);
    }

    inline esp_err_t open_vendor_specific(uint16_t vid, uint16_t pid, uint8_t interface_idx, const cdc_acm_host_device_config_t *dev_config)
    {
        return cdc_acm_host_open(vid, pid, interface_idx, dev_config, &this->cdc_hdl);
    }

    inline esp_err_t close()
    {
        const esp_err_t err = cdc_acm_host_close(this->cdc_hdl);
        if (err == ESP_OK) {
            this->cdc_hdl = NULL;
        }
        return err;
    }

    virtual inline esp_err_t line_coding_get(cdc_acm_line_coding_t *line_coding) const
    {
        return cdc_acm_host_line_coding_get(this->cdc_hdl, line_coding);
    }

    virtual inline esp_err_t line_coding_set(cdc_acm_line_coding_t *line_coding)
    {
        return cdc_acm_host_line_coding_set(this->cdc_hdl, line_coding);
    }

    virtual inline esp_err_t set_control_line_state(bool dtr, bool rts)
    {
        return cdc_acm_host_set_control_line_state(this->cdc_hdl, dtr, rts);
    }

    virtual inline esp_err_t send_break(uint16_t duration_ms)
    {
        return cdc_acm_host_send_break(this->cdc_hdl, duration_ms);
    }

    inline esp_err_t send_custom_request(uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex, uint16_t wLength, uint8_t *data)
    {
        return cdc_acm_host_send_custom_request(this->cdc_hdl, bmRequestType, bRequest, wValue, wIndex, wLength, data);
    }

protected:
    cdc_acm_dev_hdl_t cdc_hdl;

private:
    CdcAcmDevice &operator= (const CdcAcmDevice &Copy);
    bool operator== (const CdcAcmDevice &param) const;
    bool operator!= (const CdcAcmDevice &param) const;
};
#endif
