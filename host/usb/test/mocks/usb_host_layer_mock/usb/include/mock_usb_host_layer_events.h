/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "enum.h"
#include "usbh.h"

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------------------------- USBH events -------------------------------------------------------

/**
 * @brief Propagate control transfer event
 *
 * @param[in] dev_obj Device object passed to the host layer
 * @param[in] urb URB associated with the completed control transfer
 *
 * @return
 *    - ESP_OK: Event propagated
 *    - ESP_ERR_INVALID_STATE: USBH callback not registered, usb_register_mock_callback() must be called first
 */
esp_err_t usbh_mock_event_ctrl_xfer(void *dev_obj, urb_t *urb);

/**
 * @brief Propagate new device event
 *
 * @param[in] dev_addr Address of the new device
 *
 * @note The caller must set hub_dev_new() CMock expectations before calling this helper.
 *
 * @return
 *    - ESP_OK: Event propagated
 *    - ESP_ERR_INVALID_STATE: USBH callback not registered, usb_register_mock_callback() must be called first
 */
esp_err_t usbh_mock_event_new_dev(uint8_t dev_addr);

/**
 * @brief Propagate device gone event
 *
 * @param[in] dev_addr Address of the device that is gone
 * @param[in] dev_obj Device object passed to the host layer
 *
 * @note The caller must set hub_dev_gone() CMock expectations before calling this helper when needed.
 *
 * @return
 *    - ESP_OK: Event propagated
 *    - ESP_ERR_INVALID_STATE: USBH callback not registered, usb_register_mock_callback() must be called first
 */
esp_err_t usbh_mock_event_dev_gone(uint8_t dev_addr, void *dev_obj);

/**
 * @brief Propagate device free event
 *
 * @param[in] dev_uid Unique ID of the freed device
 * @param[in] parent_dev_hdl Handle of the parent device
 * @param[in] parent_port_num Parent port number
 *
 * @return
 *    - ESP_OK: Event propagated
 *    - ESP_ERR_INVALID_STATE: USBH callback not registered, usb_register_mock_callback() must be called first
 */
esp_err_t usbh_mock_event_dev_free(unsigned int dev_uid, void *parent_dev_hdl, uint8_t parent_port_num);

/**
 * @brief Propagate all devices free event
 *
 * @return
 *    - ESP_OK: Event propagated
 *    - ESP_ERR_INVALID_STATE: USBH callback not registered, usb_register_mock_callback() must be called first
 */
esp_err_t usbh_mock_event_all_free(void);

/**
 * @brief Propagate device suspend event
 *
 * @param[in] dev_addr Address of the suspended device
 * @param[in] dev_obj Device object passed to the host layer
 *
 * @return
 *    - ESP_OK: Event propagated
 *    - ESP_ERR_INVALID_STATE: USBH callback not registered, usb_register_mock_callback() must be called first
 */
esp_err_t usbh_mock_event_dev_suspend(uint8_t dev_addr, void *dev_obj);

/**
 * @brief Propagate device resume event
 *
 * @param[in] dev_addr Address of the resumed device
 * @param[in] dev_obj Device object passed to the host layer
 *
 * @return
 *    - ESP_OK: Event propagated
 *    - ESP_ERR_INVALID_STATE: USBH callback not registered, usb_register_mock_callback() must be called first
 */
esp_err_t usbh_mock_event_dev_resume(uint8_t dev_addr, void *dev_obj);

/**
 * @brief Propagate all devices idle event
 *
 * @note The caller must set hub_root_mark_suspend() CMock expectations before calling this helper.
 *
 * @return
 *    - ESP_OK: Event propagated
 *    - ESP_ERR_INVALID_STATE: USBH callback not registered, usb_register_mock_callback() must be called first
 */
esp_err_t usbh_mock_event_all_idle(void);

/**
 * @brief Propagate device removed event
 *
 * @param[in] dev_addr Address of the removed device
 *
 * @return
 *    - ESP_OK: Event propagated
 *    - ESP_ERR_INVALID_STATE: USBH callback not registered, usb_register_mock_callback() must be called first
 */
esp_err_t usbh_mock_event_dev_removed(uint8_t dev_addr);

// ------------------------------------------------- Hub events --------------------------------------------------------

