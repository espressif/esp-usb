/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cdc_host_common.h
 * @brief Common USB Host CDC driver.
 *
 * Reference-counted USB Host client shared by CDC-ACM and CDC-like class drivers (CH34x, CP210x, FTDI, ...).
 * Hides USB Host client registration, device tracking, CDC descriptor parsing, notification / BULK IN polling
 * and optional per-port RX/TX ring buffers.
 *
 * Typical flow: `cdc_host_common_acquire()` -> optional `cdc_host_common_register_dev_event_cb()` ->
 * `cdc_host_common_open()` -> data/control APIs -> `cdc_host_common_close()` -> `cdc_host_common_release()`.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "usb/usb_host.h"
#include "usb/usb_types_cdc.h"

#ifdef USB_HOST_LIB_EVENT_FLAGS_AUTO_SUSPEND
/** @brief Defined when the underlying ESP-IDF exposes USB Host suspend and resume support. */
#define CDC_HOST_COMMON_SUSPEND_RESUME_API_SUPPORTED
#endif

#ifdef REMOTE_WAKE_HAL_SUPPORTED
/** @brief Defined when the underlying ESP-IDF exposes remote wakeup support for USB host devices. */
#define CDC_HOST_COMMON_REMOTE_WAKE_SUPPORTED
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Match any USB vendor ID when opening a CDC device. */
#define CDC_HOST_COMMON_ANY_VID      (0)
/** @brief Match any USB product ID when opening a CDC device. */
#define CDC_HOST_COMMON_ANY_PID      (0)
/** @brief Match any USB device address when opening a CDC device. */
#define CDC_HOST_COMMON_ANY_DEV_ADDR (0)

/** @brief Opaque handle to a common CDC host driver instance. */
typedef struct cdc_host_common_driver_s *cdc_host_common_driver_handle_t;
/** @brief Opaque handle to an opened common CDC host port (one interface). */
typedef struct cdc_host_common_port_s *cdc_host_common_port_handle_t;
/** @brief Opaque handle to a registered device-event callback. */
typedef struct cdc_host_common_dev_event_cb_s *cdc_host_common_dev_event_cb_handle_t;

/** @brief Device-level events reported to registered device-event callbacks. */
typedef enum {
    CDC_HOST_COMMON_DEV_EVENT_NEW = 0,       /*!< A USB device is available for CDC probing/opening; `dev_hdl` is valid only during the callback. */
    CDC_HOST_COMMON_DEV_EVENT_GONE,          /*!< A USB device was removed; `dev_hdl` may be NULL for devices never opened by the common driver. */
} cdc_host_common_dev_event_t;

/** @brief Port-level events reported to the per-port event callback. */
typedef enum {
    CDC_HOST_COMMON_PORT_EVENT_ERROR = 0,    /*!< USB transfer error; see `data.error`. */
    CDC_HOST_COMMON_PORT_EVENT_NOTIFICATION, /*!< CDC notification received on the interrupt IN endpoint; see `data.notification`. */
    CDC_HOST_COMMON_PORT_EVENT_DISCONNECTED, /*!< Underlying device was removed; the port must be treated as closed. */
#ifdef CDC_HOST_COMMON_SUSPEND_RESUME_API_SUPPORTED
    CDC_HOST_COMMON_PORT_EVENT_SUSPENDED,    /*!< USB bus suspended (only when the ESP-IDF exposes suspend/resume support). */
    CDC_HOST_COMMON_PORT_EVENT_RESUMED,      /*!< USB bus resumed (only when the ESP-IDF exposes suspend/resume support). */
#endif
    CDC_HOST_COMMON_PORT_EVENT_RX_OVERFLOW,  /*!< RX ring buffer overflowed and one or more incoming bytes were dropped. */
} cdc_host_common_port_event_t;

/** @brief Flags controlling per-port open behavior. */
typedef enum {
    CDC_HOST_COMMON_OPEN_FLAG_NONE = 0,                      /*!< No special flags. */
    CDC_HOST_COMMON_OPEN_FLAG_DISABLE_NOTIFICATION = 1 << 0, /*!< Do not claim the notification interface and skip interrupt IN polling. */
} cdc_host_common_open_flags_t;

