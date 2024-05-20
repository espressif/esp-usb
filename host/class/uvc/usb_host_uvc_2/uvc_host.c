/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/queue.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_system.h"

#include "usb/usb_host.h"
#include "usb/uvc_host.h"
#include "uvc_control.h"
#include "uvc_types_priv.h"
#include "uvc_frame_priv.h"
#include "uvc_descriptors_priv.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

static const char *TAG = "uvc";

// UVC spinlock
static portMUX_TYPE uvc_lock = portMUX_INITIALIZER_UNLOCKED;
#define UVC_ENTER_CRITICAL()   portENTER_CRITICAL(&uvc_lock)
#define UVC_EXIT_CRITICAL()    portEXIT_CRITICAL(&uvc_lock)

// UVC events
#define UVC_TEARDOWN          BIT0
#define UVC_TEARDOWN_COMPLETE BIT1

// Transfer callbacks
static void ctrl_xfer_cb(usb_transfer_t *transfer);
static void in_xfer_cb(usb_transfer_t *transfer);

// UVC driver object
typedef struct {
    usb_host_client_handle_t usb_client_hdl; /*!< USB Host handle reused for all UVC devices in the system */
    SemaphoreHandle_t open_close_mutex;      /*!< Protects list of opened devices from concurrent access */
    EventGroupHandle_t driver_status;        /*!< Holds status of the driver */
    usb_transfer_t *ctrl_transfer;           /*!< CTRL (endpoint 0) transfer */
    SemaphoreHandle_t ctrl_mutex;            /*!< CTRL mutex */
    SLIST_HEAD(list_dev, uvc_host_stream_s) uvc_stream_list;   /*!< List of open streams */
} uvc_obj_t;

static uvc_obj_t *p_uvc_obj = NULL;

/**
 * @brief USB Host Client event callback
 *
 * Handling of USB device connection/disconnection to/from USB device tree.
 *
 * @param[in] event_msg Event message type
 * @param[in] arg Caller's argument (not used in this driver)
 */
static void usb_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        ESP_LOGD(TAG, "New device connected");
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE: {
        ESP_LOGD(TAG, "Device suddenly disconnected");
        // Find UVC pseudo-devices associated with this USB device and close them
        uvc_stream_t *uvc_stream;
        uvc_stream_t *tusb_stream;
        // We are using 'SAFE' version of 'SLIST_FOREACH' which enables user to close the disconnected device in the callback
        SLIST_FOREACH_SAFE(uvc_stream, &p_uvc_obj->uvc_stream_list, list_entry, tusb_stream) {
            if (uvc_stream->dev_hdl == event_msg->dev_gone.dev_hdl && uvc_stream->stream_cb) {
                // The suddenly disconnected device was opened by this driver: inform user about this
                const uvc_host_stream_event_data_t disconn_event = {
                    .type = UVC_HOST_DEVICE_DISCONNECTED,
                    .data.stream_hdl = (uvc_host_stream_hdl_t) uvc_stream,
                };
                uvc_stream->stream_cb(&disconn_event, uvc_stream->cb_arg);
            }
        }
        break;
    }
    default:
        assert(false);
        break;
    }
}

//@todo revise this section according to MSC and HID drivers
/**
 * @brief UVC driver handling task
 *
 * USB host client registration and deregistration is handled here.
 *
 * @param[in] arg User's argument. Handle of a task that started this task.
 */
static void uvc_client_task(void *arg)
{
    vTaskSuspend(NULL); // Task will be resumed from uvc_host_install()
    uvc_obj_t *uvc_obj = p_uvc_obj; // Make local copy of the driver's handle
    assert(uvc_obj->usb_client_hdl);

    // Start handling client's events
    while (1) {
        usb_host_client_handle_events(uvc_obj->usb_client_hdl, portMAX_DELAY);
        EventBits_t events = xEventGroupGetBits(uvc_obj->driver_status);
        if (events & UVC_TEARDOWN) {
            break;
        }
    }

    ESP_LOGD(TAG, "Deregistering client");
    ESP_ERROR_CHECK(usb_host_client_deregister(uvc_obj->usb_client_hdl));
    xEventGroupSetBits(uvc_obj->driver_status, UVC_TEARDOWN_COMPLETE);
    vTaskDelete(NULL);
}

/**
 * @brief Free USB transfers used by this device
 *
 * @note There can be no transfers in flight, at the moment of calling this function.
 * @param[in] uvc_stream Pointer to UVC stream
 */
static void uvc_transfers_free(uvc_stream_t *uvc_stream)
{
    for (unsigned i = 0; i < uvc_stream->num_of_xfers; i++) {
        usb_host_transfer_free(uvc_stream->xfers[i]);
    }
    free(uvc_stream->xfers);
}

