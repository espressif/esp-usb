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
    UVC_VS_FORMAT_NV12,        /*!< NV12 stream format. */
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
        size_t urb_size;             /*!< Size in bytes of one URB. Larger values trade memory for fewer interrupts. Set to 0 to use the default size, which is 4x MPS */
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
 * @brief UVC class-specific request codes.
 *
 * @see USB UVC specification ver 1.5, table A.8
 */
typedef enum {
    UVC_HOST_REQ_SET_CUR  = 0x01, /*!< Set the current value of a control. */
    UVC_HOST_REQ_GET_CUR  = 0x81, /*!< Get the current value of a control. */
    UVC_HOST_REQ_GET_MIN  = 0x82, /*!< Get the minimum value of a control. */
    UVC_HOST_REQ_GET_MAX  = 0x83, /*!< Get the maximum value of a control. */
    UVC_HOST_REQ_GET_RES  = 0x84, /*!< Get the resolution (step size) of a control. */
    UVC_HOST_REQ_GET_LEN  = 0x85, /*!< Get the byte length of a control. */
    UVC_HOST_REQ_GET_INFO = 0x86, /*!< Get the capabilities of a control. */
    UVC_HOST_REQ_GET_DEF  = 0x87, /*!< Get the default value of a control. */
} uvc_host_req_code_t;

/**
 * @brief Send a custom control request to the camera.
 *
 * Sends a control transfer as described in chapter 9 of the USB specification. Intended for
 * vendor-specific requests that this driver does not implement itself. For a VideoControl
 * unit or terminal, prefer uvc_host_stream_unit_ctrl(): it composes wValue and wIndex for
 * you, which is the part that is easy to get wrong.
 *
 * @param[in]    stream_hdl    UVC handle obtained from uvc_host_stream_open().
 * @param[in]    bmRequestType Field of the USB control request.
 * @param[in]    bRequest      Field of the USB control request.
 * @param[in]    wValue        Field of the USB control request.
 * @param[in]    wIndex        Field of the USB control request.
 * @param[in]    wLength       Field of the USB control request.
 * @param[inout] data          Payload buffer, at least wLength bytes.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if stream_hdl is NULL, or data is NULL with a non-zero wLength
 *      - ESP_ERR_INVALID_SIZE if the transfer is larger than the driver's control buffer
 *      - ESP_ERR_TIMEOUT if the camera did not answer
 *      - ESP_ERR_NOT_SUPPORTED if the camera stalled the request, meaning it does not
 *        implement this control. A permanent answer: do not keep asking.
 *      - ESP_ERR_INVALID_RESPONSE if the transfer failed on the bus or replied short. A
 *        transient answer, already retried a few times, and worth trying again later.
 */
esp_err_t uvc_host_stream_send_custom_request(uvc_host_stream_hdl_t stream_hdl, uint8_t bmRequestType,
                                              uint8_t bRequest, uint16_t wValue, uint16_t wIndex, uint16_t wLength, uint8_t *data);

/**
 * @brief Find an Extension Unit by its GUID.
 *
 * Unit IDs are assigned per camera and a camera may expose several extension units, so the
 * GUID is the only portable way to address one.
 *
 * @param[in]  stream_hdl UVC handle obtained from uvc_host_stream_open().
 * @param[in]  guid       16-byte GUID, little-endian as it appears in the descriptor.
 * @param[out] unit_id    Unit ID to pass to uvc_host_stream_unit_ctrl().
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if an argument is NULL
 *      - ESP_ERR_NOT_FOUND if this camera has no extension unit with that GUID
 */
esp_err_t uvc_host_stream_find_extension_unit(uvc_host_stream_hdl_t stream_hdl, const uint8_t guid[16],
                                              uint8_t *unit_id);

/**
 * @brief Standard UVC terminal types.
 *
 * Values for the wTerminalType field, for use with uvc_host_stream_find_terminal(). The
 * standard set is listed here; vendor-specific terminals use values outside it, so that
 * function takes a plain uint16_t rather than this enum.
 *
 * @see USB UVC Terminal Types specification ver 1.5, tables 2-1 to 2-3
 */
enum uvc_host_terminal_type {
    UVC_HOST_TT_VENDOR_SPECIFIC          = 0x0100, /*!< USB vendor-specific terminal. */
    UVC_HOST_TT_STREAMING                = 0x0101, /*!< USB streaming terminal. */
    UVC_HOST_ITT_VENDOR_SPECIFIC         = 0x0200, /*!< Vendor-specific input terminal. */
    UVC_HOST_ITT_CAMERA                  = 0x0201, /*!< Camera sensor input terminal. */
    UVC_HOST_ITT_MEDIA_TRANSPORT_INPUT   = 0x0202, /*!< Media transport input terminal. */
    UVC_HOST_OTT_VENDOR_SPECIFIC         = 0x0300, /*!< Vendor-specific output terminal. */
    UVC_HOST_OTT_DISPLAY                 = 0x0301, /*!< Display output terminal. */
    UVC_HOST_OTT_MEDIA_TRANSPORT_OUTPUT  = 0x0302, /*!< Media transport output terminal. */
};