/**
 * @brief Configuration for the common CDC host driver task.
 *
 * Only consulted on the acquire that actually creates the driver; subsequent acquires just bump the refcount.
 */
typedef struct {
    size_t task_stack_size;    /*!< Stack size of the common driver task, in bytes. */
    unsigned task_priority;    /*!< FreeRTOS priority of the common driver task; should be higher than any calling task. */
    int task_coreid;           /*!< Core affinity of the common driver task, or -1 for no affinity. */
} cdc_host_common_driver_config_t;

/** @brief Data delivered with a device-level event. */
typedef struct {
    cdc_host_common_dev_event_t type;              /*!< Event type; selects the active union member. */
    union {
        struct {
            usb_device_handle_t dev_hdl;           /*!< Temporarily opened device handle, valid only during the callback. */
            uint8_t dev_addr;                      /*!< USB device address on the bus. */
            const usb_device_desc_t *device_desc;  /*!< Device descriptor, valid only during the callback. */
            const usb_config_desc_t *config_desc;  /*!< Active configuration descriptor, valid only during the callback. */
        } new_dev;                                 /*!< Payload for @ref CDC_HOST_COMMON_DEV_EVENT_NEW. */
        struct {
            usb_device_handle_t dev_hdl;           /*!< Device handle from the USB Host removal event, or NULL if only address is known. */
            uint8_t dev_addr;                      /*!< USB device address, or 0 if not available. */
        } dev_gone;                                /*!< Payload for @ref CDC_HOST_COMMON_DEV_EVENT_GONE. */
    } data;                                        /*!< Event payload; select the member according to `type`. */
} cdc_host_common_dev_event_data_t;

/** @brief Data delivered with a port-level event. */
typedef struct {
    cdc_host_common_port_event_t type;             /*!< Event type; selects the active union member. */
    union {
        int error;                                 /*!< USB transfer status, valid for @ref CDC_HOST_COMMON_PORT_EVENT_ERROR. */
        struct {
            const uint8_t *data;                   /*!< Raw notification bytes, valid only during the callback. */
            size_t data_len;                       /*!< Length of the notification in bytes. */
        } notification;                            /*!< Payload for @ref CDC_HOST_COMMON_PORT_EVENT_NOTIFICATION. */
        cdc_host_common_port_handle_t port;        /*!< Port related to disconnect/suspend/resume events. */
    } data;                                        /*!< Event payload; select the member according to `type`. */
} cdc_host_common_port_event_data_t;

/**
 * @brief BULK IN data callback.
 *
 * @param[in] port     Port that produced the data.
 * @param[in] data     Received bytes, valid only during the callback.
 * @param[in] data_len Length of `data` in bytes.
 * @param[in] user_arg User argument from @ref cdc_host_common_open_config_t.
 * @return true if the callback consumed the data (driver keeps buffer as-is);
 *         false to fall back to the RX ring buffer (or drop the chunk if none was created).
 */
typedef bool (*cdc_host_common_data_cb_t)(cdc_host_common_port_handle_t port, const uint8_t *data, size_t data_len, void *user_arg);

/**
 * @brief Port-level event callback.
 *
 * @param[in] port     Port the event belongs to.
 * @param[in] event    Event payload, valid only during the callback.
 * @param[in] user_arg User argument from @ref cdc_host_common_open_config_t.
 */
typedef void (*cdc_host_common_port_event_cb_t)(cdc_host_common_port_handle_t port, const cdc_host_common_port_event_data_t *event, void *user_arg);

/**
 * @brief Device-level event callback.
 *
 * Called from the common driver context; must not open a CDC port directly and should not block for long.
 *
 * @param[in] event    Event payload, valid only during the callback.
 * @param[in] user_arg User argument set at registration time.
 */
