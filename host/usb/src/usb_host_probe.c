/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "usb_private.h"
#include "usbh.h"
#include "usb_host_probe.h"
#include "hub.h"
#include "usb/usb_helpers.h"

#define PROBE_CTRL_TRANSFER_MAX_DATA_LEN    CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE

/**
 * @brief Stages of the post-light-sleep device probe state machine
 */
typedef enum {
    PROBE_STAGE_IDLE = 0,                   /**< No device to be probed */
    PROBE_STAGE_GET_DEV_DESC,               /**< Probe the device by getting a device descriptor */
    PROBE_STAGE_CHECK_DEV_DESC,             /**< Probe the device by checking a device descriptor */
    PROBE_STAGE_DONE,                       /**< Probing done */
} probe_stage_t;

static const char *const probe_stage_strings[] = {
    "IDLE",
    "GET_DEV_DESC",
    "CHECK_DEV_DESC",
    "DONE",
};

typedef struct {
    struct {
        probe_stage_t stage;                /**< Current probing stage */
        unsigned int node_uid;              /**< Unique node ID of device being probed */
        usb_device_handle_t dev_hdl;        /**< Handle of device being probed */
        uint8_t ep0_mps;                    /**< Endpoint 0 Max Packet Size */
        int expect_num_bytes;               /**< Expected number of bytes for IN transfers stages. Set to 0 for OUT transfer */
        bool probe_passed;                  /**< Flag indicating, that the probing passed */
    } single_thread;                        /**< Single thread members don't require a critical section as long as they are never accessed from multiple threads */

    struct {
        urb_t *urb;                         /**< URB used for probe control transfers. Max data length of 8 for LS devices, 64 for FS/HS devices */
        usb_proc_req_cb_t proc_req_cb;      /**< USB Host process request callback. Refer to proc_req_callback() in usb_host.c */
        void *proc_req_cb_arg;              /**< USB Host process request callback argument */
        probe_event_cb_t probe_event_cb;    /**< Probe event callback */
        void *probe_event_cb_arg;           /**< Probe event callback argument */
    } constant;                             /**< Constant members. Do not change after installation thus do not require a critical section or mutex */
} probe_driver_t;

static probe_driver_t *p_probe_driver = NULL;

static const char *PROBE_TAG = "PROBE";

// ---------------------------- Helpers ----------------------------------------

#define PROBE_CHECK(cond, ret_val) ({               \
            if (unlikely(!(cond))) {                \
                return (ret_val);                   \
            }                                       \
})

// ------------------------ Private functions ----------------------------------

/**
 * @brief Control transfer completion callback
 *
 * Is called by lower logic when the probe control transfer is completed with or without error
 *
 * @param[in] ctrl_xfer Pointer to a transfer buffer
 */
static void probe_control_transfer_complete(usb_transfer_t *ctrl_xfer)
{
    assert(ctrl_xfer);
    assert(ctrl_xfer->context);
    assert(p_probe_driver->single_thread.dev_hdl == ctrl_xfer->context);

    // Request processing
    p_probe_driver->constant.proc_req_cb(USB_PROC_REQ_SOURCE_PROBE, false, p_probe_driver->constant.proc_req_cb_arg);
}

/**
 * @brief Control request: Get Device descriptor
 *
 * Prepares and submits a GET_DESCRIPTOR(DEVICE) control transfer for the device being probed
 *
 * @param[in] stage Current probe stage (for logging)
 *
 * @return
 *    - ESP_OK: Control transfer submitted successfully
 *    - Other: Error from usbh_dev_submit_ctrl_urb(); stage is set to PROBE_STAGE_DONE
 */