/**
 * @brief Allocate UVC transfers
 *
 * @param[in] uvc_stream       Pointer to UVC stream
 * @param[in] num_of_transfers Number of USB transfers allocated for this stream
 * @param[in] transfer_size    Size of 1 USB transfer
 * @param[in] ep_desc          Descriptor of the streaming endpoint
 * @return
 *     - ESP_OK:              Success
 *     - ESP_ERR_NO_MEM:      Not enough memory for transfers and semaphores allocation
 *     - ESP_ERR_INVALID_ARG: Transfer size or Max packet size are invalid
 */
static esp_err_t uvc_transfers_allocate(uvc_stream_t *uvc_stream, unsigned num_of_transfers, size_t transfer_size, const usb_ep_desc_t *ep_desc)
{
    UVC_CHECK(ep_desc, ESP_ERR_INVALID_ARG);
    esp_err_t ret = ESP_OK;
    uint16_t max_packet_size = USB_EP_DESC_GET_MPS(ep_desc);
    max_packet_size *= (USB_EP_DESC_GET_MULT(ep_desc) + 1); // We multiply MPS by number of transactions in microframe

    UVC_CHECK(max_packet_size <= transfer_size, ESP_ERR_INVALID_ARG);
    UVC_CHECK(max_packet_size > 0, ESP_ERR_INVALID_ARG);

    // Allocate array of transfers
    uvc_stream->num_of_xfers = num_of_transfers;
    uvc_stream->xfers = malloc(num_of_transfers * sizeof(usb_transfer_t *));
    UVC_CHECK(uvc_stream->xfers, ESP_ERR_NO_MEM);

    // Divide the transfer data buffer into ISOC packets
    const unsigned num_isoc_packets = transfer_size / max_packet_size;
    ESP_LOGD(TAG, "Allocating %d USB ISOC transfers, each has %d ISOC packets per %d bytes.", num_of_transfers, num_isoc_packets, max_packet_size);

    for (unsigned i = 0; i < num_of_transfers; i++) {
        ESP_GOTO_ON_ERROR(
            usb_host_transfer_alloc(transfer_size, num_isoc_packets, &uvc_stream->xfers[i]),
            err, TAG, "Could not allocate USB transfers");
        usb_transfer_t *this_transfer = uvc_stream->xfers[i];
        this_transfer->device_handle = uvc_stream->dev_hdl;
        this_transfer->callback = in_xfer_cb;
        this_transfer->context = uvc_stream;
        this_transfer->timeout_ms = 1000;
        this_transfer->num_bytes = num_isoc_packets * max_packet_size;
        this_transfer->bEndpointAddress = ep_desc->bEndpointAddress;
        for (unsigned j = 0; j < num_isoc_packets; j++) {
            this_transfer->isoc_packet_desc[j].num_bytes = max_packet_size;
        }
    }
    return ESP_OK;

err:
    uvc_transfers_free(uvc_stream);
    return ret;
}

/**
 * @brief Helper function that releases resources claimed by UVC device
 *
 * Close underlying USB device, free device driver memory
 *
 * @note All interfaces claimed by this device must be release before calling this function
 * @param uvc_stream UVC stream handle to be removed
 */
static void uvc_device_remove(uvc_stream_t *uvc_stream)
{
    assert(uvc_stream);
    uvc_transfers_free(uvc_stream);
    uvc_frame_free(uvc_stream);
    // We don't check the error code of usb_host_device_close, as the close might fail, if someone else is still using the device (not all interfaces are released)
    usb_host_device_close(p_uvc_obj->usb_client_hdl, uvc_stream->dev_hdl); // Gracefully continue on error
    free(uvc_stream);
}

/**
 * @brief Open USB device with requested VID/PID
 *
 * This function has two regular return paths:
 * 1. USB device with matching VID/PID is already opened by this driver: allocate new UVC device on top of the already opened USB device.
 * 2. USB device with matching VID/PID is NOT opened by this driver yet: poll USB connected devices until it is found.
 *
 * @note This function will block for timeout_ms, if the device is not enumerated at the moment of calling this function.
 * @param[in] vid Vendor ID
 * @param[in] pid Product ID
 * @param[in] timeout Connection timeout in FreeRTOS ticks
 * @param[out] dev UVC device
 * @return esp_err_t
 */
