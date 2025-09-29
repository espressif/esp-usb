/*
 * SPDX-FileCopyrightText: 2015-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdlib.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "esp_err.h"
#include "usb_private.h"
#include "usbh.h"

#if CONFIG_USB_HOST_HUBS_SUPPORTED
#define ENABLE_USB_HUBS                     1
#endif // CONFIG_USB_HOST_HUBS_SUPPORTED

#ifdef __cplusplus
extern "C" {
#endif

// ----------------------------- Events ----------------------------------------
typedef enum {
    HUB_EVENT_CONNECTED,            /**< Device has been connected */
    HUB_EVENT_RESET_COMPLETED,      /**< Device reset completed */
    HUB_EVENT_DISCONNECTED,         /**< Device has been disconnected */
} hub_event_t;

typedef struct {
    hub_event_t event;                          /**< HUB event ID */
    union {
        struct {
            unsigned int uid;                   /**< Unique device ID */
        } connected;                            /**< HUB_EVENT_DEV_CONNECTED specific data */

        struct {
            unsigned int uid;                   /**< Unique device ID */
        } reset_completed;                      /**< HUB_EVENT_RESET_COMPLETED specific data */

        struct {
            unsigned int uid;                   /**< Unique device ID */
        } disconnected;                         /**< HUB_EVENT_DEV_DISCONNECTED specific data */
    };
} hub_event_data_t;

// ---------------------- Callbacks ------------------------

/**
 * @brief Callback used to indicate that the USBH has an event
 *
 * @note This callback is called from within usbh_process()
 */
typedef void (*hub_event_cb_t)(hub_event_data_t *event_data, void *arg);

// ------------------------------------------------------ Types --------------------------------------------------------

/**
 * @brief Hub driver configuration
 */
typedef struct {
    unsigned port_map;                              /**< Bitmap of root ports to enable */
    usb_proc_req_cb_t proc_req_cb;                  /**< Processing request callback */
    void *proc_req_cb_arg;                          /**< Processing request callback argument */
    hub_event_cb_t event_cb;                        /**< Hub event callback */
    void *event_cb_arg;                             /**< Hub event callback argument */
} hub_config_t;

// ---------------------------------------------- Hub Driver Functions -------------------------------------------------

/**
 * @brief Install Hub driver
 *
 * Entry:
 * - USBH must already be installed
 * Exit:
 * - Install Hub driver memory resources
 * - Initializes the HCD root port
 *
 * @param[in] hub_config Hub driver configuration
 * @param[out] client_ret Unique pointer to identify the Hub as a USB Host client
 *
 * @return
 *    - ESP_OK: Hub driver installed successfully
 *    - ESP_ERR_INVALID_STATE: Hub driver is not in correct state to be installed
 *    - ESP_ERR_NO_MEM: Insufficient memory
 *    - ESP_ERR_INVALID_ARG: Arguments are invalid
 */
esp_err_t hub_install(hub_config_t *hub_config, void **client_ret);

/**
 * @brief Uninstall Hub driver
 *
 * This must be called before uninstalling the USBH
 * Entry:
 * - Must have stopped the root port
 * Exit:
 * - HCD root port deinitialized
 *
 * @return
 *    - ESP_OK: Hub driver uninstalled successfully
 *    - ESP_ERR_INVALID_STATE: Hub driver is not installed, or root port is in other state than not powered
 */
esp_err_t hub_uninstall(void);

/**
 * @brief Start the Hub driver's root port
 *
 * This will power the root port ON
 *
 * @note This function should only be called from the Host Library task
 *
 * @param[in] port_idx  Root port index
 *
 * @return
 *    - ESP_OK: Hub driver started successfully
 *    - ESP_ERR_INVALID_STATE: Hub driver is not installed, or root port is in other state than not powered
 */
esp_err_t hub_root_start(uint8_t port_idx);

/**
 * @brief Stops the Hub driver's root port
 *
 * This will power OFF the root port
 *
 * @param[in] port_idx  Root port index
 *
 * @return
 *    - ESP_OK: Hub driver started successfully
 *    - ESP_ERR_INVALID_STATE: Hub driver is not installed, or root port is in not powered state
 */
esp_err_t hub_root_stop(uint8_t port_idx);

/**
 * @brief Indicate to the Hub driver that a device's port can be recycled
 *
 * The device connected to the port has been freed.
 * The Hub driver can now recycled the node and re-enable the port while it it still present.
 *
 * @note This function should only be called from the Host Library task
 *
 * @param[in] node_uid  Device's node unique ID
 *
 * @return
 *    - ESP_OK: device's port can be recycled
 *    - ESP_ERR_INVALID_STATE: Hub driver is not installed
 *    - ESP_ERR_NOT_SUPPORTED: Recycling External Port is not available (External Hub support disabled),
 *      or ext hub port error
 *    - ESP_ERR_NOT_FOUND: Device's node is not found
 */