static esp_err_t probe_get_dev_desc(probe_stage_t stage)
{
    usb_transfer_t *transfer = &p_probe_driver->constant.urb->transfer;
    const uint8_t ctrl_ep_mps = p_probe_driver->single_thread.ep0_mps;

    USB_SETUP_PACKET_INIT_GET_DEVICE_DESC((usb_setup_packet_t *)transfer->data_buffer);
    transfer->num_bytes = sizeof(usb_setup_packet_t) + usb_round_up_to_mps(sizeof(usb_device_desc_t), ctrl_ep_mps);
    p_probe_driver->single_thread.expect_num_bytes = sizeof(usb_setup_packet_t) + sizeof(usb_device_desc_t);

    esp_err_t ret = usbh_dev_submit_ctrl_urb(p_probe_driver->single_thread.dev_hdl, p_probe_driver->constant.urb);
    if (ret != ESP_OK) {
        ESP_LOGE(PROBE_TAG, "Control transfer submit error %s at %s", esp_err_to_name(ret), probe_stage_strings[stage]);

        p_probe_driver->single_thread.stage = PROBE_STAGE_DONE;

        // Request processing
        p_probe_driver->constant.proc_req_cb(USB_PROC_REQ_SOURCE_PROBE, false, p_probe_driver->constant.proc_req_cb_arg);
        return ret;
    }

    // Proceed to next stage
    p_probe_driver->single_thread.stage = PROBE_STAGE_CHECK_DEV_DESC;
    return ret;
}

/**
 * @brief Parse Device descriptor control transfer response
 *
 * Validates transfer status, response length, and bDescriptorType of the received device descriptor
 *
 * @return
 *    - ESP_OK: Device descriptor is valid; device is still present at its address
 *    - ESP_ERR_NOT_FINISHED: Transfer did not complete successfully
 *    - ESP_ERR_INVALID_SIZE: Response shorter than expected
 *    - ESP_ERR_INVALID_RESPONSE: bDescriptorType is not USB_B_DESCRIPTOR_TYPE_DEVICE
 */
static esp_err_t probe_parse_xfer_response(void)
{
    usb_transfer_t *ctrl_xfer = &p_probe_driver->constant.urb->transfer;
    esp_err_t ret = ESP_FAIL;

    if (ctrl_xfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGE(PROBE_TAG, "Bad transfer status %d at %s",
                 ctrl_xfer->status, probe_stage_strings[p_probe_driver->single_thread.stage]);
        ret = ESP_ERR_NOT_FINISHED;
        goto probe_failed;
    }

    if (p_probe_driver->single_thread.expect_num_bytes != ctrl_xfer->actual_num_bytes) {
        ESP_LOGW(PROBE_TAG, "Unexpected response length %d (expected %d)",
                 ctrl_xfer->actual_num_bytes, p_probe_driver->single_thread.expect_num_bytes);
        if (ctrl_xfer->actual_num_bytes < p_probe_driver->single_thread.expect_num_bytes) {
            ESP_LOGE(PROBE_TAG, "Invalid response length");
            ret = ESP_ERR_INVALID_SIZE;
            goto probe_failed;
        }
    }

    const usb_device_desc_t *dev_desc = (const usb_device_desc_t *)(ctrl_xfer->data_buffer + sizeof(usb_setup_packet_t));
    if (dev_desc->bDescriptorType != USB_B_DESCRIPTOR_TYPE_DEVICE) {
        ESP_LOGE(PROBE_TAG, "Probe dev desc has wrong bDescriptorType");
        ret = ESP_ERR_INVALID_RESPONSE;
        goto probe_failed;
    }

    // Descriptor check passed, device is in addressed state
    ret = ESP_OK;

probe_failed:
    return ret;
}

/**
 * @brief Control request response handling: Check Device descriptor
 *
 * Parses the GET_DESCRIPTOR(DEVICE) response and advances to PROBE_STAGE_DONE
 */
static void probe_check_dev_desc(void)
{
    if (ESP_OK == probe_parse_xfer_response()) {
        // Device descriptor is valid, device is in addressed state
        p_probe_driver->single_thread.probe_passed = true;
    }

    // Finish probe
    p_probe_driver->single_thread.stage = PROBE_STAGE_DONE;
    // Request processing
    p_probe_driver->constant.proc_req_cb(USB_PROC_REQ_SOURCE_PROBE, false, p_probe_driver->constant.proc_req_cb_arg);
}

