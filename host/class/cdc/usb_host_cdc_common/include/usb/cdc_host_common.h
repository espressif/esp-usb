/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "usb/usb_host.h"
#include "usb/usb_types_cdc.h"

#ifdef USB_HOST_LIB_EVENT_FLAGS_AUTO_SUSPEND
/**
 * @brief Defined when the underlying ESP-IDF exposes USB Host suspend and resume support.
 */
#define CDC_HOST_COMMON_SUSPEND_RESUME_API_SUPPORTED
#endif

#ifdef REMOTE_WAKE_HAL_SUPPORTED
/**
 * @brief Defined when the underlying ESP-IDF exposes remote wakeup support for USB host devices.
 */
#define CDC_HOST_COMMON_REMOTE_WAKE_SUPPORTED
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define CDC_HOST_COMMON_ANY_VID      (0)
#define CDC_HOST_COMMON_ANY_PID      (0)
#define CDC_HOST_COMMON_ANY_DEV_ADDR (0)

typedef struct cdc_host_common_driver_s *cdc_host_common_driver_handle_t;
typedef struct cdc_host_common_port_s *cdc_host_common_port_handle_t;
typedef struct cdc_host_common_dev_event_cb_s *cdc_host_common_dev_event_cb_handle_t;

typedef enum {
    CDC_HOST_COMMON_DEV_EVENT_NEW = 0,                  /*!< A USB device is available for CDC probing/opening. */
    CDC_HOST_COMMON_DEV_EVENT_GONE,                     /*!< A USB device was removed; dev_hdl can be NULL for unopened devices. */
} cdc_host_common_dev_event_t;

typedef enum {
    CDC_HOST_COMMON_PORT_EVENT_ERROR = 0,
    CDC_HOST_COMMON_PORT_EVENT_NOTIFICATION,
    CDC_HOST_COMMON_PORT_EVENT_DISCONNECTED,            /*!< An opened CDC port belongs to a removed device and must be treated as closed. */
#ifdef CDC_HOST_COMMON_SUSPEND_RESUME_API_SUPPORTED
    CDC_HOST_COMMON_PORT_EVENT_SUSPENDED,
    CDC_HOST_COMMON_PORT_EVENT_RESUMED,
#endif
} cdc_host_common_port_event_t;

typedef enum {
    CDC_HOST_COMMON_OPEN_FLAG_NONE = 0,
    CDC_HOST_COMMON_OPEN_FLAG_DISABLE_NOTIFICATION = 1 << 0,
} cdc_host_common_open_flags_t;

typedef struct {
    size_t task_stack_size;    /*!< Stack size of the common driver task */
    unsigned task_priority;    /*!< Priority of the common driver task */
    int task_coreid;           /*!< Core affinity of the common driver task, -1 for no affinity */
} cdc_host_common_driver_config_t;

typedef struct {
    cdc_host_common_dev_event_t type;
    union {
        struct {
            usb_device_handle_t dev_hdl;               /*!< Temporary opened device handle, valid only during the callback */
            uint8_t dev_addr;                          /*!< Device address */
            const usb_device_desc_t *device_desc;      /*!< Device descriptor, valid only during the callback */
            const usb_config_desc_t *config_desc;      /*!< Active configuration descriptor, valid only during the callback */
        } new_dev;
        struct {
            usb_device_handle_t dev_hdl;               /*!< Device handle from USB Host event, or NULL when only a removal notification is available */
            uint8_t dev_addr;                          /*!< Device address, 0 if not available */
        } dev_gone;
    } data;
} cdc_host_common_dev_event_data_t;

typedef struct {
    cdc_host_common_port_event_t type;
    union {
        int error;                                     /*!< USB transfer status on error */
        struct {
            const uint8_t *data;                       /*!< Raw notification bytes, valid only during the callback */
            size_t data_len;                           /*!< Raw notification length */
        } notification;
        cdc_host_common_port_handle_t port;            /*!< Port related to disconnect/suspend/resume */
    } data;
} cdc_host_common_port_event_data_t;

typedef bool (*cdc_host_common_data_cb_t)(cdc_host_common_port_handle_t port, const uint8_t *data, size_t data_len, void *user_arg);
typedef void (*cdc_host_common_port_event_cb_t)(cdc_host_common_port_handle_t port, const cdc_host_common_port_event_data_t *event, void *user_arg);
typedef void (*cdc_host_common_dev_event_cb_t)(const cdc_host_common_dev_event_data_t *event, void *user_arg);