typedef void (*cdc_host_common_dev_event_cb_t)(const cdc_host_common_dev_event_data_t *event, void *user_arg);

/** @brief Configuration for opening a CDC (or CDC-like) interface. */
typedef struct {
    uint16_t vid;                              /*!< Device vendor ID, or @ref CDC_HOST_COMMON_ANY_VID to match any vendor. */
    uint16_t pid;                              /*!< Device product ID, or @ref CDC_HOST_COMMON_ANY_PID to match any product. */
    uint8_t dev_addr;                          /*!< Device address, or @ref CDC_HOST_COMMON_ANY_DEV_ADDR to match any address. */
    uint8_t interface_idx;                     /*!< CDC communication or vendor-specific interface index to open. */
    uint32_t connection_timeout_ms;            /*!< Wait for a matching device up to this many ms; 0 waits forever. */
    size_t ctrl_buffer_size;                   /*!< Max control payload size in bytes; 0 defaults to 64. */
    size_t in_buffer_size;                     /*!< BULK IN transfer size; 0 disables IN polling, or falls back to MPS when `data_cb`/`rx_ringbuf_size` is set. */
    size_t out_buffer_size;                    /*!< BULK OUT transfer size; 0 opens the port read-only. */
    size_t rx_ringbuf_size;                    /*!< Optional RX ring buffer size; 0 disables buffered RX. */
    size_t tx_ringbuf_size;                    /*!< Optional TX ring buffer size; 0 disables buffered TX. Non-zero requires `out_buffer_size > 0`. */
    cdc_host_common_open_flags_t flags;        /*!< Bitwise OR of @ref cdc_host_common_open_flags_t values. */
    cdc_host_common_data_cb_t data_cb;         /*!< BULK IN data callback; may be NULL. */
    cdc_host_common_port_event_cb_t event_cb;  /*!< Port event callback; may be NULL. */
    void *user_arg;                            /*!< User argument passed to both `data_cb` and `event_cb`. */
} cdc_host_common_open_config_t;

/**
 * @brief Acquire a reference to the common CDC host driver.
 *
 * The USB Host Library must be installed first. The first successful call creates the shared client task;
 * subsequent calls just increment the refcount. Each successful acquire must be paired with one release.
 *
 * @param[in]  config     Driver configuration, or NULL for defaults (ignored while the driver is already alive).
 * @param[out] driver_ret Returned driver handle. Must not be NULL.
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if `driver_ret` is NULL
 *      - ESP_ERR_NO_MEM if the driver or its task cannot be allocated
 *      - Other error codes from USB Host client registration
 */
esp_err_t cdc_host_common_acquire(const cdc_host_common_driver_config_t *config, cdc_host_common_driver_handle_t *driver_ret);

/**
 * @brief Release a reference to the common CDC host driver.
 *
 * When the refcount reaches zero all remaining device-event callbacks are unregistered, the client task is
 * torn down and the driver is freed. All ports must be closed before the last release.
 *
 * @param[in] driver Driver handle from @ref cdc_host_common_acquire.
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if `driver` is NULL or does not refer to the active driver instance
 *      - ESP_ERR_INVALID_STATE if any port is still open
 *      - ESP_ERR_NOT_FINISHED if the client task does not tear down in time (the refcount is restored)
 */
esp_err_t cdc_host_common_release(cdc_host_common_driver_handle_t driver);

/**
 * @brief Register a callback for device-level events.
 *
 * Called for every new USB device (not only CDC-compliant ones) and every removal. Runs on the common
 * driver task, so it must not open a port or block for long; hand off to an application task if needed.
 * Multiple callbacks can be registered.
 *
 * @param[in]  driver        Driver handle from @ref cdc_host_common_acquire.
 * @param[in]  cb            Callback function. Must not be NULL.
 * @param[in]  user_arg      Opaque argument passed back to `cb`.
 * @param[out] cb_handle_ret Returned callback handle used to unregister later. Must not be NULL.
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if any required argument is NULL
 *      - ESP_ERR_NO_MEM if the callback entry cannot be allocated
 */