/**
 * @brief Propagate device connected event
 *
 * @param[in] uid Unique device ID of the connected device
 *
 * @note The caller must set enum_start() CMock expectations before calling this helper.
 *
 * @return
 *    - ESP_OK: Event propagated
 *    - ESP_ERR_INVALID_STATE: Hub callback not registered, hub_register_mock_callback() must be called first
 */
esp_err_t hub_mock_event_connected(unsigned int uid);

/**
 * @brief Propagate device reset completed event
 *
 * @param[in] uid Unique device ID of the device
 *
 * @note The caller must set enum_proceed() CMock expectations before calling this helper.
 *
 * @return
 *    - ESP_OK: Event propagated
 *    - ESP_ERR_INVALID_STATE: Hub callback not registered, hub_register_mock_callback() must be called first
 */
esp_err_t hub_mock_event_reset_completed(unsigned int uid);

/**
 * @brief Propagate device disconnected event
 *
 * @param[in] uid Unique device ID of the disconnected device
 *
 * @note The caller must set enum_cancel() and usbh_devs_remove() CMock expectations before calling this helper.
 *
 * @return
 *    - ESP_OK: Event propagated
 *    - ESP_ERR_INVALID_STATE: Hub callback not registered, hub_register_mock_callback() must be called first
 */
esp_err_t hub_mock_event_disconnected(unsigned int uid);

// ------------------------------------------------- Enum events -------------------------------------------------------

/**
 * @brief Propagate enumeration started event
 *
 * @param[in] node_uid Unique node ID of the enumerating device
 * @param[in] dev_hdl Handle of the enumerating device
 *
 * @return
 *    - ESP_OK: Event propagated
 *    - ESP_ERR_INVALID_STATE: Enum callback not registered, enum_register_mock_callback() must be called first
 */
esp_err_t enum_mock_event_started(unsigned int node_uid, usb_device_handle_t dev_hdl);

/**
 * @brief Propagate enumeration reset required event
 *
 * @param[in] node_uid Unique node ID of the enumerating device
 * @param[in] dev_hdl Handle of the enumerating device
 *
 * @return
 *    - ESP_OK: Event propagated
 *    - ESP_ERR_INVALID_STATE: Enum callback not registered, enum_register_mock_callback() must be called first
 */
esp_err_t enum_mock_event_reset_required(unsigned int node_uid, usb_device_handle_t dev_hdl);

/**
 * @brief Propagate enumeration completed event
 *
 * @param[in] node_uid Unique node ID of the enumerated device
 * @param[in] dev_hdl Handle of the enumerated device
 *
 * @return
 *    - ESP_OK: Event propagated
 *    - ESP_ERR_INVALID_STATE: Enum callback not registered, enum_register_mock_callback() must be called first
 */
esp_err_t enum_mock_event_completed(unsigned int node_uid, usb_device_handle_t dev_hdl);

/**
 * @brief Propagate enumeration canceled event
 *
 * @param[in] node_uid Unique node ID of the canceled enumeration
 * @param[in] dev_hdl Handle of the enumerating device
 *
 * @return
 *    - ESP_OK: Event propagated
 *    - ESP_ERR_INVALID_STATE: Enum callback not registered, enum_register_mock_callback() must be called first
 */
esp_err_t enum_mock_event_canceled(unsigned int node_uid, usb_device_handle_t dev_hdl);

// ------------------------------------------------- Processing requests -----------------------------------------------

/**
 * @brief Propagate USBH processing request
 *
 * @return
 *    - ESP_OK: Processing request propagated
 *    - ESP_ERR_INVALID_STATE: Processing request callback not registered, usb_register_mock_callback() must be called first
 */
esp_err_t usbh_mock_proc_req(void);

/**
 * @brief Propagate Enum processing request
 *
 * @return
 *    - ESP_OK: Processing request propagated
 *    - ESP_ERR_INVALID_STATE: Processing request callback not registered, enum_register_mock_callback() must be called first
 */
esp_err_t enum_mock_proc_req(void);

/**
 * @brief Propagate Hub processing request
 *
 * @return
 *    - ESP_OK: Processing request propagated
 *    - ESP_ERR_INVALID_STATE: Processing request callback not registered, hub_register_mock_callback() must be called first
 */
esp_err_t hub_mock_proc_req(void);

#ifdef __cplusplus
}
#endif