static esp_err_t uvc_find_and_open_usb_device(uint16_t vid, uint16_t pid, TickType_t timeout, uvc_stream_t **dev)
{
    assert(p_uvc_obj);
    assert(dev);

    *dev = calloc(1, sizeof(uvc_stream_t));
    if (*dev == NULL) {
        return ESP_ERR_NO_MEM;
    }

    // First, check list of already opened UVC devices
    ESP_LOGD(TAG, "Checking list of opened USB devices");
    uvc_stream_t *uvc_stream;
    SLIST_FOREACH(uvc_stream, &p_uvc_obj->uvc_stream_list, list_entry) {
        const usb_device_desc_t *device_desc;
        ESP_ERROR_CHECK(usb_host_get_device_descriptor(uvc_stream->dev_hdl, &device_desc));
        if (device_desc->idVendor == vid && device_desc->idProduct == pid) {
            // Return path 1:
            (*dev)->dev_hdl = uvc_stream->dev_hdl;
            return ESP_OK;
        }
    }

    // Second, poll connected devices until new device is connected or timeout
    TickType_t timeout_ticks = timeout;
    TimeOut_t connection_timeout;
    vTaskSetTimeOutState(&connection_timeout);

    ESP_LOGD(TAG, "Checking list of connected USB devices");
    do {
        uint8_t dev_addr_list[10];
        int num_of_devices;
        ESP_ERROR_CHECK(usb_host_device_addr_list_fill(sizeof(dev_addr_list), dev_addr_list, &num_of_devices));

        // Go through device address list and find the one we are looking for
        for (int i = 0; i < num_of_devices; i++) {
            usb_device_handle_t current_device;
            // Open USB device
            if (usb_host_device_open(p_uvc_obj->usb_client_hdl, dev_addr_list[i], &current_device) != ESP_OK) {
                continue; // In case we failed to open this device, continue with next one in the list
            }
            assert(current_device);
            const usb_device_desc_t *device_desc;
            ESP_ERROR_CHECK(usb_host_get_device_descriptor(current_device, &device_desc));
            if (device_desc->idVendor == vid && device_desc->idProduct == pid) {
                // Return path 2:
                (*dev)->dev_hdl = current_device;
                return ESP_OK;
            }
            usb_host_device_close(p_uvc_obj->usb_client_hdl, current_device);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    } while (xTaskCheckForTimeOut(&connection_timeout, &timeout_ticks) == pdFALSE);

    // Timeout was reached, clean-up
    free(*dev);
    *dev = NULL;
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t uvc_find_streaming_intf(uvc_stream_t *uvc_stream, uint8_t uvc_index, const uvc_host_stream_format_t *vs_format)
{
    UVC_CHECK(uvc_stream && vs_format, ESP_ERR_INVALID_ARG);

    // Find UVC USB function with desired index
    const uvc_vc_header_desc_t *vc_header_desc = uvc_desc_get_control_interface_header(uvc_stream, uvc_index);
    ESP_RETURN_ON_FALSE(vc_header_desc, ESP_ERR_NOT_FOUND, TAG, "Could not find UVC function with index %d", uvc_index);
    uvc_stream->bcdUVC = vc_header_desc->bcdUVC;

    // Find video streaming interface that offers the requested format
    bool format_found = false;
    for (int streaming_if = 0; streaming_if < vc_header_desc->bInCollection; streaming_if++) {
        uint8_t current_bInterfaceNumber = vc_header_desc->baInterfaceNr[streaming_if];
        if (uvc_desc_is_format_supported(uvc_stream, current_bInterfaceNumber, vs_format)) {
            uvc_stream->bInterfaceNumber = current_bInterfaceNumber;
            format_found = true;
            break;
        }
    }
    ESP_RETURN_ON_FALSE(format_found, ESP_ERR_NOT_FOUND, TAG, "Could not find frame format %dx%d@%dFPS", vs_format->h_res, vs_format->v_res, vs_format->fps);
    return ESP_OK;
}

/**
 * @brief
 *
 * @param[in]  uvc_stream       UVC stream handle
 * @param[out] ep_desc_ret      Pointer of associated streaming endpoint
 * @return esp_err_t
 */
static esp_err_t uvc_claim_interface(uvc_stream_t *uvc_stream, const usb_ep_desc_t **ep_desc_ret)
{
    UVC_CHECK(p_uvc_obj && uvc_stream, ESP_ERR_INVALID_STATE);

    const usb_intf_desc_t *intf_desc;
    const usb_ep_desc_t *ep_desc;
    ESP_RETURN_ON_ERROR (
        uvc_desc_get_streaming_intf_and_ep(uvc_stream, uvc_stream->bInterfaceNumber, &intf_desc, &ep_desc),
        TAG, "Could not find Streaming interface %d", uvc_stream->bInterfaceNumber);

    // Save all required parameters
    uvc_stream->bAlternateSetting = intf_desc->bAlternateSetting;
    *ep_desc_ret = ep_desc;

    return usb_host_interface_claim(p_uvc_obj->usb_client_hdl, uvc_stream->dev_hdl, intf_desc->bInterfaceNumber, intf_desc->bAlternateSetting);
}

esp_err_t uvc_host_install(const uvc_host_driver_config_t *driver_config)
{
    UVC_CHECK(!p_uvc_obj, ESP_ERR_INVALID_STATE);
    UVC_CHECK(driver_config, ESP_ERR_INVALID_ARG);

    // Allocate all we need for this driver
    esp_err_t ret;
    uvc_obj_t *uvc_obj = heap_caps_calloc(1, sizeof(uvc_obj_t), MALLOC_CAP_DEFAULT);
    EventGroupHandle_t driver_status = xEventGroupCreate();
    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    SemaphoreHandle_t ctrl_mutex = xSemaphoreCreateMutex();
    SemaphoreHandle_t ctrl_sem = xSemaphoreCreateBinary();
    usb_transfer_t *ctrl_xfer = NULL;
    usb_host_transfer_alloc(64, 0, &ctrl_xfer); // Worst case HS MPS
    TaskHandle_t driver_task_h = NULL;
    xTaskCreatePinnedToCore(
        uvc_client_task, "USB-UVC", driver_config->driver_task_stack_size, NULL,
        driver_config->driver_task_priority, &driver_task_h, driver_config->xCoreID);

    if (uvc_obj == NULL || driver_task_h == NULL || driver_status == NULL ||
            mutex == NULL || ctrl_mutex == NULL || ctrl_xfer == NULL || ctrl_sem == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto err;
    }

    // Register USB Host client
    usb_host_client_handle_t usb_client = NULL;
    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 3,
        .async.client_event_callback = usb_event_cb,
        .async.callback_arg = NULL
    };
    ESP_GOTO_ON_ERROR(usb_host_client_register(&client_config, &usb_client), err, TAG, "Failed to register USB host client");

    // Initialize UVC driver structure
    SLIST_INIT(&(uvc_obj->uvc_stream_list));
    uvc_obj->driver_status = driver_status;
    uvc_obj->open_close_mutex = mutex;
    uvc_obj->usb_client_hdl = usb_client;
    uvc_obj->ctrl_mutex = ctrl_mutex;
    uvc_obj->ctrl_transfer = ctrl_xfer;
    uvc_obj->ctrl_transfer->context = ctrl_sem;
    uvc_obj->ctrl_transfer->bEndpointAddress = 0;
    uvc_obj->ctrl_transfer->timeout_ms = 5000;
    uvc_obj->ctrl_transfer->callback = ctrl_xfer_cb;

    // Between 1st call of this function and following section, another task might try to install this driver:
    // Make sure that there is only one instance of this driver in the system
    UVC_ENTER_CRITICAL();
    if (p_uvc_obj) {
        // Already created
        ret = ESP_ERR_INVALID_STATE;
        UVC_EXIT_CRITICAL();
        goto client_err;
    } else {
        p_uvc_obj = uvc_obj;
    }
    UVC_EXIT_CRITICAL();

    // Everything OK: Start UVC-Driver task and return
    vTaskResume(driver_task_h);
    return ESP_OK;

client_err:
    usb_host_client_deregister(usb_client);
err: // Clean-up
    free(uvc_obj);
    if (driver_status) {
        vEventGroupDelete(driver_status);
    }
    if (driver_task_h) {
        vTaskDelete(driver_task_h);
    }
    if (mutex) {
        vSemaphoreDelete(mutex);
    }
    if (ctrl_mutex) {
        vSemaphoreDelete(ctrl_mutex);
    }
    if (ctrl_xfer) {
        usb_host_transfer_free(ctrl_xfer);
    }
    if (ctrl_sem) {
        vSemaphoreDelete(ctrl_sem);
    }
    return ret;
}

esp_err_t uvc_host_uninstall()
{
    esp_err_t ret;

    UVC_ENTER_CRITICAL();
    UVC_CHECK_FROM_CRIT(p_uvc_obj, ESP_ERR_INVALID_STATE);
    uvc_obj_t *uvc_obj = p_uvc_obj; // Save Driver's handle to temporary handle
    UVC_EXIT_CRITICAL();

    xSemaphoreTake(p_uvc_obj->open_close_mutex, portMAX_DELAY); // Wait for all open/close calls to finish
    xSemaphoreTake(p_uvc_obj->ctrl_mutex, portMAX_DELAY); // Wait for all CTRL transfers to finish

    UVC_ENTER_CRITICAL();
    if (SLIST_EMPTY(&p_uvc_obj->uvc_stream_list)) { // Check that device list is empty (all devices closed)
        p_uvc_obj = NULL; // NULL static driver pointer: No open/close calls form this point
    } else {
        ret = ESP_ERR_INVALID_STATE;
        UVC_EXIT_CRITICAL();
        goto unblock;
    }
    UVC_EXIT_CRITICAL();

    // Signal to UVC task to stop, unblock it and wait for its deletion
    xEventGroupSetBits(uvc_obj->driver_status, UVC_TEARDOWN);
    usb_host_client_unblock(uvc_obj->usb_client_hdl);
    ESP_GOTO_ON_FALSE(
        xEventGroupWaitBits(uvc_obj->driver_status, UVC_TEARDOWN_COMPLETE, pdFALSE, pdFALSE, pdMS_TO_TICKS(100)),
        ESP_ERR_NOT_FINISHED, unblock, TAG,);

    // Free remaining resources and return
    vEventGroupDelete(uvc_obj->driver_status);
    xSemaphoreGive(uvc_obj->open_close_mutex);
    vSemaphoreDelete(uvc_obj->open_close_mutex);
    xSemaphoreGive(uvc_obj->ctrl_mutex);
    vSemaphoreDelete(uvc_obj->ctrl_mutex);
    vSemaphoreDelete(uvc_obj->ctrl_transfer->context);
    usb_host_transfer_free(uvc_obj->ctrl_transfer);
    free(uvc_obj);
    return ESP_OK;

unblock:
    xSemaphoreGive(uvc_obj->open_close_mutex);
    xSemaphoreGive(uvc_obj->ctrl_mutex);
    return ret;
}

esp_err_t uvc_host_stream_open(const uvc_host_stream_config_t *stream_config, int timeout, uvc_host_stream_hdl_t *stream_hdl_ret)
{
    esp_err_t ret;
    UVC_CHECK(p_uvc_obj, ESP_ERR_INVALID_STATE);
    UVC_CHECK(stream_config, ESP_ERR_INVALID_ARG);
    UVC_CHECK(stream_hdl_ret, ESP_ERR_INVALID_ARG);

    xSemaphoreTake(p_uvc_obj->open_close_mutex, portMAX_DELAY);
    // Find underlying USB device
    uvc_stream_t *uvc_stream;
    ESP_GOTO_ON_ERROR(
        uvc_find_and_open_usb_device(stream_config->usb.vid, stream_config->usb.pid, timeout, &uvc_stream),
        not_found, TAG, "USB device with VID: 0x%04X, PID: 0x%04X not found", stream_config->usb.vid, stream_config->usb.pid);

    // Find the streaming interface
    ESP_GOTO_ON_ERROR(
        uvc_find_streaming_intf(uvc_stream, stream_config->usb.uvc_function_index, &stream_config->vs_format),
        err, TAG, "Could not find streaming interface");

    // Negotiate the frame format
    uvc_vs_ctrl_t vs_result;
    ESP_GOTO_ON_ERROR(
        uvc_host_stream_control_negotiate(uvc_stream, &stream_config->vs_format, &vs_result),
        err, TAG, "Failed to negotiate requested Video Stream format");

    // Claim Video Streaming interface
    const usb_ep_desc_t *ep_desc;
    ESP_GOTO_ON_ERROR(
        uvc_claim_interface(uvc_stream, &ep_desc),
        claim_err, TAG, "Could not claim Streaming interface");
    ESP_LOGD(TAG, "Claimed interface index %d with MPS %d", uvc_stream->bInterfaceNumber, USB_EP_DESC_GET_MPS(ep_desc));

    // Allocate USB transfers
    ESP_GOTO_ON_ERROR(
        uvc_transfers_allocate(uvc_stream, stream_config->advanced.number_of_urbs, stream_config->advanced.urb_size, ep_desc),
        err, TAG,);

    // Allocate Frame buffers
    size_t frame_buffer_size;
    if (stream_config->advanced.frame_size != 0) {
        frame_buffer_size = stream_config->advanced.frame_size; // If user provided custom frame size, use it
    } else {
        frame_buffer_size = vs_result.dwMaxVideoFrameSize; // Use value from frame format negotiation
    };

    ESP_GOTO_ON_ERROR(
        uvc_frame_allocate(uvc_stream, stream_config->advanced.number_of_frame_buffers, frame_buffer_size),
        err, TAG,);

    // Save info
    memcpy((uvc_host_stream_format_t *)&uvc_stream->vs_format, &stream_config->vs_format, sizeof(uvc_host_stream_format_t));
    uvc_stream->stream_cb = stream_config->event_cb;
    uvc_stream->frame_cb = stream_config->frame_cb;
    uvc_stream->cb_arg = stream_config->user_arg;

    // Everything OK, add the device into list
    UVC_ENTER_CRITICAL();
    SLIST_INSERT_HEAD(&p_uvc_obj->uvc_stream_list, uvc_stream, list_entry);
    UVC_EXIT_CRITICAL();
    *stream_hdl_ret = (uvc_host_stream_hdl_t)uvc_stream;
    xSemaphoreGive(p_uvc_obj->open_close_mutex);
    return ESP_OK;

err:
    usb_host_interface_release(p_uvc_obj->usb_client_hdl, uvc_stream->dev_hdl, uvc_stream->bInterfaceNumber);
claim_err:
    uvc_device_remove(uvc_stream);
not_found:
    xSemaphoreGive(p_uvc_obj->open_close_mutex);
    *stream_hdl_ret = NULL;
    return ret;
}

esp_err_t uvc_host_stream_close(uvc_host_stream_hdl_t stream_hdl)
{
    UVC_CHECK(p_uvc_obj, ESP_ERR_INVALID_STATE);
    UVC_CHECK(stream_hdl, ESP_ERR_INVALID_ARG);

    esp_err_t ret = ESP_OK;
    xSemaphoreTake(p_uvc_obj->open_close_mutex, portMAX_DELAY);

    // Make sure that the device is in the devices list (that it is not already closed)
    uvc_stream_t *uvc_stream;
    bool device_found = false;
    UVC_ENTER_CRITICAL();
    SLIST_FOREACH(uvc_stream, &p_uvc_obj->uvc_stream_list, list_entry) {
        if (uvc_stream == (uvc_stream_t *)stream_hdl) {
            device_found = true;
            break;
        }
    }

    // Device was not found in the uvc_stream_list; it was already closed, return OK
    if (!device_found) {
        goto exit_critical;
    }

    if (uvc_stream->streaming) {
        ret = ESP_ERR_INVALID_STATE;
        goto exit_critical;
    }
    uvc_stream->stream_cb = NULL; // No user callbacks from this point
    uvc_stream->frame_cb = NULL;
    UVC_EXIT_CRITICAL();

    if (!uvc_frame_are_all_returned(uvc_stream)) {
        ret = ESP_ERR_INVALID_STATE;
        goto exit;
    }

    // Release all interfaces
    ESP_ERROR_CHECK(usb_host_interface_release(p_uvc_obj->usb_client_hdl, uvc_stream->dev_hdl, uvc_stream->bInterfaceNumber));

    UVC_ENTER_CRITICAL();
    SLIST_REMOVE(&p_uvc_obj->uvc_stream_list, uvc_stream, uvc_host_stream_s, list_entry);
    UVC_EXIT_CRITICAL();

    uvc_device_remove(uvc_stream);

    UVC_ENTER_CRITICAL();
exit_critical:
    UVC_EXIT_CRITICAL();
exit:
    xSemaphoreGive(p_uvc_obj->open_close_mutex);
    return ret;
}

static esp_err_t uvc_set_interface(uvc_host_stream_hdl_t stream_hdl, bool stream_on)
{
    uvc_stream_t *uvc_stream = (uvc_stream_t *)stream_hdl;
    return uvc_host_usb_ctrl(
               stream_hdl,
               USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_STANDARD | USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
               USB_B_REQUEST_SET_INTERFACE,
               stream_on ? uvc_stream->bAlternateSetting : 0,
               uvc_stream->bInterfaceNumber,
               0,
               NULL);
}

esp_err_t uvc_host_stream_start(uvc_host_stream_hdl_t stream_hdl)
{
    UVC_CHECK(stream_hdl, ESP_ERR_INVALID_ARG);
    UVC_CHECK(stream_hdl->streaming == false, ESP_ERR_INVALID_STATE);

    esp_err_t ret = ESP_OK;
    uvc_stream_t *uvc_stream = (uvc_stream_t *)stream_hdl;

    // 1. Negotiate the frame format
    // @see USB UVC specification ver 1.5, figure 4-1
    uvc_vs_ctrl_t vs_result;
    ESP_RETURN_ON_ERROR(
        uvc_host_stream_control_negotiate(uvc_stream, &uvc_stream->vs_format, &vs_result),
        TAG, "Failed to negotiate requested Video Stream format");
    vTaskDelay(pdMS_TO_TICKS(10)); // Some cameras need delay between format Commit and SetInterface

    // 2. Send command to the camera to start streaming
    ESP_RETURN_ON_ERROR(
        uvc_set_interface(stream_hdl, true),
        TAG, "Could not Set Interface %d-%d", uvc_stream->bInterfaceNumber, uvc_stream->bAlternateSetting);

    // 3. Submit all URBs
    UVC_ENTER_CRITICAL();
    uvc_stream->streaming = true;
    UVC_EXIT_CRITICAL();
    for (int i = 0; i < uvc_stream->num_of_xfers; i++) {
        ESP_GOTO_ON_ERROR(
            usb_host_transfer_submit(uvc_stream->xfers[i]),
            stop_stream, TAG, "Could not submit ISOC transfer %d", i);
    }
    return ESP_OK;

stop_stream:
    UVC_ENTER_CRITICAL();
    uvc_stream->streaming = false;
    UVC_EXIT_CRITICAL();
    uvc_host_stream_stop(stream_hdl);
    return ret;
}

esp_err_t uvc_host_stream_stop(uvc_host_stream_hdl_t stream_hdl)
{
    UVC_CHECK(stream_hdl, ESP_ERR_INVALID_ARG);

    // We do not cancel the ongoing transfers here, it is not supported by USB Host Library
    // By setting uvc_stream->streaming = false; no frame callbacks will be called and the transfer can gracefully finish
    uvc_stream_t *uvc_stream = (uvc_stream_t *)stream_hdl;
    uvc_frame_t *current_frame;
    //@todo halt the pipe here?
    UVC_ENTER_CRITICAL();
    uvc_stream->streaming = false;
    current_frame = uvc_stream->current_frame;
    uvc_stream->current_frame = NULL;
    UVC_EXIT_CRITICAL();
    if (current_frame) {
        uvc_host_frame_return(uvc_stream, current_frame);
    }
    //@todo this is not a clean solution
    vTaskDelay(pdMS_TO_TICKS(10)); // Wait for all transfers to finish
    return uvc_set_interface(stream_hdl, false);
}

static void in_xfer_cb(usb_transfer_t *transfer)
{
    ESP_LOGD(TAG, "in xfer cb");
    uvc_stream_t *uvc_stream = (uvc_stream_t *)transfer->context;

    UVC_ENTER_CRITICAL();
    bool streaming_on = uvc_stream->streaming;
    UVC_EXIT_CRITICAL();
    if (!streaming_on) {
        return; // If the streaming was turned off, we don't have to do anything
    }

    const uint8_t *payload = transfer->data_buffer;
    for (int i = 0; i < transfer->num_isoc_packets; i++) {
        usb_isoc_packet_desc_t *isoc_desc = &transfer->isoc_packet_desc[i];

        // Check USB status
        switch (isoc_desc->status) {
        case USB_TRANSFER_STATUS_COMPLETED:
            break;
        case USB_TRANSFER_STATUS_NO_DEVICE:
        case USB_TRANSFER_STATUS_CANCELED:
            return; // No need to process the rest
        case USB_TRANSFER_STATUS_ERROR:
        case USB_TRANSFER_STATUS_OVERFLOW:
        case USB_TRANSFER_STATUS_STALL:
            ESP_LOGW(TAG, "usb err %d", isoc_desc->status);
            uvc_stream->skip_current_frame = true;
            goto next_isoc_packet; // Data corrupted

        case USB_TRANSFER_STATUS_TIMED_OUT:
        case USB_TRANSFER_STATUS_SKIPPED:
            goto next_isoc_packet; // Skipped and timed out ISOC transfers are not an issue
        default:
            assert(false);
        }

        // Check for Zero Length Packet
        if (isoc_desc->actual_num_bytes == 0) {
            goto next_isoc_packet;
        }

        // Check for start of new frame
        const uvc_payload_header_t *payload_header = (const uvc_payload_header_t *)payload;
        const bool start_of_frame = (uvc_stream->current_frame_id != payload_header->bmHeaderInfo.frame_id);
        if (start_of_frame) {
            // We detected start of new frame. Update Frame ID and start fetching this frame
            uvc_stream->current_frame_id = payload_header->bmHeaderInfo.frame_id;
            uvc_stream->skip_current_frame = false;

            // Get free frame buffer for this new frame
            UVC_ENTER_CRITICAL();
            bool need_new_frame = (uvc_stream->streaming && !uvc_stream->current_frame);
            if (need_new_frame) {
                UVC_EXIT_CRITICAL();
                uvc_stream->current_frame = uvc_frame_get_empty(uvc_stream);
                if (uvc_stream->current_frame == NULL) {
                    // There is no free frame buffer now, skipping this frame
                    uvc_stream->skip_current_frame = true;
                    goto next_isoc_packet;
                }
            } else {
                // We received SoF but current_frame is not NULL: We missed EoF - reset the frame buffer
                uvc_frame_reset(uvc_stream->current_frame);
                UVC_EXIT_CRITICAL();
            }
        } else if (uvc_stream->skip_current_frame) {
            // Previous packets indicated we must skip this frame
            goto next_isoc_packet;
        }

        // Check for empty data
        if (isoc_desc->actual_num_bytes <= payload_header->bHeaderLength) {
            goto next_isoc_packet;
        }

        // Check for error flag
        if (payload_header->bmHeaderInfo.error) {
            uvc_stream->skip_current_frame = true;
            goto next_isoc_packet;
        }

        // Add received data to frame buffer
        const uint8_t *payload_data = payload + payload_header->bHeaderLength;
        const size_t payload_data_len = isoc_desc->actual_num_bytes - payload_header->bHeaderLength;
        esp_err_t ret = uvc_frame_add_data(uvc_stream->current_frame, payload_data, payload_data_len);
        if (ret != ESP_OK) {
            uvc_stream->skip_current_frame = true;
            goto next_isoc_packet;
        }

        // End of Frame. Pass the frame to user
        if (payload_header->bmHeaderInfo.end_of_frame) {
            bool return_frame = true; // In case streaming is stopped ATM, we must return the frame

            // Check if the user did not stop the stream in the meantime
            UVC_ENTER_CRITICAL();
            uvc_frame_t *this_frame = uvc_stream->current_frame;
            uvc_stream->current_frame = NULL; // Stop writing more data to this frame
            const bool invoke_fb_callback = (uvc_stream->streaming && uvc_stream->frame_cb && this_frame);
            uvc_host_frame_callback_t fb_callback = uvc_stream->frame_cb;
            UVC_EXIT_CRITICAL();
            if (invoke_fb_callback) {
                memcpy((uvc_host_stream_format_t *)&this_frame->vs_format, &uvc_stream->vs_format, sizeof(uvc_host_stream_format_t));
                return_frame = fb_callback(this_frame, uvc_stream->cb_arg);
            }
            if (return_frame) {
                // The user has processed the frame in his callback, return it back to empty queue
                uvc_host_frame_return(uvc_stream, this_frame);
            }
        }
next_isoc_packet:
        payload += isoc_desc->num_bytes;
        continue;
    }

    UVC_ENTER_CRITICAL();
    streaming_on = uvc_stream->streaming;
    UVC_EXIT_CRITICAL();
    if (streaming_on) {
        usb_host_transfer_submit(transfer); // Restart the transfer
    }
}

static void ctrl_xfer_cb(usb_transfer_t *transfer)
{
    ESP_LOGD(TAG, "ctrl xfer cb");
    assert(transfer->context);
    xSemaphoreGive((SemaphoreHandle_t)transfer->context);
}

esp_err_t uvc_host_usb_ctrl(uvc_host_stream_hdl_t stream_hdl, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex, uint16_t wLength, uint8_t *data)
{
    UVC_CHECK(stream_hdl, ESP_ERR_INVALID_ARG);
    uvc_stream_t *uvc_stream = (uvc_stream_t *)stream_hdl;
    if (wLength > 0) {
        UVC_CHECK(data, ESP_ERR_INVALID_ARG);
    }
    UVC_CHECK(p_uvc_obj->ctrl_transfer->data_buffer_size >= wLength, ESP_ERR_INVALID_SIZE);

    esp_err_t ret;

    // Take Mutex and fill the CTRL request
    BaseType_t taken = xSemaphoreTake(p_uvc_obj->ctrl_mutex, pdMS_TO_TICKS(5000));
    if (!taken) {
        return ESP_ERR_TIMEOUT;
    }
    usb_setup_packet_t *req = (usb_setup_packet_t *)(p_uvc_obj->ctrl_transfer->data_buffer);
    uint8_t *start_of_data = (uint8_t *)req + sizeof(usb_setup_packet_t);
    req->bmRequestType = bmRequestType;
    req->bRequest = bRequest;
    req->wValue = wValue;
    req->wIndex = wIndex;
    req->wLength = wLength;

    // Bind the transfer and the device
    p_uvc_obj->ctrl_transfer->device_handle = uvc_stream->dev_hdl;
    p_uvc_obj->ctrl_transfer->num_bytes = wLength + sizeof(usb_setup_packet_t);

    // For IN transfers we must transfer data ownership to the driver
    const bool in_transfer = bmRequestType & USB_BM_REQUEST_TYPE_DIR_IN;
    if (!in_transfer) {
        memcpy(start_of_data, data, wLength);
    }

    ESP_GOTO_ON_ERROR(
        usb_host_transfer_submit_control(p_uvc_obj->usb_client_hdl, p_uvc_obj->ctrl_transfer),
        reset_ep0, TAG, "CTRL transfer failed");

    taken = xSemaphoreTake((SemaphoreHandle_t)p_uvc_obj->ctrl_transfer->context, pdMS_TO_TICKS(5000)); // This is a fixed timeout. Every device should be able to respond to CTRL transfer in 5 seconds
    ESP_GOTO_ON_FALSE(taken, ESP_ERR_TIMEOUT, reset_ep0, TAG, "CTRL timeout");
    ESP_GOTO_ON_FALSE(p_uvc_obj->ctrl_transfer->status == USB_TRANSFER_STATUS_COMPLETED, ESP_ERR_INVALID_RESPONSE, unblock, TAG, "Control transfer error");
    ESP_GOTO_ON_FALSE(p_uvc_obj->ctrl_transfer->actual_num_bytes == p_uvc_obj->ctrl_transfer->num_bytes, ESP_ERR_INVALID_RESPONSE, unblock, TAG, "Incorrect number of bytes transferred");

    // For OUT transfers, we must transfer data ownership to user
    if (in_transfer) {
        memcpy(data, start_of_data, wLength);
    }
    ret = ESP_OK;

reset_ep0:
    // Transfer was not finished, error in USB LIB. Reset the endpoint 0
    usb_host_endpoint_halt(uvc_stream->dev_hdl, 0);
    usb_host_endpoint_flush(uvc_stream->dev_hdl, 0);
    usb_host_endpoint_clear(uvc_stream->dev_hdl, 0);

unblock:
    xSemaphoreGive(p_uvc_obj->ctrl_mutex);
    return ret;
}