esp_err_t hub_node_recycle(unsigned int node_uid);

/**
 * @brief Reset the device in the port, related to the specific Device Tree node
 *
 * @note This function should only be called from the Host Library task
 *
 * @param[in] node_uid  Device's node unique ID
 *
 * @return
 *    - ESP_OK if device in port reset successful
 *    - ESP_ERR_INVALID_STATE: Hub driver is not installed
 *    - ESP_ERR_NOT_SUPPORTED: Resetting External Port is not available (External Hub support disabled),
 *      or ext hub port error
 *    - ESP_ERR_NOT_FOUND: Device's node is not found
 */
esp_err_t hub_node_reset(unsigned int node_uid);

/**
 * @brief Port, related to the specific Device Tree node, has an active device
 *
 * @note This function should only be called from the Host Library task
 *
 * @param[in] node_uid  Device's node unique ID
 *
 * @return
 *    - ESP_OK if Port, related to the Device Tree node was activated successfully
 *    - ESP_ERR_NOT_SUPPORTED if activating Port is not available (External Hub support disabled),
 *      or ext hub port error
 *    - ESP_ERR_NOT_FOUND if Device's node is not found
 */
esp_err_t hub_node_active(unsigned int node_uid);

/**
 * @brief Disable the port, related to the specific Device Tree node
 *
 * @note This function should only be called from the Host Library task
 *
 * @param[in] node_uid  Device's node unique ID
 *
 * @return
 *    - ESP_OK:                  Port has been disabled without error
 *    - ESP_ERR_INVALID_STATE:   Port can't be disabled in current state
 *    - ESP_ERR_NOT_SUPPORTED:   This function is not support by the selected port
 *    - ESP_ERR_NOT_FOUND:       Device's node is not found
 */
esp_err_t hub_node_disable(unsigned int node_uid);

/**
 * @brief Get the node information of the device
 *
 * @note This function should only be called from the Host Library task
 *
 * @param[in] node_uid  Device's node unique ID
 * @param[out] info     Device information
 *
 * @return
 *    - ESP_ERR_NOT_ALLOWED if the Hub driver is not installed
 *    - ESP_ERR_INVALID_ARG if the info pointer is NULL
 *    - ESP_ERR_NOT_FOUND if the device node is not found
 *    - ESP_OK if Device's information obtained successfully
 */
esp_err_t hub_node_get_info(unsigned int node_uid, usb_parent_dev_info_t *info);

/**
 * @brief Notify Hub driver that new device has been attached
 *
 * If device is has a HUB class, then it will be added as External Hub to Hub Driver.
 *
 * @note This function should only be called from the Host Library task
 * @note If the Hub support feature is disabled and device has a Hub class, only the warning message will be shown.
 *
 * @param[in] dev_addr  Device bus address
 *
 * @return
 *    - ESP_OK: New device added successfully
 *    - ESP_ERR_INVALID_STATE: Hub driver is not in a correct state
 */
esp_err_t hub_dev_new(uint8_t dev_addr);

/**
 * @brief Notify Hub driver that device has been removed
 *
 * If the device was an External Hub, then it will be removed from the Hub Driver.
 *
 * @note This function should only be called from the Host Library task
 * @note If the Hub support feature is disabled, no additional logic requires here.
 *
 * @param[in] dev_addr  Device bus address
 *
 * @return
 *    - ESP_OK: A device removed successfully
 *    - ESP_ERR_INVALID_STATE: Hub driver is not in a correct state
 */
esp_err_t hub_dev_gone(uint8_t dev_addr);

#if ENABLE_USB_HUBS
/**
 * @brief Notify Hub driver that all devices should be freed
 *
 * @note This function should only be called from the Host Library task
 *
 * @return
 *    - ESP_OK: All the devices can be freed
 *    - ESP_ERR_INVALID_STATE: Hub driver is not in a correct state
 */
esp_err_t hub_notify_all_free(void);
#endif // ENABLE_USB_HUBS

/**
 * @brief Hub driver's processing function
 *
 * Hub driver handling function that must be called repeatedly to process the Hub driver's events. If blocking, the
 * caller can block on the notification callback of source USB_PROC_REQ_SOURCE_HUB to run this function.
 *
 * @return
 *    - ESP_OK: All events handled
 */
esp_err_t hub_process(void);

#ifdef __cplusplus
}
#endif