esp_err_t cdc_host_common_register_dev_event_cb(cdc_host_common_driver_handle_t driver, cdc_host_common_dev_event_cb_t cb,
                                                void *user_arg, cdc_host_common_dev_event_cb_handle_t *cb_handle_ret);

/**
 * @brief Unregister a device-event callback. The handle becomes invalid on success.
 *
 * @param[in] cb_handle Callback handle from @ref cdc_host_common_register_dev_event_cb.
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if `cb_handle` is NULL or the driver is not acquired
 *      - ESP_ERR_NOT_FOUND if the handle does not match any registered callback
 */
esp_err_t cdc_host_common_unregister_dev_event_cb(cdc_host_common_dev_event_cb_handle_t cb_handle);

/**
 * @brief Open a CDC (or CDC-like) interface on a USB device.
 *
 * Waits up to `connection_timeout_ms` for a device matching `vid`/`pid`/`dev_addr`, parses the requested
 * interface, allocates transfers and ring buffers, claims the interface(s) and starts BULK IN / interrupt IN
 * polling. Falls back to a two-bulk-endpoint vendor-specific layout when the CDC descriptors are not present.
 *
 * @param[in]  driver      Driver handle from @ref cdc_host_common_acquire.
 * @param[in]  open_config Open configuration. Must not be NULL.
 * @param[out] port_ret    Returned port handle. Must not be NULL; set to NULL on failure.
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if any required argument is NULL, or if a TX ring buffer is requested without BULK OUT
 *      - ESP_ERR_INVALID_STATE if the interface is already opened on this device
 *      - ESP_ERR_NO_MEM if port, ring buffers or transfers cannot be allocated
 *      - ESP_ERR_NOT_FOUND if no matching device appears before the timeout
 *      - Other error codes from descriptor queries, CDC parsing or USB Host interface claim
 */
esp_err_t cdc_host_common_open(cdc_host_common_driver_handle_t driver, const cdc_host_common_open_config_t *open_config,
                               cdc_host_common_port_handle_t *port_ret);

/**
 * @brief Close a previously opened CDC port.
 *
 * Cancels in-flight transfers, releases claimed interfaces and frees port resources. Calling this on a port
 * that is already closing (for example after a disconnect event) succeeds immediately.
 *
 * @param[in] port Port handle from @ref cdc_host_common_open.
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if `port` is NULL / already fully closed, or if the driver is not acquired
 */
esp_err_t cdc_host_common_close(cdc_host_common_port_handle_t port);

/**
 * @brief Write data to the port.
 *
 * Without a TX ring buffer the call blocks until the data is transmitted (an internal default timeout is used
 * when `ticks_to_wait` is `portMAX_DELAY`). With a TX ring buffer the data is pushed to the ring and the
 * driver task drains it asynchronously; the call only blocks waiting for space.
 *
 * @param[in] port          Port handle from @ref cdc_host_common_open.
 * @param[in] data          Data to send. Must not be NULL.
 * @param[in] data_len      Number of bytes to send. Must be greater than 0.
 * @param[in] ticks_to_wait Max FreeRTOS ticks to wait for space / completion; `portMAX_DELAY` for the default blocking behavior.
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if any required argument is invalid
 *      - ESP_ERR_NOT_SUPPORTED if the port was opened read-only
 *      - ESP_ERR_TIMEOUT if the write times out
 *      - Other error codes from the underlying blocking TX or ring buffer push
 */
esp_err_t cdc_host_common_write_bytes(cdc_host_common_port_handle_t port, const uint8_t *data, size_t data_len, TickType_t ticks_to_wait);