typedef struct {
    uint16_t vid;                                      /*!< Device vendor ID, or CDC_HOST_COMMON_ANY_VID */
    uint16_t pid;                                      /*!< Device product ID, or CDC_HOST_COMMON_ANY_PID */
    uint8_t dev_addr;                                  /*!< Device address, or CDC_HOST_COMMON_ANY_DEV_ADDR */
    uint8_t interface_idx;                             /*!< CDC communication or vendor-specific interface index */
    uint32_t connection_timeout_ms;                    /*!< Device connection timeout in milliseconds, 0 waits forever */
    size_t ctrl_buffer_size;                           /*!< Maximum control payload size, 0 defaults to 64 bytes */
    size_t in_buffer_size;                             /*!< BULK IN transfer size, 0 disables IN polling */
    size_t out_buffer_size;                            /*!< BULK OUT transfer size, 0 opens read-only */
    size_t rx_ringbuf_size;                            /*!< Optional RX ring buffer size, 0 disables buffered RX */
    size_t tx_ringbuf_size;                            /*!< Optional TX ring buffer size, 0 disables buffered TX */
    cdc_host_common_open_flags_t flags;                /*!< Open behavior flags */
    cdc_host_common_data_cb_t data_cb;                 /*!< BULK IN data callback */
    cdc_host_common_port_event_cb_t event_cb;          /*!< Port event callback */
    void *user_arg;                                    /*!< Callback user argument */
} cdc_host_common_open_config_t;

esp_err_t cdc_host_common_acquire(const cdc_host_common_driver_config_t *config, cdc_host_common_driver_handle_t *driver_ret);
esp_err_t cdc_host_common_release(cdc_host_common_driver_handle_t driver);
esp_err_t cdc_host_common_register_dev_event_cb(cdc_host_common_driver_handle_t driver, cdc_host_common_dev_event_cb_t cb,
                                                void *user_arg, cdc_host_common_dev_event_cb_handle_t *cb_handle_ret);
esp_err_t cdc_host_common_unregister_dev_event_cb(cdc_host_common_dev_event_cb_handle_t cb_handle);
esp_err_t cdc_host_common_open(cdc_host_common_driver_handle_t driver, const cdc_host_common_open_config_t *open_config,
                               cdc_host_common_port_handle_t *port_ret);
esp_err_t cdc_host_common_close(cdc_host_common_port_handle_t port);
esp_err_t cdc_host_common_tx_blocking(cdc_host_common_port_handle_t port, const uint8_t *data, size_t data_len, uint32_t timeout_ms);
esp_err_t cdc_host_common_write_bytes(cdc_host_common_port_handle_t port, const uint8_t *data, size_t data_len, TickType_t ticks_to_wait);
esp_err_t cdc_host_common_read_bytes(cdc_host_common_port_handle_t port, uint8_t *buf, size_t *length, TickType_t ticks_to_wait);
esp_err_t cdc_host_common_flush_rx_buffer(cdc_host_common_port_handle_t port);
esp_err_t cdc_host_common_flush_tx_buffer(cdc_host_common_port_handle_t port);
esp_err_t cdc_host_common_get_rx_buffer_size(cdc_host_common_port_handle_t port, size_t *size);
esp_err_t cdc_host_common_send_control(cdc_host_common_port_handle_t port, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue,
                                       uint16_t wIndex, uint16_t wLength, uint8_t *data);
esp_err_t cdc_host_common_get_rx_data(cdc_host_common_port_handle_t port, const uint8_t **data, size_t *data_len);
esp_err_t cdc_host_common_get_dev_handle(cdc_host_common_port_handle_t port, usb_device_handle_t *dev_hdl);
esp_err_t cdc_host_common_get_intf_desc(cdc_host_common_port_handle_t port, const usb_intf_desc_t **notif_intf, const usb_intf_desc_t **data_intf);
esp_err_t cdc_host_common_protocols_get(cdc_host_common_port_handle_t port, cdc_comm_protocol_t *comm, cdc_data_protocol_t *data);
esp_err_t cdc_host_common_cdc_desc_get(cdc_host_common_port_handle_t port, cdc_desc_subtype_t desc_type, const usb_standard_desc_t **desc_out);
void cdc_host_common_desc_print(cdc_host_common_port_handle_t port);

#ifdef CDC_HOST_COMMON_REMOTE_WAKE_SUPPORTED
esp_err_t cdc_host_common_enable_remote_wakeup(cdc_host_common_port_handle_t port, bool enable);
#endif

#ifdef __cplusplus
}
#endif
