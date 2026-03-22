/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "usb/usb_host.h"
#include "esp_err.h"

/** @brief Wildcard vendor ID for opening a UVC stream. */
#define UVC_HOST_ANY_VID (0)
/** @brief Wildcard product ID for opening a UVC stream. */
#define UVC_HOST_ANY_PID (0)
/** @brief Wildcard USB device address for opening a UVC stream. */
#define UVC_HOST_ANY_DEV_ADDR (0)

#ifdef __cplusplus
extern "C" {
#endif

// For backward compatibility with IDF versions which do not have suspend/resume api
#ifdef USB_HOST_LIB_EVENT_FLAGS_AUTO_SUSPEND
/** @brief Indicates that suspend and resume events are available in this build. */
#define UVC_HOST_SUSPEND_RESUME_API_SUPPORTED
#endif

typedef struct uvc_host_stream_s *uvc_host_stream_hdl_t; /*!< UVC stream handle. */

/**
 * @brief UVC host driver event types.
 */
enum uvc_host_driver_event {
    UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED = 0x0, /*!< A compatible UVC device has been connected. */
};

/**
 * @brief Stream formats supported by this driver.
 */
enum uvc_host_stream_format {
    UVC_VS_FORMAT_DEFAULT = 0, /*!< Device default format. */
    UVC_VS_FORMAT_MJPEG,       /*!< MJPEG stream format. */
    UVC_VS_FORMAT_YUY2,        /*!< YUY2 stream format. */
    UVC_VS_FORMAT_H264,        /*!< H.264 stream format. */
    UVC_VS_FORMAT_H265,        /*!< H.265 stream format. */
};

/**
 * @brief UVC frame information.
 */
typedef struct {
    enum uvc_host_stream_format format;       /*!< Format of this frame buffer. */
    unsigned h_res;                           /*!< Horizontal resolution in pixels. */
    unsigned v_res;                           /*!< Vertical resolution in pixels. */
    uint32_t default_interval;                /*!< Default frame interval. */
    uint8_t interval_type;                    /*!< 0 for continuous range, non-zero for discrete interval count. */
    union {
        struct {
            uint32_t interval_min;            /*!< Minimum frame interval. */
            uint32_t interval_max;            /*!< Maximum frame interval. */
            uint32_t interval_step;           /*!< Frame interval step. */
        };
        uint32_t interval[CONFIG_UVC_INTERVAL_ARRAY_SIZE]; /*!< Discrete frame intervals. */
    };
} uvc_host_frame_info_t;

/**
 * @brief UVC host driver event data.
 */
typedef struct {
    enum uvc_host_driver_event type;      /*!< Event type. */
    union {
        struct {
            uint8_t dev_addr;             /*!< USB device address. */
            uint8_t uvc_stream_index;     /*!< Index of UVC function for this uvc stream. */
            size_t frame_info_num;        /*!< Number of entries available from uvc_host_get_frame_list(). */
        } device_connected;               /*!< Data for UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED. */
    };
} uvc_host_driver_event_data_t;

/**
 * @brief USB Host UVC driver event callback.
 *
 * @param[in] event Event structure.
 * @param[in] user_ctx User argument from uvc_host_driver_config_t.
 */
typedef void (*uvc_host_driver_event_callback_t)(const uvc_host_driver_event_data_t *event, void *user_ctx);

/**
 * @brief USB Host UVC driver configuration structure.
 */
typedef struct {
    size_t driver_task_stack_size;         /*!< Stack size of the driver's task. */
    unsigned driver_task_priority;         /*!< Priority of the driver's task. */
    int xCoreID;                           /*!< Core affinity of the driver's task. */
    bool create_background_task;           /*!< When set to true, a background task handles USB events.
                                                Otherwise call uvc_host_handle_events(). */
    uvc_host_driver_event_callback_t event_cb; /*!< Callback function that handles driver events. */
    void *user_ctx;                        /*!< User argument passed to event_cb. */
} uvc_host_driver_config_t;

/**
 * @brief UVC stream event types.
 */
enum uvc_host_dev_event {
    UVC_HOST_TRANSFER_ERROR,         /*!< USB transfer error */
    UVC_HOST_DEVICE_DISCONNECTED,    /*!< Device was suddenly disconnected. The stream is stopped. */
    UVC_HOST_FRAME_BUFFER_OVERFLOW,  /*!< Frame discarded because it exceeded buffer space. */
    UVC_HOST_FRAME_BUFFER_UNDERFLOW, /*!< Frame discarded because no free buffer was available. */
#ifdef UVC_HOST_SUSPEND_RESUME_API_SUPPORTED
    UVC_HOST_DEVICE_SUSPENDED,      /*!< Device was suspended. The stream is stopped. */
    UVC_HOST_DEVICE_RESUMED,        /*!< Device was resumed. */
#endif // UVC_HOST_SUSPEND_RESUME_API_SUPPORTED
};

/**
 * @brief UVC stream event data.
 */
typedef struct {
    enum uvc_host_dev_event type; /*!< Event type. */
    union {
        struct {
            esp_err_t error;                   /*!< Error code from USB Host. */
        } transfer_error;                      /*!< Data for UVC_HOST_TRANSFER_ERROR. */
        struct {
            uvc_host_stream_hdl_t stream_hdl;  /*!< Stream that was disconnected. */
        } device_disconnected;                 /*!< Data for UVC_HOST_DEVICE_DISCONNECTED. */
        struct {
        } frame_overflow;                      /*!< Data for UVC_HOST_FRAME_BUFFER_OVERFLOW. */
        struct {
        } frame_underflow;                     /*!< Data for UVC_HOST_FRAME_BUFFER_UNDERFLOW. */
#ifdef UVC_HOST_SUSPEND_RESUME_API_SUPPORTED
        struct {
            uvc_host_stream_hdl_t stream_hdl;  /*!< Stream affected by suspend or resume. */
        } device_suspended_resumed;            /*!< Data for suspend and resume events. */
#endif // UVC_HOST_SUSPEND_RESUME_API_SUPPORTED
    };
} uvc_host_stream_event_data_t;

/**
 * @brief UVC stream format selection.
 */
typedef struct {
    unsigned h_res;                     /*!< Horizontal resolution in pixels. */
    unsigned v_res;                     /*!< Vertical resolution in pixels. */
    float fps;                          /*!< Frames per second. Set to 0 to request the device default. */
    enum uvc_host_stream_format format; /*!< Frame coding format. */
} uvc_host_stream_format_t;

/**
 * @brief Video stream frame.
 *
 * This type is returned from the frame callback when a new frame is received.
 */
typedef struct {
    const uvc_host_stream_format_t vs_format; /*!< Format of this frame buffer. */
    size_t data_buffer_len;                   /*!< Maximum data length supported by this frame buffer. */
    size_t data_len;                          /*!< Data length of the currently stored frame. */
    uint8_t *data;                            /*!< Frame data. */
} uvc_host_frame_t;

/**
 * @brief Stream event callback type.
 *
 * @param[in] event Event structure.
 * @param[in] user_ctx User argument from uvc_host_stream_config_t.
 */
typedef void (*uvc_host_stream_callback_t)(const uvc_host_stream_event_data_t *event, void *user_ctx);

/**
 * @brief Frame callback type.
 *
 * @param[in] frame Received frame.
 * @param[in] user_ctx User argument from uvc_host_stream_config_t.
 * @return
 *      - true if the frame was processed and ownership returns to the driver immediately
 *      - false if the frame is retained by the user and must later be returned with uvc_host_frame_return()
 */
typedef bool (*uvc_host_frame_callback_t)(const uvc_host_frame_t *frame, void *user_ctx);

/**
 * @brief UVC stream configuration structure.
 */
typedef struct {
    uvc_host_stream_callback_t event_cb;  /*!< Stream event callback function. Can be NULL. */
    uvc_host_frame_callback_t frame_cb;   /*!< Stream frame callback function. */
    void *user_ctx;                       /*!< User argument passed to the callbacks. */
    struct {
        uint8_t dev_addr;                 /*!< USB address of device. Set to 0 for any. */
        uint16_t vid;                     /*!< Device's Vendor ID. Set to 0 for any */
        uint16_t pid;                     /*!< Device's Product ID. Set to 0 for any */
        uint8_t uvc_stream_index;         /*!< UVC function index. Set to 0 to use the first available function. */
    } usb;                                /*!< USB device matching criteria. */
    uvc_host_stream_format_t vs_format;   /*!< Video stream format. Resolution, FPS, and encoding. */
    struct {
        int number_of_frame_buffers; /*!< Number of frame buffers. They must be large enough to hold full frames. */
        size_t frame_size;           /*!< Frame buffer size. Use 0 to take dwMaxVideoFrameSize from negotiation. */
        uint32_t frame_heap_caps;    /*!< Memory capabilities for frame buffers passed to heap_caps_malloc(). */
        int number_of_urbs;          /*!< Number of URBs used by this stream. Triple buffering is recommended. */
        size_t urb_size;             /*!< Size in bytes of one URB. Larger values trade memory for fewer interrupts. */
        uint8_t **user_frame_buffers; /*!< Optional user-provided frame buffers. NULL lets the driver allocate them. */
    } advanced;                       /*!< Advanced buffering and transfer settings. */
} uvc_host_stream_config_t;

/**
 * @brief Install the UVC driver.
 *
 * USB Host Library must already be installed with usb_host_install() before
 * calling this function.
 *
 * @param[in] driver_config Driver configuration structure. If NULL, a default configuration is used.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the driver is already installed or USB Host Library is unavailable
 *      - ESP_ERR_NO_MEM if there is not enough memory to install the driver
 */
esp_err_t uvc_host_install(const uvc_host_driver_config_t *driver_config);

/**
 * @brief Uninstall the UVC driver.
 *
 * All open streams must be closed with uvc_host_stream_close() before calling
 * this function.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the driver is not installed or not all streams are closed
 *      - ESP_ERR_NOT_FINISHED if teardown does not complete within the internal timeout
 */
esp_err_t uvc_host_uninstall(void);

/**
 * @brief Handle USB Host events for the UVC driver.
 *
 * If uvc_host_install() is called with `create_background_task = false`, the
 * application must call this function periodically to dispatch USB Host events.
 * Do not call this function when `create_background_task = true`.
 *
 * @param[in] timeout Timeout in FreeRTOS ticks.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the UVC driver is not installed
 *      - ESP_FAIL if event handling finished and the function should no longer be called
 *      - Other error codes returned by usb_host_client_handle_events()
 */
esp_err_t uvc_host_handle_events(unsigned long timeout);

/**
 * @brief Open a UVC-compliant stream.
 *
 * @param[in] stream_config Stream configuration structure.
 * @param[in] timeout Timeout in FreeRTOS ticks.
 * @param[out] stream_hdl_ret UVC stream handle output.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the UVC driver is not installed
 *      - ESP_ERR_INVALID_ARG if stream_config or stream_hdl_ret is NULL, or the frame buffer configuration is invalid
 *      - ESP_ERR_NO_MEM if there is not enough memory for the stream
 *      - ESP_ERR_NOT_FOUND if a stream matching the requested configuration is not found
 *      - Other error codes from the USB Host library
 */
esp_err_t uvc_host_stream_open(const uvc_host_stream_config_t *stream_config,
                               int timeout,
                               uvc_host_stream_hdl_t *stream_hdl_ret);

/**
 * @brief Start a UVC stream.
 *
 * After this call, the frame callback is invoked for each received frame.
 *
 * @param[in] stream_hdl UVC handle obtained from uvc_host_stream_open().
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if stream_hdl is NULL
 *      - ESP_ERR_INVALID_STATE if the stream is already running
 *      - ESP_ERR_NOT_FOUND if format negotiation fails
 *      - Other error codes from the USB Host library
 */
esp_err_t uvc_host_stream_start(uvc_host_stream_hdl_t stream_hdl);

/**
 * @brief Change the format of an open stream.
 *
 * @note If the stream is already running, it is stopped, reconfigured, and then started again.
 *
 * @param[in] stream_hdl UVC handle obtained from uvc_host_stream_open().
 * @param[in,out] format Format to configure. It is updated if default FPS or default format is requested.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if stream_hdl or format is NULL
 *      - ESP_ERR_NOT_FOUND if format negotiation fails
 *      - Other error codes from the USB Host library
 */
esp_err_t uvc_host_stream_format_select(uvc_host_stream_hdl_t stream_hdl, uvc_host_stream_format_t *format);

/**
 * @brief Get the format of an open stream.
 *
 * @note No control transfer is sent to the device. The format is taken from the
 *       last successful negotiation.
 *
 * @param[in] stream_hdl UVC handle obtained from uvc_host_stream_open().
 * @param[out] format Pointer to the format structure to fill.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if stream_hdl or format is NULL
 */
esp_err_t uvc_host_stream_format_get(uvc_host_stream_hdl_t stream_hdl, uvc_host_stream_format_t *format);

/**
 * @brief Stop a UVC stream.
 *
 * @param[in] stream_hdl UVC handle obtained from uvc_host_stream_open().
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if stream_hdl is NULL
 *      - Other error codes from the USB Host library
 */
esp_err_t uvc_host_stream_stop(uvc_host_stream_hdl_t stream_hdl);

/**
 * @brief Close a UVC stream and release its resources.
 *
 * @param[in] stream_hdl UVC handle obtained from uvc_host_stream_open().
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the UVC driver is not installed or some frames were not returned
 *      - ESP_ERR_INVALID_ARG if stream_hdl is NULL
 */
esp_err_t uvc_host_stream_close(uvc_host_stream_hdl_t stream_hdl);

/**
 * @brief Return a processed frame back to the driver.
 *
 * Do not call this function if the frame callback returned true.
 * Call this function after the frame is processed if the frame callback
 * returned false.
 *
 * @param[in] stream_hdl UVC handle obtained from uvc_host_stream_open().
 * @param[in] frame Frame obtained from the frame callback.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if frame or stream_hdl is NULL
 *      - ESP_FAIL if the frame could not be returned to the driver
 */
esp_err_t uvc_host_frame_return(uvc_host_stream_hdl_t stream_hdl, uvc_host_frame_t *frame);

/**
 * @brief Print a device's descriptors.
 *
 * The device descriptor and the full configuration descriptor are printed in a
 * human-readable format to stdout.
 *
 * @param[in] stream_hdl UVC handle obtained from uvc_host_stream_open().
 */
void uvc_host_desc_print(uvc_host_stream_hdl_t stream_hdl);

/**
 * @brief Retrieve the list of frame descriptors for a specific streaming interface in a UVC device.
 *
 * This function extracts all frame descriptors associated with the requested
 * UVC streaming interface and stores them in uvc_host_frame_info_t entries.
 *
 * If frame_info_list is NULL, the function returns only the number of available
 * entries through list_size.
 *
 * @param[in] dev_addr USB device address, typically obtained from the driver event callback.
 * @param[in] uvc_stream_index UVC stream index from the driver event callback.
 *                             Use 0 to select the first available function.
 * @param[out] frame_info_list Caller-allocated uvc_host_frame_info_t array,
 *                             or NULL to query the count only.
 * @param[in,out] list_size Input: frame_info_list capacity. Output: valid or
 *                          required entry count.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if list_size is NULL or the device configuration cannot be retrieved
 *      - ESP_ERR_NOT_FOUND if the required UVC descriptors cannot be found
 */
esp_err_t uvc_host_get_frame_list(uint8_t dev_addr,
                                  uint8_t uvc_stream_index,
                                  uvc_host_frame_info_t (*frame_info_list)[],
                                  size_t *list_size);

#ifdef __cplusplus
}
#endif