/**
 * @brief Complete stage
 *
 * Closes the probed device, resets driver state to idle, and delivers PROBE_EVENT_PASSED or
 * PROBE_EVENT_FAILED via the probe event callback
 */
static void probe_stage_done(void)
{
    const unsigned int node_uid = p_probe_driver->single_thread.node_uid;
    usb_device_handle_t dev_hdl = p_probe_driver->single_thread.dev_hdl;
    const bool probe_passed = p_probe_driver->single_thread.probe_passed;

    if (dev_hdl != NULL) {
        ESP_ERROR_CHECK(usbh_dev_close(dev_hdl));
    }

    // Cleanup
    p_probe_driver->single_thread.node_uid = 0;
    p_probe_driver->single_thread.dev_hdl = NULL;
    p_probe_driver->single_thread.probe_passed = false;
    p_probe_driver->single_thread.expect_num_bytes = 0;
    p_probe_driver->constant.urb->transfer.context = NULL;
    p_probe_driver->single_thread.stage = PROBE_STAGE_IDLE;

    // Deliver event
    probe_event_data_t event_data = {};
    if (probe_passed) {
        ESP_LOGI(PROBE_TAG, "Post-light-sleep probe passed for uid %u", node_uid);

        const probe_event_data_t passed_event_data = {
            .event = PROBE_EVENT_PASSED,
            .passed = {
                .uid = node_uid,
            },
        };
        event_data = passed_event_data;
    } else {
        ESP_LOGW(PROBE_TAG, "Post-light-sleep probe failed for uid %u", node_uid);

        const probe_event_data_t failed_event_data = {
            .event = PROBE_EVENT_FAILED,
            .failed = {
                .uid = node_uid,
            },
        };
        event_data = failed_event_data;
    }

    // Call event callback
    p_probe_driver->constant.probe_event_cb(&event_data, p_probe_driver->constant.probe_event_cb_arg);
}

// -------------------------- Public API ---------------------------------------

esp_err_t probe_install(probe_config_t *config, void **client_ret)
{
    PROBE_CHECK(p_probe_driver == NULL, ESP_ERR_INVALID_STATE);
    PROBE_CHECK(config != NULL && client_ret != NULL, ESP_ERR_INVALID_ARG);

    esp_err_t ret;
    probe_driver_t *probe_drv = heap_caps_calloc(1, sizeof(probe_driver_t), MALLOC_CAP_DEFAULT);
    PROBE_CHECK(probe_drv != NULL, ESP_ERR_NO_MEM);

    urb_t *urb = urb_alloc(sizeof(usb_setup_packet_t) + PROBE_CTRL_TRANSFER_MAX_DATA_LEN, 0);
    if (urb == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto alloc_err;
    }

    // URB setup
    urb->usb_host_client = (void *) probe_drv;   // Client is an address of the probe driver object
    urb->transfer.callback = probe_control_transfer_complete;
    // Driver setup
    probe_drv->constant.urb = urb;
    probe_drv->constant.proc_req_cb = config->proc_req_cb;
    probe_drv->constant.proc_req_cb_arg = config->proc_req_cb_arg;
    probe_drv->constant.probe_event_cb = config->probe_event_cb;
    probe_drv->constant.probe_event_cb_arg = config->probe_event_cb_arg;
    probe_drv->single_thread.stage = PROBE_STAGE_IDLE;

    if (p_probe_driver != NULL) {
        ret = ESP_ERR_INVALID_STATE;
        goto err;
    }

    p_probe_driver = probe_drv;
    *client_ret = (void *)probe_drv;

    ESP_LOGD(PROBE_TAG, "USB Host probe driver installed");
    return ESP_OK;

err:
    urb_free(urb);
alloc_err:
    heap_caps_free(probe_drv);
    return ret;
}

esp_err_t probe_uninstall(void)
{
    PROBE_CHECK(p_probe_driver != NULL, ESP_ERR_INVALID_STATE);
    PROBE_CHECK(p_probe_driver->single_thread.stage == PROBE_STAGE_IDLE, ESP_ERR_NOT_FINISHED);

    probe_driver_t *probe_drv = p_probe_driver;
    p_probe_driver = NULL;
    // Free memory
    urb_free(probe_drv->constant.urb);
    heap_caps_free(probe_drv);
    return ESP_OK;
}