/**
 * @brief Read data from the port.
 *
 * With an RX ring buffer, pops up to `*length` bytes (clamped to the ring size) into `buf` and updates
 * `*length` with the number of bytes returned. Without an RX ring buffer, `*length` must equal the current
 * buffered chunk size (query with @ref cdc_host_common_get_rx_buffer_size first).
 *
 * @param[in]     port          Port handle from @ref cdc_host_common_open.
 * @param[out]    buf           Destination buffer. Must not be NULL.
 * @param[inout]  length        In: buffer capacity in bytes; out: number of bytes written (0 on failure).
 * @param[in]     ticks_to_wait Max FreeRTOS ticks to wait (only used in ring buffer mode).
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if any argument is invalid, or `*length` does not match the current unbuffered chunk
 *      - ESP_ERR_INVALID_STATE if no unbuffered data is currently available
 *      - Other error codes from the RX ring buffer
 */
esp_err_t cdc_host_common_read_bytes(cdc_host_common_port_handle_t port, uint8_t *buf, size_t *length, TickType_t ticks_to_wait);

/**
 * @brief Flush the port's RX ring buffer.
 *
 * @param[in] port Port handle from @ref cdc_host_common_open.
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG for invalid `port`; ESP_ERR_NOT_SUPPORTED if no RX ring buffer.
 */
esp_err_t cdc_host_common_flush_rx_buffer(cdc_host_common_port_handle_t port);

/**
 * @brief Flush the port's TX ring buffer.
 *
 * Waits for the outstanding TX transfer and discards any bytes still queued in the TX ring buffer.
 *
 * @param[in] port Port handle from @ref cdc_host_common_open.
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if `port` is invalid
 *      - ESP_ERR_NOT_SUPPORTED if the port was opened without a TX ring buffer
 *      - ESP_ERR_TIMEOUT if the TX mutex cannot be acquired within the internal timeout
 */
esp_err_t cdc_host_common_flush_tx_buffer(cdc_host_common_port_handle_t port);

/**
 * @brief Get the amount of RX data currently available (ring buffer bytes, or size of the last unbuffered chunk).
 *
 * @param[in]  port Port handle from @ref cdc_host_common_open.
 * @param[out] size Returned size in bytes. Must not be NULL.
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG if any argument is invalid.
 */
esp_err_t cdc_host_common_get_rx_buffer_size(cdc_host_common_port_handle_t port, size_t *size);

/**
 * @brief Send a control (SETUP) request on the port using its shared control buffer.
 *
 * For OUT requests, `data` provides the payload; for IN requests, the received payload is copied back into `data`.
 *
 * @param[in]    port          Port handle from @ref cdc_host_common_open.
 * @param[in]    bmRequestType `bmRequestType` field of the USB setup packet.
 * @param[in]    bRequest      `bRequest` field of the USB setup packet.
 * @param[in]    wValue        `wValue` field of the USB setup packet.
 * @param[in]    wIndex        `wIndex` field of the USB setup packet.
 * @param[in]    wLength       Payload length in bytes.
 * @param[inout] data          Payload buffer. Must not be NULL when `wLength > 0`.
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if `port` is invalid or `data` is NULL while `wLength > 0`
 *      - ESP_ERR_INVALID_STATE if the underlying USB device is not open
 *      - ESP_ERR_INVALID_SIZE if `wLength` exceeds the configured control buffer size
 *      - ESP_ERR_TIMEOUT if the control mutex or the transfer times out
 *      - ESP_ERR_INVALID_RESPONSE on non-`COMPLETED` transfer status
 *      - Other error codes from USB control transfer submission
 */
esp_err_t cdc_host_common_send_control(cdc_host_common_port_handle_t port, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue,
                                       uint16_t wIndex, uint16_t wLength, uint8_t *data);

/**
 * @brief Peek at the current unbuffered RX chunk without copying it.
 *
 * The returned pointer is only valid until the next BULK IN transfer completes or the port is closed.
 *
 * @param[in]  port     Port handle from @ref cdc_host_common_open.
 * @param[out] data     Returned pointer to received bytes. Must not be NULL.
 * @param[out] data_len Returned length in bytes. Must not be NULL.
 * @return ESP_OK if data is available; ESP_ERR_INVALID_ARG on invalid argument; ESP_ERR_INVALID_STATE if no data.
 */