/**
 * @brief Find a terminal by its type.
 *
 * Input and output terminals are both searched; the standard type values do not overlap, so
 * the type alone identifies which is wanted. Pass UVC_HOST_ITT_CAMERA for the terminal that
 * carries the sensor-side controls: exposure, focus, zoom and Auto-Exposure Priority.
 *
 * @param[in]  stream_hdl    UVC handle obtained from uvc_host_stream_open().
 * @param[in]  terminal_type wTerminalType to look for, e.g. UVC_HOST_ITT_CAMERA.
 * @param[out] terminal_id   Terminal ID to pass to uvc_host_stream_unit_ctrl().
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if an argument is NULL
 *      - ESP_ERR_NOT_FOUND if this camera has no terminal of that type
 */
esp_err_t uvc_host_stream_find_terminal(uvc_host_stream_hdl_t stream_hdl, uint16_t terminal_type,
                                        uint8_t *terminal_id);

/**
 * @brief Ask whether a unit or terminal implements a control.
 *
 * Worth calling before every optional control. A camera that does not implement one answers
 * with a STALL, and the USB host library logs that at ERROR, so an unconditional write puts an
 * error in the log of every boot on cameras that simply lack the control.
 *
 * @param[in]  stream_hdl  UVC handle obtained from uvc_host_stream_open().
 * @param[in]  unit_id     Unit or terminal ID.
 * @param[in]  control_bit Bit position in bmControls, counted from D0 across all bytes. For
 *                         example D2 of the Camera Terminal is Auto-Exposure Priority.
 * @param[out] supported   Whether the camera claims this control.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if an argument is NULL
 *      - ESP_ERR_NOT_FOUND if there is no such unit, or it declares no bmControls
 */
esp_err_t uvc_host_stream_unit_supports_control(uvc_host_stream_hdl_t stream_hdl, uint8_t unit_id,
                                                uint8_t control_bit, bool *supported);

/**
 * @brief Issue a control request to a VideoControl unit or terminal.
 *
 * Addresses the VideoControl interface and composes wValue and wIndex from the selector and
 * unit ID. Use it for Camera Terminal, Processing Unit and Extension Unit controls.
 *
 * @param[in]    stream_hdl UVC handle obtained from uvc_host_stream_open().
 * @param[in]    unit_id    Unit or terminal ID, from uvc_host_stream_find_extension_unit() or
 *                          uvc_host_stream_find_terminal().
 * @param[in]    selector   Control selector, defined by the unit.
 * @param[in]    req        Request code. UVC_HOST_REQ_SET_CUR writes, the rest read.
 * @param[inout] data       Payload buffer, at least len bytes.
 * @param[in]    len        Payload length in bytes.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if stream_hdl or data is NULL
 *      - ESP_ERR_NOT_SUPPORTED if the camera stalled it, i.e. this unit does not implement
 *        this control. Prefer uvc_host_stream_unit_supports_control() to find that out
 *        without provoking a STALL that the USB host library logs at ERROR.
 *      - ESP_ERR_INVALID_RESPONSE if the transfer failed on the bus
 *      - Other error codes from the USB Host library
 */
esp_err_t uvc_host_stream_unit_ctrl(uvc_host_stream_hdl_t stream_hdl, uint8_t unit_id, uint8_t selector,
                                    uvc_host_req_code_t req, void *data, uint16_t len);

/**
 * @brief Issue a raw control request to the VideoStreaming interface.
 *
 * Use this for a vendor-specific VideoStreaming selector, or for a spec control this driver
 * has not implemented yet. If you find yourself reaching for the second case, the control
 * probably wants implementing here instead. Nothing is gated for you: a camera that does not
 * implement the selector answers with a STALL, which the USB host library logs at ERROR.
 *
 * @param[in]    stream_hdl UVC handle obtained from uvc_host_stream_open().
 * @param[in]    selector   VideoStreaming control selector.
 * @param[in]    req        Request code. UVC_HOST_REQ_SET_CUR writes, the rest read.
 * @param[inout] data       Payload buffer, at least len bytes.
 * @param[in]    len        Payload length in bytes.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if stream_hdl or data is NULL
 *      - ESP_ERR_NOT_SUPPORTED if the camera stalled it, i.e. it does not implement this
 *        control
 *      - ESP_ERR_INVALID_RESPONSE if the transfer failed on the bus
 *      - Other error codes from the USB Host library
 */
esp_err_t uvc_host_stream_vs_ctrl(uvc_host_stream_hdl_t stream_hdl, uint8_t selector,
                                  uvc_host_req_code_t req, void *data, uint16_t len);

/**
 * @brief Ask the camera to emit a key frame now.
 *
 * Issues the VideoStreaming Generate Key Frame control. Useful when a viewer joins mid-GOP or
 * after packet loss: without it, recovery is bounded by the camera's own key-frame interval,
 * which is often several seconds.
 *
 * @param[in] stream_hdl UVC handle obtained from uvc_host_stream_open().
 *
 * @return
 *      - ESP_OK if the key frame was requested
 *      - ESP_ERR_INVALID_ARG if stream_hdl is NULL
 *      - ESP_ERR_NOT_SUPPORTED if the camera does not implement the control, either by not
 *        claiming it in its descriptor or by stalling the request. Permanent: stop asking.
 *      - ESP_ERR_INVALID_RESPONSE if the transfer failed on the bus. Transient: try later.
 */
esp_err_t uvc_host_stream_request_key_frame(uvc_host_stream_hdl_t stream_hdl);

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