esp_err_t probe_start(unsigned int uid)
{
    PROBE_CHECK(p_probe_driver != NULL, ESP_ERR_INVALID_STATE);
    PROBE_CHECK(p_probe_driver->single_thread.stage == PROBE_STAGE_IDLE, ESP_ERR_NOT_FINISHED);

    // Check if enum lock is taken for this device's uid
    bool enum_locked;
    ESP_RETURN_ON_ERROR(usbh_dev_enum_is_locked_by_uid(uid, &enum_locked), PROBE_TAG, "Failed to get device enum lock");
    PROBE_CHECK(!enum_locked, ESP_ERR_NOT_SUPPORTED);

    usb_device_handle_t dev_hdl;
    esp_err_t ret = usbh_devs_open_uid(uid, &dev_hdl);
    if (ret != ESP_OK) {
        ESP_LOGE(PROBE_TAG, "Failed to open device uid %u: %s", uid, esp_err_to_name(ret));
        return ret;
    }

    usb_device_info_t dev_info;
    ESP_ERROR_CHECK(usbh_dev_get_info(dev_hdl, &dev_info));

    uint8_t ctrl_ep_mps = (dev_info.speed == USB_SPEED_LOW) ? 8 : 64;
    const usb_device_desc_t *dev_desc = NULL;
    // Try to get cached device descriptor of the device
    if (usbh_dev_get_desc(dev_hdl, &dev_desc) == ESP_OK && dev_desc != NULL) {
        ctrl_ep_mps = dev_desc->bMaxPacketSize0;
    }

    ESP_LOGI(PROBE_TAG, "Start post-light-sleep probe for uid %u", uid);

    p_probe_driver->single_thread.stage = PROBE_STAGE_GET_DEV_DESC;
    p_probe_driver->single_thread.node_uid = uid;
    p_probe_driver->single_thread.dev_hdl = dev_hdl;
    p_probe_driver->single_thread.ep0_mps = ctrl_ep_mps;
    p_probe_driver->single_thread.probe_passed = false;
    p_probe_driver->constant.urb->transfer.context = (void *)dev_hdl;

    // Request processing
    p_probe_driver->constant.proc_req_cb(USB_PROC_REQ_SOURCE_PROBE, false, p_probe_driver->constant.proc_req_cb_arg);
    return ESP_OK;
}

esp_err_t probe_cancel(unsigned int uid)
{
    (void)uid;
    PROBE_CHECK(p_probe_driver != NULL, ESP_ERR_INVALID_STATE);

    const probe_stage_t stage = p_probe_driver->single_thread.stage;
    if (stage == PROBE_STAGE_IDLE || stage == PROBE_STAGE_DONE) {
        return ESP_OK;
    }

    ESP_LOGV(PROBE_TAG, "Cancel at %s", probe_stage_strings[stage]);
    p_probe_driver->single_thread.probe_passed = false;
    p_probe_driver->single_thread.stage = PROBE_STAGE_DONE;

    // Request processing
    p_probe_driver->constant.proc_req_cb(USB_PROC_REQ_SOURCE_PROBE, false, p_probe_driver->constant.proc_req_cb_arg);
    return ESP_OK;
}

esp_err_t probe_process(void)
{
    PROBE_CHECK(p_probe_driver != NULL, ESP_ERR_INVALID_STATE);
    const probe_stage_t stage = p_probe_driver->single_thread.stage;

    switch (stage) {
    case PROBE_STAGE_GET_DEV_DESC:
        probe_get_dev_desc(stage);
        break;
    case PROBE_STAGE_CHECK_DEV_DESC:
        probe_check_dev_desc();
        break;
    case PROBE_STAGE_DONE:
        probe_stage_done();
        break;
    default:
        abort();
        break;
    }

    return ESP_OK;
}