esp_err_t cdc_host_common_get_rx_data(cdc_host_common_port_handle_t port, const uint8_t **data, size_t *data_len);

/**
 * @brief Get the USB device handle backing the port. The handle is owned by the driver and must not be closed by the caller.
 *
 * @param[in]  port    Port handle from @ref cdc_host_common_open.
 * @param[out] dev_hdl Returned USB device handle. Must not be NULL.
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG on invalid argument; ESP_ERR_INVALID_STATE if the device is no longer open.
 */
esp_err_t cdc_host_common_get_dev_handle(cdc_host_common_port_handle_t port, usb_device_handle_t *dev_hdl);

/**
 * @brief Get the CDC notification and data interface descriptors. Either output pointer may be NULL to skip that value.
 *
 * `*notif_intf` is set to NULL when the port has no separate notification interface (e.g. two-bulk vendor devices).
 *
 * @param[in]  port       Port handle from @ref cdc_host_common_open.
 * @param[out] notif_intf Returned notification interface descriptor, or NULL to ignore.
 * @param[out] data_intf  Returned data interface descriptor, or NULL to ignore.
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG if `port` is invalid.
 */
esp_err_t cdc_host_common_get_intf_desc(cdc_host_common_port_handle_t port, const usb_intf_desc_t **notif_intf, const usb_intf_desc_t **data_intf);

/**
 * @brief Get the protocols reported by the CDC interface descriptors. Either output pointer may be NULL.
 *
 * @param[in]  port Port handle from @ref cdc_host_common_open.
 * @param[out] comm Returned communication protocol, or NULL to ignore.
 * @param[out] data Returned data protocol, or NULL to ignore.
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG if `port` is invalid.
 */
esp_err_t cdc_host_common_protocols_get(cdc_host_common_port_handle_t port, cdc_comm_protocol_t *comm, cdc_data_protocol_t *data);

/**
 * @brief Get a CDC functional descriptor of the requested subtype.
 *
 * @param[in]  port      Port handle from @ref cdc_host_common_open.
 * @param[in]  desc_type Functional descriptor subtype to retrieve.
 * @param[out] desc_out  Returned descriptor pointer. Must not be NULL.
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG on invalid argument or `desc_type` out of range; ESP_ERR_NOT_FOUND if not present.
 */
esp_err_t cdc_host_common_cdc_desc_get(cdc_host_common_port_handle_t port, cdc_desc_subtype_t desc_type, const usb_standard_desc_t **desc_out);

/**
 * @brief Print the device and active configuration descriptors of the port's USB device to stdout.
 *
 * @param[in] port Port handle from @ref cdc_host_common_open.
 */
void cdc_host_common_desc_print(cdc_host_common_port_handle_t port);

#ifdef CDC_HOST_COMMON_REMOTE_WAKE_SUPPORTED
/**
 * @brief Enable or disable remote wakeup on the port's device (SET/CLEAR_FEATURE(DEVICE_REMOTE_WAKEUP)).
 *
 * @note Available only when the underlying ESP-IDF exposes remote wakeup support (@ref CDC_HOST_COMMON_REMOTE_WAKE_SUPPORTED).
 *
 * @param[in] port   Port handle from @ref cdc_host_common_open.
 * @param[in] enable true to enable remote wakeup, false to disable.
 * @return
 *      - ESP_OK on success, or if the requested state is already configured
 *      - ESP_ERR_INVALID_ARG if `port` is invalid
 *      - ESP_ERR_INVALID_STATE if the underlying USB device is not open
 *      - ESP_ERR_NOT_SUPPORTED if the device does not advertise remote wakeup in its configuration descriptor
 *      - ESP_ERR_TIMEOUT if the control mutex or the transfer times out
 *      - ESP_ERR_INVALID_RESPONSE on non-`COMPLETED` transfer status
 *      - Other error codes from USB descriptor queries or control transfer submission
 */
esp_err_t cdc_host_common_enable_remote_wakeup(cdc_host_common_port_handle_t port, bool enable);
#endif

#ifdef __cplusplus
}
#endif
