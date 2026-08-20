/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/ringbuf.h"
#include "soc/soc_caps.h"
#include "esp_check.h"
#include "esp_idf_version.h"
#include "esp_bit_defs.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "usb/usb_helpers.h"
#include "usb/cdc_host_common.h"
#include "cdc_host_descriptor_parsing.h"

static const char *TAG = "cdc_common";

#define CDC_COMMON_CTRL_DATA_SIZE_DEFAULT 64

#if CONFIG_IDF_TARGET_LINUX
#define CDC_COMMON_CTRL_TIMEOUT_MS       1000
#else
#define CDC_COMMON_CTRL_TIMEOUT_MS       5000
#endif

#define CDC_COMMON_TEARDOWN              BIT0
#define CDC_COMMON_TEARDOWN_COMPLETE     BIT1
#define CDC_COMMON_RINGBUF_TIMEOUT_MS    200
#define CDC_COMMON_TX_TIMEOUT_MS         5000

static portMUX_TYPE cdc_common_lock = portMUX_INITIALIZER_UNLOCKED;
#define CDC_COMMON_ENTER_CRITICAL()      portENTER_CRITICAL(&cdc_common_lock)
#define CDC_COMMON_EXIT_CRITICAL()       portEXIT_CRITICAL(&cdc_common_lock)

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#define USB_EVENT_SUPPORT_REMOVED 1
#else
#define USB_EVENT_SUPPORT_REMOVED 0
#endif

typedef struct cdc_host_common_dev_s {
    usb_device_handle_t dev_hdl;
    uint8_t dev_addr;
    size_t ref_count;
    bool new_dev_pending;
#ifdef CDC_HOST_COMMON_REMOTE_WAKE_SUPPORTED
    bool remote_wakeup_enabled;
#endif
    SLIST_ENTRY(cdc_host_common_dev_s) list_entry;
} cdc_host_common_dev_t;

typedef struct cdc_host_common_dev_event_cb_s {
    cdc_host_common_dev_event_cb_t cb;
    void *user_arg;
    SLIST_ENTRY(cdc_host_common_dev_event_cb_s) list_entry;
} cdc_host_common_dev_event_cb_item_t;

typedef struct cdc_host_common_port_s {
    cdc_host_common_dev_t *dev;
    cdc_host_common_data_cb_t data_cb;
    cdc_host_common_port_event_cb_t event_cb;
    void *user_arg;
    struct {
        usb_transfer_t *xfer;
        const usb_intf_desc_t *intf_desc;
        bool polling;
    } notif;
    struct {
        usb_transfer_t *out_xfer;
        usb_transfer_t *in_xfer;
        SemaphoreHandle_t out_done_sem;
        SemaphoreHandle_t out_mux;
        uint16_t in_mps;
        uint8_t *in_data_buffer_base;
        const usb_intf_desc_t *intf_desc;
        bool in_polling;
        const uint8_t *current_data;
        size_t current_data_len;
        RingbufHandle_t rx_ringbuf;
        RingbufHandle_t tx_ringbuf;
        size_t rx_ringbuf_size;
        size_t tx_ringbuf_size;
        bool tx_inflight;
    } data;
    usb_transfer_t *ctrl_xfer;
    SemaphoreHandle_t ctrl_done_sem;
    SemaphoreHandle_t ctrl_mux;
    size_t ctrl_data_size;
    cdc_comm_protocol_t comm_protocol;
    cdc_data_protocol_t data_protocol;
    int cdc_func_desc_cnt;
    const usb_standard_desc_t *(*cdc_func_desc)[];
    bool to_close;
    SLIST_ENTRY(cdc_host_common_port_s) list_entry;
} cdc_host_common_port_t;

typedef struct cdc_host_common_driver_s {
    usb_host_client_handle_t client_hdl;
    SemaphoreHandle_t open_close_mutex;
    EventGroupHandle_t event_group;
    size_t ref_count;
    SLIST_HEAD(list_dev, cdc_host_common_dev_s) dev_list;
    SLIST_HEAD(list_port, cdc_host_common_port_s) port_list;
    SLIST_HEAD(list_dev_cb, cdc_host_common_dev_event_cb_s) dev_event_cb_list;
} cdc_host_common_driver_t;

static cdc_host_common_driver_t *p_cdc_common = NULL;

static const cdc_host_common_driver_config_t cdc_common_default_config = {
    .task_stack_size = 4096,
    .task_priority = 10,
    .task_coreid = 0,
};

static void usb_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg);
static void in_xfer_cb(usb_transfer_t *transfer);
static void notif_xfer_cb(usb_transfer_t *transfer);
static void out_xfer_cb(usb_transfer_t *transfer);
static void ctrl_xfer_cb(usb_transfer_t *transfer);
static void cdc_common_port_close_locked(cdc_host_common_driver_t *driver, cdc_host_common_port_t *port);

static bool port_is_opened(cdc_host_common_port_t *port)
{
    cdc_host_common_port_t *current = NULL;
    SLIST_FOREACH(current, &p_cdc_common->port_list, list_entry) {
        if (current == port) {
            return true;
        }
    }
    return false;
}

static bool cdc_common_interface_is_opened(cdc_host_common_driver_t *driver, cdc_host_common_dev_t *dev, uint8_t intf_num)
{
    cdc_host_common_port_t *port = NULL;
    SLIST_FOREACH(port, &driver->port_list, list_entry) {
        if (port->dev != dev || port->to_close) {
            continue;
        }
        if (port->data.intf_desc && port->data.intf_desc->bInterfaceNumber == intf_num) {
            return true;
        }
        if (port->notif.intf_desc && port->notif.intf_desc->bInterfaceNumber == intf_num) {
            return true;
        }
    }
    return false;
}

static void cdc_common_client_task(void *arg)
{
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    cdc_host_common_driver_t *driver = p_cdc_common;
    assert(driver && driver->client_hdl);

    while (1) {
        usb_host_client_handle_events(driver->client_hdl, portMAX_DELAY);
        EventBits_t events = xEventGroupGetBits(driver->event_group);
        if (events & CDC_COMMON_TEARDOWN) {
            break;
        }
    }

    ESP_LOGD(TAG, "Deregistering common client");
    ESP_ERROR_CHECK(usb_host_client_deregister(driver->client_hdl));
    xEventGroupSetBits(driver->event_group, CDC_COMMON_TEARDOWN_COMPLETE);
    vTaskDelete(NULL);
}

static esp_err_t cdc_common_reset_transfer_endpoint(usb_device_handle_t dev_hdl, usb_transfer_t *transfer)
{
    assert(dev_hdl);
    assert(transfer);
    ESP_RETURN_ON_ERROR(usb_host_endpoint_halt(dev_hdl, transfer->bEndpointAddress), TAG, "Endpoint halt failed");
    ESP_RETURN_ON_ERROR(usb_host_endpoint_flush(dev_hdl, transfer->bEndpointAddress), TAG, "Endpoint flush failed");
    usb_host_endpoint_clear(dev_hdl, transfer->bEndpointAddress);
    return ESP_OK;
}

static void cdc_common_reset_in_transfer(cdc_host_common_port_t *port)
{
    assert(port->data.in_xfer);
    usb_transfer_t *transfer = port->data.in_xfer;
    uint8_t **ptr = (uint8_t **)(&(transfer->data_buffer));
    *ptr = port->data.in_data_buffer_base;
    transfer->num_bytes = transfer->data_buffer_size;
    transfer->num_bytes -= transfer->data_buffer_size % port->data.in_mps;
}

static size_t cdc_common_ringbuf_len(RingbufHandle_t ringbuf)
{
    size_t uxItemsWaiting = 0;
    vRingbufferGetInfo(ringbuf, NULL, NULL, NULL, NULL, &uxItemsWaiting);
    return uxItemsWaiting;
}

static esp_err_t cdc_common_ringbuf_pop(RingbufHandle_t ringbuf, uint8_t *buf, size_t req_bytes, size_t *read_bytes, TickType_t ticks_to_wait)
{
    uint8_t *buf_rcv = xRingbufferReceiveUpTo(ringbuf, read_bytes, ticks_to_wait, req_bytes);
    if (!buf_rcv) {
        return ESP_FAIL;
    }

    memcpy(buf, buf_rcv, *read_bytes);
    vRingbufferReturnItem(ringbuf, (void *)buf_rcv);

    size_t read_bytes2 = 0;
    if (*read_bytes < req_bytes) {
        buf_rcv = xRingbufferReceiveUpTo(ringbuf, &read_bytes2, 0, req_bytes - *read_bytes);
        if (buf_rcv) {
            memcpy(buf + *read_bytes, buf_rcv, read_bytes2);
            *read_bytes += read_bytes2;
            vRingbufferReturnItem(ringbuf, (void *)buf_rcv);
        }
    }

    return ESP_OK;
}

static esp_err_t cdc_common_ringbuf_push(RingbufHandle_t ringbuf, const uint8_t *buf, size_t write_bytes, TickType_t ticks_to_wait)
{
    int res = xRingbufferSend(ringbuf, buf, write_bytes, ticks_to_wait);
    if (res != pdTRUE) {
        ESP_LOGW(TAG, "CDC ringbuffer is full, data dropped");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void cdc_common_ringbuf_flush(RingbufHandle_t ringbuf)
{
    assert(ringbuf);
    size_t read_bytes = 0;
    size_t uxItemsWaiting = 0;
    vRingbufferGetInfo(ringbuf, NULL, NULL, NULL, NULL, &uxItemsWaiting);
    uint8_t *buf_rcv = xRingbufferReceiveUpTo(ringbuf, &read_bytes, 0, uxItemsWaiting);

    if (buf_rcv) {
        vRingbufferReturnItem(ringbuf, (void *)buf_rcv);
    }

    if (uxItemsWaiting > read_bytes) {
        // Read the second time to flush all data.
        vRingbufferGetInfo(ringbuf, NULL, NULL, NULL, NULL, &uxItemsWaiting);
        buf_rcv = xRingbufferReceiveUpTo(ringbuf, &read_bytes, 0, uxItemsWaiting);
        if (buf_rcv) {
            vRingbufferReturnItem(ringbuf, (void *)buf_rcv);
        }
    }
}

static esp_err_t cdc_common_submit_poll(usb_transfer_t *xfer, bool *polling_active, const char *xfer_name)
{
    if (*polling_active) {
        ESP_LOGD(TAG, "%s poll already active, skip submit", xfer_name);
        return ESP_OK;
    }

    esp_err_t err = usb_host_transfer_submit(xfer);
    if (err == ESP_OK) {
        *polling_active = true;
        return ESP_OK;
    }

    if (err == ESP_ERR_NOT_FINISHED) {
        *polling_active = true;
        ESP_LOGD(TAG, "%s poll already in-flight, treat as active", xfer_name);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "%s poll submit failed: %s", xfer_name, esp_err_to_name(err));
    return err;
}

static void cdc_common_suspend_polling(cdc_host_common_port_t *port)
{
    if (port->data.in_xfer) {
        port->data.in_polling = false;
    }
    if (port->notif.xfer) {
        port->notif.polling = false;
    }
}

#ifdef CDC_HOST_COMMON_SUSPEND_RESUME_API_SUPPORTED
static esp_err_t cdc_common_resume_polling(cdc_host_common_port_t *port)
{
    esp_err_t ret = ESP_OK;
    if (port->data.in_xfer) {
        ESP_RETURN_ON_ERROR(cdc_common_submit_poll(port->data.in_xfer, &port->data.in_polling, "BULK IN"), TAG, "BULK IN poll restart failed");
    }
    if (port->notif.xfer) {
        ESP_RETURN_ON_ERROR(cdc_common_submit_poll(port->notif.xfer, &port->notif.polling, "INTR IN"), TAG, "INTR IN poll restart failed");
    }
    return ret;
}
#endif

static void cdc_common_port_event(cdc_host_common_port_t *port, const cdc_host_common_port_event_data_t *event)
{
    if (!port->to_close && port->event_cb) {
        port->event_cb((cdc_host_common_port_handle_t)port, event, port->user_arg);
    }
}

static void cdc_common_disconnect_removed_ports(cdc_host_common_driver_t *driver, usb_device_handle_t dev_hdl, uint8_t dev_addr)
{
    // Notify each opened CDC port on the removed device before the common layer frees its port object.
    while (true) {
        cdc_host_common_port_t *port = NULL;
        cdc_host_common_port_event_cb_t event_cb = NULL;
        void *user_arg = NULL;

        xSemaphoreTake(driver->open_close_mutex, portMAX_DELAY);
        SLIST_FOREACH(port, &driver->port_list, list_entry) {
            bool dev_matched = port->dev && ((dev_hdl && port->dev->dev_hdl == dev_hdl) || (!dev_hdl && dev_addr && port->dev->dev_addr == dev_addr));
            if (dev_matched && !port->to_close) {
                // Reserve the port for the internal disconnect close before notifying upper layers.
                port->to_close = true;
                event_cb = port->event_cb;
                user_arg = port->user_arg;
                break;
            }
        }
        xSemaphoreGive(driver->open_close_mutex);

        if (!port) {
            break;
        }

        cdc_host_common_port_event_data_t event = {
            .type = CDC_HOST_COMMON_PORT_EVENT_DISCONNECTED,
            .data.port = (cdc_host_common_port_handle_t)port,
        };
        if (event_cb) {
            // The port is already marked closing, so call the saved callback directly instead of cdc_common_port_event().
            event_cb((cdc_host_common_port_handle_t)port, &event, user_arg);
        }

        xSemaphoreTake(driver->open_close_mutex, portMAX_DELAY);
        if (port_is_opened(port)) {
            // Release claimed interfaces before closing the gone device handle.
            cdc_common_port_close_locked(driver, port);
        }
        xSemaphoreGive(driver->open_close_mutex);
    }
}

static bool cdc_common_is_transfer_completed(cdc_host_common_port_t *port, usb_transfer_t *transfer)
{
    switch (transfer->status) {
    case USB_TRANSFER_STATUS_COMPLETED:
        return true;
    case USB_TRANSFER_STATUS_NO_DEVICE:
    case USB_TRANSFER_STATUS_CANCELED:
        return false;
    default: {
        cdc_host_common_port_event_data_t event = {
            .type = CDC_HOST_COMMON_PORT_EVENT_ERROR,
            .data.error = (int)transfer->status,
        };
        cdc_common_port_event(port, &event);
        return false;
    }
    }
}

static esp_err_t cdc_common_tx_submit_next(cdc_host_common_port_t *port)
{
    assert(port);
    assert(port->data.out_xfer);

    if (port->to_close || !port->data.tx_ringbuf) {
        port->data.tx_inflight = false;
        xSemaphoreGive(port->data.out_done_sem);
        return ESP_OK;
    }

    size_t data_len = cdc_common_ringbuf_len(port->data.tx_ringbuf);
    if (data_len == 0) {
        port->data.tx_inflight = false;
        xSemaphoreGive(port->data.out_done_sem);
        return ESP_OK;
    }

    if (data_len > port->data.out_xfer->data_buffer_size) {
        data_len = port->data.out_xfer->data_buffer_size;
    }

    size_t actual_num_bytes = 0;
    esp_err_t ret = cdc_common_ringbuf_pop(port->data.tx_ringbuf, port->data.out_xfer->data_buffer, data_len, &actual_num_bytes, 0);
    if (ret != ESP_OK || actual_num_bytes == 0) {
        port->data.tx_inflight = false;
        xSemaphoreGive(port->data.out_done_sem);
        ESP_LOGW(TAG, "CDC TX ringbuffer pop failed");
        return ESP_FAIL;
    }

    port->data.out_xfer->num_bytes = actual_num_bytes;
    port->data.out_xfer->timeout_ms = CDC_COMMON_TX_TIMEOUT_MS;
    port->data.tx_inflight = true;
    ret = usb_host_transfer_submit(port->data.out_xfer);
    if (ret != ESP_OK) {
        port->data.tx_inflight = false;
        xSemaphoreGive(port->data.out_done_sem);
        ESP_LOGE(TAG, "BULK OUT submit from TX ringbuffer failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

static void in_xfer_cb(usb_transfer_t *transfer)
{
    cdc_host_common_port_t *port = (cdc_host_common_port_t *)transfer->context;
    port->data.in_polling = false;

    if (!cdc_common_is_transfer_completed(port, transfer)) {
        return;
    }

    bool data_processed = true;
    port->data.current_data = transfer->data_buffer;
    port->data.current_data_len = transfer->actual_num_bytes;
    if (port->data.rx_ringbuf) {
        size_t ringbuf_len = cdc_common_ringbuf_len(port->data.rx_ringbuf);
        if (ringbuf_len + transfer->actual_num_bytes >= port->data.rx_ringbuf_size) {
            ESP_LOGW(TAG, "CDC RX ringbuffer is full, drop %u bytes", (unsigned)transfer->actual_num_bytes);
        } else if (cdc_common_ringbuf_push(port->data.rx_ringbuf, transfer->data_buffer, transfer->actual_num_bytes,
                                           pdMS_TO_TICKS(CDC_COMMON_RINGBUF_TIMEOUT_MS)) != ESP_OK) {
            ESP_LOGE(TAG, "CDC RX ringbuffer push failed");
        }
    }
    if (port->data_cb) {
        data_processed = port->data_cb((cdc_host_common_port_handle_t)port, transfer->data_buffer, transfer->actual_num_bytes, port->user_arg);
    }
    if (port->data.rx_ringbuf) {
        data_processed = true;
    }
    port->data.current_data = NULL;
    port->data.current_data_len = 0;

    if (!data_processed) {
#if !SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE
        uint8_t **ptr = (uint8_t **)(&(transfer->data_buffer));
        *ptr += transfer->actual_num_bytes;
        size_t space_left = transfer->data_buffer_size - (transfer->data_buffer - port->data.in_data_buffer_base);
        uint16_t mps = port->data.in_mps;
        transfer->num_bytes = (space_left / mps) * mps;
        if (transfer->num_bytes == 0) {
            ESP_LOGW(TAG, "IN buffer overflow");
            cdc_host_common_port_event_data_t overflow_event = {
                .type = CDC_HOST_COMMON_PORT_EVENT_RX_OVERFLOW,
                .data.port = (cdc_host_common_port_handle_t)port,
            };
            cdc_common_port_event(port, &overflow_event);
            cdc_common_reset_in_transfer(port);
        }
#else
        ESP_LOGW(TAG, "RX buffer append is not supported on this target");
        cdc_common_reset_in_transfer(port);
#endif
    } else {
        cdc_common_reset_in_transfer(port);
    }

    ESP_LOGD(TAG, "Submitting poll for BULK IN transfer");
    cdc_common_submit_poll(port->data.in_xfer, &port->data.in_polling, "BULK IN");
}

static void notif_xfer_cb(usb_transfer_t *transfer)
{
    cdc_host_common_port_t *port = (cdc_host_common_port_t *)transfer->context;
    port->notif.polling = false;

    if (cdc_common_is_transfer_completed(port, transfer)) {
        cdc_host_common_port_event_data_t event = {
            .type = CDC_HOST_COMMON_PORT_EVENT_NOTIFICATION,
            .data.notification = {
                .data = transfer->data_buffer,
                .data_len = transfer->actual_num_bytes,
            },
        };
        cdc_common_port_event(port, &event);
        ESP_LOGD(TAG, "Submitting poll for INTR IN transfer");
        cdc_common_submit_poll(port->notif.xfer, &port->notif.polling, "INTR IN");
    }
}

static void out_xfer_cb(usb_transfer_t *transfer)
{
    cdc_host_common_port_t *port = (cdc_host_common_port_t *)transfer->context;
    if (port->data.tx_ringbuf) {
        port->data.tx_inflight = false;
        if (port->to_close) {
            xSemaphoreGive(port->data.out_done_sem);
            return;
        }

        if (cdc_common_is_transfer_completed(port, transfer)) {
            esp_err_t ret = cdc_common_tx_submit_next(port);
            if (ret != ESP_OK) {
                cdc_host_common_port_event_data_t event = {
                    .type = CDC_HOST_COMMON_PORT_EVENT_ERROR,
                    .data.error = (int)ret,
                };
                cdc_common_port_event(port, &event);
            }
        } else {
            xSemaphoreGive(port->data.out_done_sem);
        }
        return;
    }
    xSemaphoreGive(port->data.out_done_sem);
}

static void ctrl_xfer_cb(usb_transfer_t *transfer)
{
    cdc_host_common_port_t *port = (cdc_host_common_port_t *)transfer->context;
    xSemaphoreGive(port->ctrl_done_sem);
}

static esp_err_t cdc_common_ringbufs_allocate(cdc_host_common_port_t *port, size_t rx_ringbuf_size, size_t tx_ringbuf_size)
{
    port->data.rx_ringbuf_size = rx_ringbuf_size;
    port->data.tx_ringbuf_size = tx_ringbuf_size;

    if (rx_ringbuf_size != 0) {
        port->data.rx_ringbuf = xRingbufferCreate(rx_ringbuf_size, RINGBUF_TYPE_BYTEBUF);
        ESP_RETURN_ON_FALSE(port->data.rx_ringbuf, ESP_ERR_NO_MEM, TAG, "Failed to create CDC RX ringbuffer");
    }

    if (tx_ringbuf_size != 0) {
        port->data.tx_ringbuf = xRingbufferCreate(tx_ringbuf_size, RINGBUF_TYPE_BYTEBUF);
        ESP_RETURN_ON_FALSE(port->data.tx_ringbuf, ESP_ERR_NO_MEM, TAG, "Failed to create CDC TX ringbuffer");
    }
    return ESP_OK;
}

static void cdc_common_ringbufs_free(cdc_host_common_port_t *port)
{
    if (port->data.rx_ringbuf) {
        cdc_common_ringbuf_flush(port->data.rx_ringbuf);
        vRingbufferDelete(port->data.rx_ringbuf);
        port->data.rx_ringbuf = NULL;
    }
    if (port->data.tx_ringbuf) {
        cdc_common_ringbuf_flush(port->data.tx_ringbuf);
        vRingbufferDelete(port->data.tx_ringbuf);
        port->data.tx_ringbuf = NULL;
    }
    port->data.rx_ringbuf_size = 0;
    port->data.tx_ringbuf_size = 0;
    port->data.tx_inflight = false;
}

static void cdc_common_transfers_free(cdc_host_common_port_t *port)
{
    assert(port);
    cdc_common_ringbufs_free(port);
    if (port->notif.xfer) {
        usb_host_transfer_free(port->notif.xfer);
        port->notif.xfer = NULL;
    }
    if (port->data.in_xfer) {
        cdc_common_reset_in_transfer(port);
        usb_host_transfer_free(port->data.in_xfer);
        port->data.in_xfer = NULL;
    }
    if (port->data.out_xfer) {
        usb_host_transfer_free(port->data.out_xfer);
        port->data.out_xfer = NULL;
    }
    if (port->ctrl_xfer) {
        usb_host_transfer_free(port->ctrl_xfer);
        port->ctrl_xfer = NULL;
    }
    if (port->data.out_done_sem) {
        vSemaphoreDelete(port->data.out_done_sem);
        port->data.out_done_sem = NULL;
    }
    if (port->data.out_mux) {
        vSemaphoreDelete(port->data.out_mux);
        port->data.out_mux = NULL;
    }
    if (port->ctrl_done_sem) {
        vSemaphoreDelete(port->ctrl_done_sem);
        port->ctrl_done_sem = NULL;
    }
    if (port->ctrl_mux) {
        vSemaphoreDelete(port->ctrl_mux);
        port->ctrl_mux = NULL;
    }
}

static esp_err_t cdc_common_transfers_allocate(cdc_host_common_port_t *port, const usb_ep_desc_t *notif_ep_desc,
                                               const usb_ep_desc_t *in_ep_desc, size_t in_buf_len, const usb_ep_desc_t *out_ep_desc,
                                               size_t out_buf_len, size_t ctrl_data_len)
{
    esp_err_t ret;
    assert(in_ep_desc);
    assert(out_ep_desc);

    if (notif_ep_desc) {
        ESP_GOTO_ON_ERROR(usb_host_transfer_alloc(USB_EP_DESC_GET_MPS(notif_ep_desc), 0, &port->notif.xfer),
                          err, TAG, "Failed to allocate notification transfer");
        port->notif.xfer->device_handle = port->dev->dev_hdl;
        port->notif.xfer->bEndpointAddress = notif_ep_desc->bEndpointAddress;
        port->notif.xfer->callback = notif_xfer_cb;
        port->notif.xfer->context = port;
        port->notif.xfer->num_bytes = USB_EP_DESC_GET_MPS(notif_ep_desc);
    }

    ESP_GOTO_ON_ERROR(usb_host_transfer_alloc(ctrl_data_len + sizeof(usb_setup_packet_t), 0, &port->ctrl_xfer),
                      err, TAG, "Failed to allocate control transfer");
    port->ctrl_xfer->timeout_ms = 1000;
    port->ctrl_xfer->bEndpointAddress = 0;
    port->ctrl_xfer->device_handle = port->dev->dev_hdl;
    port->ctrl_xfer->callback = ctrl_xfer_cb;
    port->ctrl_xfer->context = port;
    port->ctrl_done_sem = xSemaphoreCreateBinary();
    ESP_GOTO_ON_FALSE(port->ctrl_done_sem, ESP_ERR_NO_MEM, err, TAG, "Failed to create control semaphore");
    port->ctrl_mux = xSemaphoreCreateMutex();
    ESP_GOTO_ON_FALSE(port->ctrl_mux, ESP_ERR_NO_MEM, err, TAG, "Failed to create control mutex");

    if (in_buf_len != 0) {
        ESP_GOTO_ON_ERROR(usb_host_transfer_alloc(in_buf_len, 0, &port->data.in_xfer), err, TAG, "Failed to allocate data IN transfer");
        port->data.in_xfer->callback = in_xfer_cb;
        port->data.in_xfer->num_bytes = in_buf_len;
        port->data.in_xfer->bEndpointAddress = in_ep_desc->bEndpointAddress;
        port->data.in_xfer->device_handle = port->dev->dev_hdl;
        port->data.in_xfer->context = port;
        port->data.in_mps = USB_EP_DESC_GET_MPS(in_ep_desc);
        port->data.in_data_buffer_base = port->data.in_xfer->data_buffer;
    }

    if (out_buf_len != 0) {
        ESP_GOTO_ON_ERROR(usb_host_transfer_alloc(out_buf_len, 0, &port->data.out_xfer), err, TAG, "Failed to allocate data OUT transfer");
        port->data.out_xfer->device_handle = port->dev->dev_hdl;
        port->data.out_xfer->context = port;
        port->data.out_xfer->bEndpointAddress = out_ep_desc->bEndpointAddress;
        port->data.out_xfer->callback = out_xfer_cb;
        port->data.out_done_sem = xSemaphoreCreateBinary();
        ESP_GOTO_ON_FALSE(port->data.out_done_sem, ESP_ERR_NO_MEM, err, TAG, "Failed to create OUT semaphore");
        xSemaphoreGive(port->data.out_done_sem);
        port->data.out_mux = xSemaphoreCreateMutex();
        ESP_GOTO_ON_FALSE(port->data.out_mux, ESP_ERR_NO_MEM, err, TAG, "Failed to create OUT mutex");
    }
    return ESP_OK;

err:
    cdc_common_transfers_free(port);
    return ret;
}

static esp_err_t cdc_common_dev_ref_take(cdc_host_common_driver_t *driver, uint8_t dev_addr, uint16_t vid, uint16_t pid,
                                         uint32_t timeout_ms, cdc_host_common_dev_t **dev_ret)
{
    TickType_t timeout_ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    TimeOut_t connection_timeout;
    vTaskSetTimeOutState(&connection_timeout);

    do {
        cdc_host_common_dev_t *dev;
        SLIST_FOREACH(dev, &driver->dev_list, list_entry) {
            const usb_device_desc_t *device_desc;
            if (dev->dev_hdl && usb_host_get_device_descriptor(dev->dev_hdl, &device_desc) == ESP_OK &&
                    (vid == device_desc->idVendor || vid == CDC_HOST_COMMON_ANY_VID) &&
                    (pid == device_desc->idProduct || pid == CDC_HOST_COMMON_ANY_PID) &&
                    (dev_addr == dev->dev_addr || dev_addr == CDC_HOST_COMMON_ANY_DEV_ADDR)) {
                dev->ref_count++;
                *dev_ret = dev;
                return ESP_OK;
            }
        }

        uint8_t dev_addr_list[10];
        int num_of_devices;
        ESP_RETURN_ON_ERROR(usb_host_device_addr_list_fill(sizeof(dev_addr_list), dev_addr_list, &num_of_devices), TAG, "Failed to get USB device list");
        for (int i = 0; i < num_of_devices; i++) {
            bool already_tracked = false;
            SLIST_FOREACH(dev, &driver->dev_list, list_entry) {
                if (dev->dev_addr == dev_addr_list[i]) {
                    already_tracked = true;
                    break;
                }
            }
            if (already_tracked) {
                continue;
            }

            usb_device_handle_t current_device;
            esp_err_t ret = usb_host_device_open(driver->client_hdl, dev_addr_list[i], &current_device);
            if (ret != ESP_OK) {
                ESP_LOGD(TAG, "Deferred open for USB device addr %u: %s", dev_addr_list[i], esp_err_to_name(ret));
                continue;
            }

            const usb_device_desc_t *device_desc;
            ESP_ERROR_CHECK(usb_host_get_device_descriptor(current_device, &device_desc));
            if ((device_desc->bDeviceClass != USB_CLASS_HUB) &&
                    (vid == device_desc->idVendor || vid == CDC_HOST_COMMON_ANY_VID) &&
                    (pid == device_desc->idProduct || pid == CDC_HOST_COMMON_ANY_PID) &&
                    (dev_addr == dev_addr_list[i] || dev_addr == CDC_HOST_COMMON_ANY_DEV_ADDR)) {
                dev = calloc(1, sizeof(cdc_host_common_dev_t));
                if (!dev) {
                    usb_host_device_close(driver->client_hdl, current_device);
                    ESP_LOGE(TAG, "Failed to allocate CDC common device");
                    return ESP_ERR_NO_MEM;
                }
                dev->dev_hdl = current_device;
                dev->dev_addr = dev_addr_list[i];
                dev->ref_count = 1;
                SLIST_INSERT_HEAD(&driver->dev_list, dev, list_entry);
                *dev_ret = dev;
                return ESP_OK;
            }
            usb_host_device_close(driver->client_hdl, current_device);
        }

        xSemaphoreGive(driver->open_close_mutex);
        vTaskDelay(pdMS_TO_TICKS(50));
        xSemaphoreTake(driver->open_close_mutex, portMAX_DELAY);
    } while (xTaskCheckForTimeOut(&connection_timeout, &timeout_ticks) == pdFALSE);

    return ESP_ERR_NOT_FOUND;
}

static void cdc_common_dev_free(cdc_host_common_driver_t *driver, cdc_host_common_dev_t *dev)
{
    assert(dev);
    if (dev->dev_hdl) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(usb_host_device_close(driver->client_hdl, dev->dev_hdl));
        dev->dev_hdl = NULL;
    }
    SLIST_REMOVE(&driver->dev_list, dev, cdc_host_common_dev_s, list_entry);
    free(dev);
}

static void cdc_common_dev_ref_give(cdc_host_common_driver_t *driver, cdc_host_common_dev_t *dev)
{
    assert(dev);
    assert(dev->ref_count > 0);
    dev->ref_count--;
    if (dev->ref_count > 0 || dev->new_dev_pending) {
        return;
    }
    cdc_common_dev_free(driver, dev);
}

static void cdc_common_port_close_locked(cdc_host_common_driver_t *driver, cdc_host_common_port_t *port)
{
    assert(driver);
    assert(port);

    port->to_close = true;
    port->data_cb = NULL;
    port->event_cb = NULL;
    port->user_arg = NULL;

    CDC_COMMON_ENTER_CRITICAL();
    SLIST_REMOVE(&driver->port_list, port, cdc_host_common_port_s, list_entry);
    CDC_COMMON_EXIT_CRITICAL();

    usb_device_handle_t dev_hdl = port->dev ? port->dev->dev_hdl : NULL;
    cdc_common_suspend_polling(port);

    if (dev_hdl) {
        if (port->data.in_xfer) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(cdc_common_reset_transfer_endpoint(dev_hdl, port->data.in_xfer));
        }
        if (port->data.out_xfer) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(cdc_common_reset_transfer_endpoint(dev_hdl, port->data.out_xfer));
        }
        if (port->notif.xfer) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(cdc_common_reset_transfer_endpoint(dev_hdl, port->notif.xfer));
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (port->data.out_mux) {
        if (xSemaphoreTake(port->data.out_mux, pdMS_TO_TICKS(CDC_COMMON_TX_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "Timed out waiting for TX mutex during close; forcing tear-down");
        }
    }

    if (dev_hdl) {
        if (port->data.intf_desc) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(usb_host_interface_release(driver->client_hdl, dev_hdl, port->data.intf_desc->bInterfaceNumber));
        }
        if (port->notif.intf_desc && port->notif.intf_desc != port->data.intf_desc) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(usb_host_interface_release(driver->client_hdl, dev_hdl, port->notif.intf_desc->bInterfaceNumber));
        }
    }

    free(port->cdc_func_desc);
    cdc_common_transfers_free(port);
    if (port->dev) {
        cdc_common_dev_ref_give(driver, port->dev);
    } else {
        ESP_LOGE(TAG, "CDC port has no parent device during close");
    }
    free(port);
}

esp_err_t cdc_host_common_acquire(const cdc_host_common_driver_config_t *config, cdc_host_common_driver_handle_t *driver_ret)
{
    ESP_RETURN_ON_FALSE(driver_ret, ESP_ERR_INVALID_ARG, TAG, "driver_ret is NULL");

    CDC_COMMON_ENTER_CRITICAL();
    if (p_cdc_common) {
        p_cdc_common->ref_count++;
        *driver_ret = (cdc_host_common_driver_handle_t)p_cdc_common;
        CDC_COMMON_EXIT_CRITICAL();
        return ESP_OK;
    }
    CDC_COMMON_EXIT_CRITICAL();

    if (!config) {
        config = &cdc_common_default_config;
    }

    esp_err_t ret;
    cdc_host_common_driver_t *driver = heap_caps_calloc(1, sizeof(cdc_host_common_driver_t), MALLOC_CAP_DEFAULT);
    EventGroupHandle_t event_group = xEventGroupCreate();
    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    TaskHandle_t task_h = NULL;
    BaseType_t core_id = (config->task_coreid < 0) ? tskNO_AFFINITY : config->task_coreid;
    xTaskCreatePinnedToCore(cdc_common_client_task, "cdc_common", config->task_stack_size, NULL, config->task_priority, &task_h, core_id);

    if (!driver || !event_group || !mutex || !task_h) {
        ret = ESP_ERR_NO_MEM;
        goto err;
    }

    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
#if USB_EVENT_SUPPORT_REMOVED
        .flags = {
            .notify_dev_removed = 1,
        },
#endif
        .async.client_event_callback = usb_event_cb,
        .async.callback_arg = NULL,
    };
    ESP_GOTO_ON_ERROR(usb_host_client_register(&client_config, &driver->client_hdl), err, TAG, "Failed to register USB host client");

    driver->event_group = event_group;
    driver->open_close_mutex = mutex;
    driver->ref_count = 1;
    SLIST_INIT(&driver->dev_list);
    SLIST_INIT(&driver->port_list);
    SLIST_INIT(&driver->dev_event_cb_list);

    CDC_COMMON_ENTER_CRITICAL();
    if (p_cdc_common) {
        p_cdc_common->ref_count++;
        *driver_ret = (cdc_host_common_driver_handle_t)p_cdc_common;
        CDC_COMMON_EXIT_CRITICAL();
        usb_host_client_deregister(driver->client_hdl);
        vTaskDelete(task_h);
        vEventGroupDelete(event_group);
        vSemaphoreDelete(mutex);
        free(driver);
        return ESP_OK;
    }
    p_cdc_common = driver;
    CDC_COMMON_EXIT_CRITICAL();

    xTaskNotifyGive(task_h);
    *driver_ret = (cdc_host_common_driver_handle_t)driver;
    return ESP_OK;

err:
    if (driver && driver->client_hdl) {
        usb_host_client_deregister(driver->client_hdl);
    }
    if (task_h) {
        vTaskDelete(task_h);
    }
    if (event_group) {
        vEventGroupDelete(event_group);
    }
    if (mutex) {
        vSemaphoreDelete(mutex);
    }
    free(driver);
    return ret;
}

esp_err_t cdc_host_common_release(cdc_host_common_driver_handle_t driver_hdl)
{
    ESP_RETURN_ON_FALSE(driver_hdl, ESP_ERR_INVALID_ARG, TAG, "driver is NULL");
    cdc_host_common_driver_t *driver = (cdc_host_common_driver_t *)driver_hdl;
    ESP_RETURN_ON_FALSE(driver == p_cdc_common, ESP_ERR_INVALID_ARG, TAG, "invalid common driver");

    xSemaphoreTake(driver->open_close_mutex, portMAX_DELAY);
    if (!SLIST_EMPTY(&driver->port_list)) {
        xSemaphoreGive(driver->open_close_mutex);
        ESP_LOGE(TAG, "Cannot release common driver while ports are open");
        return ESP_ERR_INVALID_STATE;
    }

    CDC_COMMON_ENTER_CRITICAL();
    if (--driver->ref_count > 0) {
        CDC_COMMON_EXIT_CRITICAL();
        xSemaphoreGive(driver->open_close_mutex);
        return ESP_OK;
    }
    p_cdc_common = NULL;
    CDC_COMMON_EXIT_CRITICAL();

    cdc_host_common_dev_event_cb_item_t *cb, *tmp_cb;
    SLIST_FOREACH_SAFE(cb, &driver->dev_event_cb_list, list_entry, tmp_cb) {
        SLIST_REMOVE(&driver->dev_event_cb_list, cb, cdc_host_common_dev_event_cb_s, list_entry);
        free(cb);
    }

    xEventGroupSetBits(driver->event_group, CDC_COMMON_TEARDOWN);
    usb_host_client_unblock(driver->client_hdl);

    EventBits_t bits = xEventGroupWaitBits(driver->event_group, CDC_COMMON_TEARDOWN_COMPLETE, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(1000));
    if (!(bits & CDC_COMMON_TEARDOWN_COMPLETE)) {
        ESP_LOGE(TAG, "CDC common client task did not tear down within timeout; leaking driver");
        CDC_COMMON_ENTER_CRITICAL();
        driver->ref_count = 1;
        p_cdc_common = driver;
        CDC_COMMON_EXIT_CRITICAL();
        xSemaphoreGive(driver->open_close_mutex);
        return ESP_ERR_NOT_FINISHED;
    }
    vEventGroupDelete(driver->event_group);
    xSemaphoreGive(driver->open_close_mutex);
    vSemaphoreDelete(driver->open_close_mutex);
    free(driver);
    return ESP_OK;
}

esp_err_t cdc_host_common_register_dev_event_cb(cdc_host_common_driver_handle_t driver_hdl, cdc_host_common_dev_event_cb_t cb,
                                                void *user_arg, cdc_host_common_dev_event_cb_handle_t *cb_handle_ret)
{
    ESP_RETURN_ON_FALSE(driver_hdl && cb && cb_handle_ret, ESP_ERR_INVALID_ARG, TAG, "invalid dev event callback argument");
    cdc_host_common_driver_t *driver = (cdc_host_common_driver_t *)driver_hdl;
    cdc_host_common_dev_event_cb_item_t *item = calloc(1, sizeof(cdc_host_common_dev_event_cb_item_t));
    ESP_RETURN_ON_FALSE(item, ESP_ERR_NO_MEM, TAG, "Failed to allocate dev event callback");
    item->cb = cb;
    item->user_arg = user_arg;
    CDC_COMMON_ENTER_CRITICAL();
    SLIST_INSERT_HEAD(&driver->dev_event_cb_list, item, list_entry);
    CDC_COMMON_EXIT_CRITICAL();
    *cb_handle_ret = (cdc_host_common_dev_event_cb_handle_t)item;
    return ESP_OK;
}

esp_err_t cdc_host_common_unregister_dev_event_cb(cdc_host_common_dev_event_cb_handle_t cb_handle)
{
    ESP_RETURN_ON_FALSE(p_cdc_common && cb_handle, ESP_ERR_INVALID_ARG, TAG, "invalid dev event callback handle");
    cdc_host_common_dev_event_cb_item_t *item = (cdc_host_common_dev_event_cb_item_t *)cb_handle;
    cdc_host_common_dev_event_cb_item_t *current, *tmp;
    CDC_COMMON_ENTER_CRITICAL();
    SLIST_FOREACH_SAFE(current, &p_cdc_common->dev_event_cb_list, list_entry, tmp) {
        if (current == item) {
            SLIST_REMOVE(&p_cdc_common->dev_event_cb_list, current, cdc_host_common_dev_event_cb_s, list_entry);
            CDC_COMMON_EXIT_CRITICAL();
            free(current);
            return ESP_OK;
        }
    }
    CDC_COMMON_EXIT_CRITICAL();
    return ESP_ERR_NOT_FOUND;
}

esp_err_t cdc_host_common_open(cdc_host_common_driver_handle_t driver_hdl, const cdc_host_common_open_config_t *open_config,
                               cdc_host_common_port_handle_t *port_ret)
{
    ESP_RETURN_ON_FALSE(driver_hdl && open_config && port_ret, ESP_ERR_INVALID_ARG, TAG, "invalid open argument");
    cdc_host_common_driver_t *driver = (cdc_host_common_driver_t *)driver_hdl;
    esp_err_t ret;

    xSemaphoreTake(driver->open_close_mutex, portMAX_DELAY);
    cdc_host_common_dev_t *dev = NULL;
    ESP_GOTO_ON_ERROR(cdc_common_dev_ref_take(driver, open_config->dev_addr, open_config->vid, open_config->pid,
                                              open_config->connection_timeout_ms, &dev),
                      exit, TAG, "CDC device not found");
    ESP_GOTO_ON_FALSE(!cdc_common_interface_is_opened(driver, dev, open_config->interface_idx),
                      ESP_ERR_INVALID_STATE, err_dev, TAG, "CDC interface %u already opened", open_config->interface_idx);

    const usb_device_desc_t *device_desc;
    const usb_config_desc_t *config_desc;
    ESP_GOTO_ON_ERROR(usb_host_get_device_descriptor(dev->dev_hdl, &device_desc), err_dev, TAG, "Failed to get device descriptor");
    ESP_GOTO_ON_ERROR(usb_host_get_active_config_descriptor(dev->dev_hdl, &config_desc), err_dev, TAG, "Failed to get active configuration descriptor");

    cdc_host_common_parsed_info_t cdc_info;
    ESP_GOTO_ON_ERROR(cdc_host_common_parse_interface_descriptor(device_desc, config_desc, open_config->interface_idx, &cdc_info),
                      err_dev, TAG, "Could not parse CDC interface %d", open_config->interface_idx);

    if (open_config->flags & CDC_HOST_COMMON_OPEN_FLAG_DISABLE_NOTIFICATION) {
        cdc_info.notif_ep = NULL;
        cdc_info.notif_intf = NULL;
    }

    ESP_GOTO_ON_FALSE(!cdc_common_interface_is_opened(driver, dev, cdc_info.data_intf->bInterfaceNumber),
                      ESP_ERR_INVALID_STATE, err_desc, TAG, "CDC data interface %u already opened", cdc_info.data_intf->bInterfaceNumber);
    if (cdc_info.notif_intf && cdc_info.notif_intf != cdc_info.data_intf) {
        ESP_GOTO_ON_FALSE(!cdc_common_interface_is_opened(driver, dev, cdc_info.notif_intf->bInterfaceNumber),
                          ESP_ERR_INVALID_STATE, err_desc, TAG, "CDC notification interface %u already opened", cdc_info.notif_intf->bInterfaceNumber);
    }

    cdc_host_common_port_t *port = calloc(1, sizeof(cdc_host_common_port_t));
    ESP_GOTO_ON_FALSE(port, ESP_ERR_NO_MEM, err_desc, TAG, "Failed to allocate CDC common port");
    port->dev = dev;
    port->data_cb = open_config->data_cb;
    port->event_cb = open_config->event_cb;
    port->user_arg = open_config->user_arg;
    port->data.intf_desc = cdc_info.data_intf;
    port->notif.intf_desc = cdc_info.notif_intf;
    port->data_protocol = (cdc_data_protocol_t)cdc_info.data_intf->bInterfaceProtocol;
    if (cdc_info.notif_intf) {
        port->comm_protocol = (cdc_comm_protocol_t)cdc_info.notif_intf->bInterfaceProtocol;
    }
    port->cdc_func_desc = cdc_info.func;
    port->cdc_func_desc_cnt = cdc_info.func_cnt;

    ESP_GOTO_ON_FALSE(open_config->tx_ringbuf_size == 0 || open_config->out_buffer_size != 0,
                      ESP_ERR_INVALID_ARG, err_port, TAG, "TX ringbuffer requires OUT transfer");
    ESP_GOTO_ON_ERROR(cdc_common_ringbufs_allocate(port, open_config->rx_ringbuf_size, open_config->tx_ringbuf_size),
                      err_port, TAG, "Failed to allocate CDC ringbuffers");

    const size_t ctrl_data_size = open_config->ctrl_buffer_size ? open_config->ctrl_buffer_size : CDC_COMMON_CTRL_DATA_SIZE_DEFAULT;
    port->ctrl_data_size = ctrl_data_size;
    const size_t in_buf_size = ((open_config->data_cb || open_config->rx_ringbuf_size) && open_config->in_buffer_size == 0) ?
                               USB_EP_DESC_GET_MPS(cdc_info.in_ep) : open_config->in_buffer_size;
    ESP_GOTO_ON_ERROR(cdc_common_transfers_allocate(port, cdc_info.notif_ep, cdc_info.in_ep, in_buf_size, cdc_info.out_ep,
                                                    open_config->out_buffer_size, ctrl_data_size),
                      err_port, TAG, "Failed to allocate CDC transfers");

    ESP_GOTO_ON_ERROR(usb_host_interface_claim(driver->client_hdl, dev->dev_hdl, port->data.intf_desc->bInterfaceNumber,
                                               port->data.intf_desc->bAlternateSetting),
                      err_port, TAG, "Could not claim data interface");
    bool data_claimed = true;
    bool notif_claimed = false;
    if (port->notif.xfer && port->notif.intf_desc != port->data.intf_desc) {
        ESP_GOTO_ON_ERROR(usb_host_interface_claim(driver->client_hdl, dev->dev_hdl, port->notif.intf_desc->bInterfaceNumber,
                                                   port->notif.intf_desc->bAlternateSetting),
                          err_claim, TAG, "Could not claim notification interface");
        notif_claimed = true;
    }

    CDC_COMMON_ENTER_CRITICAL();
    SLIST_INSERT_HEAD(&driver->port_list, port, list_entry);
    CDC_COMMON_EXIT_CRITICAL();

    if (port->data.in_xfer) {
        ESP_GOTO_ON_ERROR(cdc_common_submit_poll(port->data.in_xfer, &port->data.in_polling, "BULK IN"), err_started, TAG, "Failed to submit data IN poll");
    }
    if (port->notif.xfer) {
        ESP_GOTO_ON_ERROR(cdc_common_submit_poll(port->notif.xfer, &port->notif.polling, "INTR IN"), err_started, TAG, "Failed to submit notification poll");
    }

    *port_ret = (cdc_host_common_port_handle_t)port;
    xSemaphoreGive(driver->open_close_mutex);
    return ESP_OK;

err_started:
    CDC_COMMON_ENTER_CRITICAL();
    SLIST_REMOVE(&driver->port_list, port, cdc_host_common_port_s, list_entry);
    CDC_COMMON_EXIT_CRITICAL();
err_claim:
    if (notif_claimed) {
        usb_host_interface_release(driver->client_hdl, dev->dev_hdl, port->notif.intf_desc->bInterfaceNumber);
    }
    if (data_claimed) {
        usb_host_interface_release(driver->client_hdl, dev->dev_hdl, port->data.intf_desc->bInterfaceNumber);
    }
err_port:
    cdc_common_transfers_free(port);
    free(port);
err_desc:
    free(cdc_info.func);
err_dev:
    cdc_common_dev_ref_give(driver, dev);
exit:
    *port_ret = NULL;
    xSemaphoreGive(driver->open_close_mutex);
    return ret;
}

esp_err_t cdc_host_common_close(cdc_host_common_port_handle_t port_hdl)
{
    ESP_RETURN_ON_FALSE(p_cdc_common && port_hdl, ESP_ERR_INVALID_ARG, TAG, "invalid close argument");
    cdc_host_common_driver_t *driver = p_cdc_common;
    cdc_host_common_port_t *port = (cdc_host_common_port_t *)port_hdl;

    xSemaphoreTake(driver->open_close_mutex, portMAX_DELAY);

    if (!port_is_opened(port)) {
        xSemaphoreGive(driver->open_close_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    if (port->to_close) {
        xSemaphoreGive(driver->open_close_mutex);
        return ESP_OK;
    }
    cdc_common_port_close_locked(driver, port);
    xSemaphoreGive(driver->open_close_mutex);
    return ESP_OK;
}

static esp_err_t cdc_common_tx_blocking(cdc_host_common_port_handle_t port_hdl, const uint8_t *data, size_t data_len, TickType_t ticks_to_wait)
{
    ESP_RETURN_ON_FALSE(port_hdl && data && data_len > 0, ESP_ERR_INVALID_ARG, TAG, "invalid TX argument");
    cdc_host_common_port_t *port = (cdc_host_common_port_t *)port_hdl;
    ESP_RETURN_ON_FALSE(port_is_opened(port), ESP_ERR_INVALID_ARG, TAG, "invalid CDC common port");
    ESP_RETURN_ON_FALSE(port->data.out_xfer, ESP_ERR_NOT_SUPPORTED, TAG, "CDC port is read-only");
    ESP_RETURN_ON_FALSE(!port->data.tx_ringbuf, ESP_ERR_INVALID_STATE, TAG, "TX ringbuffer is enabled");

    const size_t buffer_size = port->data.out_xfer->data_buffer_size;
    const uint8_t *data_ptr = data;
    size_t remaining = data_len;
    TickType_t start_ticks = xTaskGetTickCount();
    TickType_t timeout_ticks = ticks_to_wait;
    int remaining_timeout_ticks = timeout_ticks;

    BaseType_t taken = xSemaphoreTake(port->data.out_mux, remaining_timeout_ticks);
    if (taken != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    while (remaining > 0) {
        if (port->to_close) {
            ret = ESP_ERR_INVALID_STATE;
            goto unblock;
        }
        size_t chunk_size = (remaining > buffer_size) ? buffer_size : remaining;
        xSemaphoreTake(port->data.out_done_sem, 0);
        memcpy(port->data.out_xfer->data_buffer, data_ptr, chunk_size);
        port->data.out_xfer->num_bytes = chunk_size;
        port->data.out_xfer->timeout_ms = pdTICKS_TO_MS(remaining_timeout_ticks);
        ESP_GOTO_ON_ERROR(usb_host_transfer_submit(port->data.out_xfer), unblock, TAG, "BULK OUT submit failed");

        taken = xSemaphoreTake(port->data.out_done_sem, remaining_timeout_ticks);
        remaining_timeout_ticks = timeout_ticks - (xTaskGetTickCount() - start_ticks);
        if (!taken || remaining_timeout_ticks < 0) {
            // Don't touch dev_hdl if disconnect concurrently kicked off close; the endpoint is already being reset by cdc_common_port_close_locked.
            if (!port->to_close && port->dev && port->dev->dev_hdl) {
                cdc_common_reset_transfer_endpoint(port->dev->dev_hdl, port->data.out_xfer);
            }
            ESP_LOGW(TAG, "TX transfer timeout");
            ret = ESP_ERR_TIMEOUT;
            goto unblock;
        }
        ESP_GOTO_ON_FALSE(port->data.out_xfer->status == USB_TRANSFER_STATUS_COMPLETED, ESP_ERR_INVALID_RESPONSE, unblock, TAG, "Bulk OUT transfer error");
        ESP_GOTO_ON_FALSE(port->data.out_xfer->actual_num_bytes == chunk_size, ESP_ERR_INVALID_RESPONSE, unblock, TAG, "Incorrect number of bytes transferred");
        remaining -= chunk_size;
        data_ptr += chunk_size;
    }

unblock:
    xSemaphoreGive(port->data.out_mux);
    return ret;
}

esp_err_t cdc_host_common_write_bytes(cdc_host_common_port_handle_t port_hdl, const uint8_t *data, size_t data_len, TickType_t ticks_to_wait)
{
    ESP_RETURN_ON_FALSE(port_hdl && data && data_len > 0, ESP_ERR_INVALID_ARG, TAG, "invalid write argument");
    cdc_host_common_port_t *port = (cdc_host_common_port_t *)port_hdl;
    ESP_RETURN_ON_FALSE(port_is_opened(port), ESP_ERR_INVALID_ARG, TAG, "invalid CDC common port");
    ESP_RETURN_ON_FALSE(port->data.out_xfer, ESP_ERR_NOT_SUPPORTED, TAG, "CDC port is read-only");

    if (!port->data.tx_ringbuf) {
        TickType_t effective_ticks = (ticks_to_wait == portMAX_DELAY) ? pdMS_TO_TICKS(CDC_COMMON_TX_TIMEOUT_MS) : ticks_to_wait;
        return cdc_common_tx_blocking(port_hdl, data, data_len, effective_ticks);
    }

    BaseType_t taken = xSemaphoreTake(port->data.out_mux, ticks_to_wait);
    if (taken != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = cdc_common_ringbuf_push(port->data.tx_ringbuf, data, data_len, ticks_to_wait);
    ESP_GOTO_ON_ERROR(ret, unblock, TAG, "CDC TX ringbuffer push failed");

    if (xSemaphoreTake(port->data.out_done_sem, 0) == pdTRUE) {
        ret = cdc_common_tx_submit_next(port);
    }

unblock:
    xSemaphoreGive(port->data.out_mux);
    return ret;
}

esp_err_t cdc_host_common_read_bytes(cdc_host_common_port_handle_t port_hdl, uint8_t *buf, size_t *length, TickType_t ticks_to_wait)
{
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_FALSE(length, ESP_ERR_INVALID_ARG, TAG, "length is NULL");
    ESP_GOTO_ON_FALSE(port_hdl && buf, ESP_ERR_INVALID_ARG, fail, TAG, "invalid read argument");
    cdc_host_common_port_t *port = (cdc_host_common_port_t *)port_hdl;
    ESP_GOTO_ON_FALSE(port_is_opened(port), ESP_ERR_INVALID_ARG, fail, TAG, "invalid CDC common port");

    if (port->data.rx_ringbuf) {
        size_t data_len = *length;
        if (data_len > port->data.rx_ringbuf_size) {
            data_len = port->data.rx_ringbuf_size;
        }
        ret = cdc_common_ringbuf_pop(port->data.rx_ringbuf, buf, data_len, length, ticks_to_wait);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "CDC RX ringbuffer read failed");
            goto fail;
        }
        return ESP_OK;
    }

    if (*length != port->data.current_data_len) {
        ESP_LOGE(TAG, "length is invalid, length = %u, actual_num_bytes = %u", (unsigned)*length, (unsigned)port->data.current_data_len);
        *length = port->data.current_data_len;
        return ESP_ERR_INVALID_ARG;
    }
    if (!port->data.current_data || port->data.current_data_len == 0) {
        *length = 0;
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(buf, port->data.current_data, port->data.current_data_len);
    *length = port->data.current_data_len;
    return ESP_OK;

fail:
    *length = 0;
    return ret;
}

esp_err_t cdc_host_common_flush_rx_buffer(cdc_host_common_port_handle_t port_hdl)
{
    ESP_RETURN_ON_FALSE(port_hdl, ESP_ERR_INVALID_ARG, TAG, "port is NULL");
    cdc_host_common_port_t *port = (cdc_host_common_port_t *)port_hdl;
    ESP_RETURN_ON_FALSE(port_is_opened(port), ESP_ERR_INVALID_ARG, TAG, "invalid CDC common port");
    ESP_RETURN_ON_FALSE(port->data.rx_ringbuf, ESP_ERR_NOT_SUPPORTED, TAG, "RX ringbuffer is not created");
    cdc_common_ringbuf_flush(port->data.rx_ringbuf);
    return ESP_OK;
}

esp_err_t cdc_host_common_flush_tx_buffer(cdc_host_common_port_handle_t port_hdl)
{
    ESP_RETURN_ON_FALSE(port_hdl, ESP_ERR_INVALID_ARG, TAG, "port is NULL");
    cdc_host_common_port_t *port = (cdc_host_common_port_t *)port_hdl;
    ESP_RETURN_ON_FALSE(port_is_opened(port), ESP_ERR_INVALID_ARG, TAG, "invalid CDC common port");
    ESP_RETURN_ON_FALSE(port->data.tx_ringbuf, ESP_ERR_NOT_SUPPORTED, TAG, "TX ringbuffer is not created");

    BaseType_t taken = xSemaphoreTake(port->data.out_mux, pdMS_TO_TICKS(CDC_COMMON_TX_TIMEOUT_MS));
    if (taken != pdTRUE) {
        ESP_LOGW(TAG, "CDC TX ringbuffer flush timeout");
        return ESP_ERR_TIMEOUT;
    }
    cdc_common_ringbuf_flush(port->data.tx_ringbuf);
    xSemaphoreGive(port->data.out_mux);
    return ESP_OK;
}

esp_err_t cdc_host_common_get_rx_buffer_size(cdc_host_common_port_handle_t port_hdl, size_t *size)
{
    ESP_RETURN_ON_FALSE(port_hdl && size, ESP_ERR_INVALID_ARG, TAG, "invalid RX size argument");
    cdc_host_common_port_t *port = (cdc_host_common_port_t *)port_hdl;
    ESP_RETURN_ON_FALSE(port_is_opened(port), ESP_ERR_INVALID_ARG, TAG, "invalid CDC common port");
    if (port->data.rx_ringbuf) {
        *size = cdc_common_ringbuf_len(port->data.rx_ringbuf);
    } else {
        *size = port->data.current_data_len;
    }
    return ESP_OK;
}

esp_err_t cdc_host_common_send_control(cdc_host_common_port_handle_t port_hdl, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue,
                                       uint16_t wIndex, uint16_t wLength, uint8_t *data)
{
    ESP_RETURN_ON_FALSE(port_hdl, ESP_ERR_INVALID_ARG, TAG, "port is NULL");
    if (wLength > 0) {
        ESP_RETURN_ON_FALSE(data, ESP_ERR_INVALID_ARG, TAG, "data is NULL");
    }
    cdc_host_common_port_t *port = (cdc_host_common_port_t *)port_hdl;
    ESP_RETURN_ON_FALSE(port_is_opened(port), ESP_ERR_INVALID_ARG, TAG, "invalid CDC common port");
    ESP_RETURN_ON_FALSE(port->dev && port->dev->dev_hdl, ESP_ERR_INVALID_STATE, TAG, "CDC device is not open");
    ESP_RETURN_ON_FALSE(wLength <= port->ctrl_data_size, ESP_ERR_INVALID_SIZE, TAG, "Control payload is too large");

    BaseType_t taken = xSemaphoreTake(port->ctrl_mux, pdMS_TO_TICKS(CDC_COMMON_CTRL_TIMEOUT_MS));
    if (!taken) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret;
    usb_setup_packet_t *req = (usb_setup_packet_t *)port->ctrl_xfer->data_buffer;
    uint8_t *start_of_data = (uint8_t *)req + sizeof(usb_setup_packet_t);
    req->bmRequestType = bmRequestType;
    req->bRequest = bRequest;
    req->wValue = wValue;
    req->wIndex = wIndex;
    req->wLength = wLength;

    const bool in_transfer = bmRequestType & USB_BM_REQUEST_TYPE_DIR_IN;
    if (!in_transfer) {
        memcpy(start_of_data, data, wLength);
    }

    port->ctrl_xfer->num_bytes = wLength + sizeof(usb_setup_packet_t);
    ESP_GOTO_ON_ERROR(usb_host_transfer_submit_control(p_cdc_common->client_hdl, port->ctrl_xfer), unblock, TAG, "CTRL transfer failed");

    taken = xSemaphoreTake(port->ctrl_done_sem, pdMS_TO_TICKS(CDC_COMMON_CTRL_TIMEOUT_MS));
    if (!taken) {
        ret = ESP_ERR_TIMEOUT;
        goto unblock;
    }

    ESP_GOTO_ON_FALSE(port->ctrl_xfer->status == USB_TRANSFER_STATUS_COMPLETED, ESP_ERR_INVALID_RESPONSE, unblock, TAG, "Control transfer error");
    if (in_transfer) {
        const size_t total_acked = port->ctrl_xfer->actual_num_bytes;
        const size_t expected = sizeof(usb_setup_packet_t) + wLength;
        if (total_acked < expected) {
            const size_t got_data = (total_acked > sizeof(usb_setup_packet_t)) ? total_acked - sizeof(usb_setup_packet_t) : 0;
            ESP_LOGD(TAG, "Short control IN reply: expected %u B, got %u B", (unsigned)wLength, (unsigned)got_data);
        }
        memcpy(data, start_of_data, wLength);
    }
    ret = ESP_OK;

unblock:
    xSemaphoreGive(port->ctrl_mux);
    return ret;
}

esp_err_t cdc_host_common_get_rx_data(cdc_host_common_port_handle_t port_hdl, const uint8_t **data, size_t *data_len)
{
    ESP_RETURN_ON_FALSE(port_hdl && data && data_len, ESP_ERR_INVALID_ARG, TAG, "invalid RX data argument");
    cdc_host_common_port_t *port = (cdc_host_common_port_t *)port_hdl;
    ESP_RETURN_ON_FALSE(port_is_opened(port), ESP_ERR_INVALID_ARG, TAG, "invalid CDC common port");
    *data = port->data.current_data;
    *data_len = port->data.current_data_len;
    return (*data && *data_len) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t cdc_host_common_get_dev_handle(cdc_host_common_port_handle_t port_hdl, usb_device_handle_t *dev_hdl)
{
    ESP_RETURN_ON_FALSE(port_hdl && dev_hdl, ESP_ERR_INVALID_ARG, TAG, "invalid dev handle argument");
    cdc_host_common_port_t *port = (cdc_host_common_port_t *)port_hdl;
    ESP_RETURN_ON_FALSE(port_is_opened(port), ESP_ERR_INVALID_ARG, TAG, "invalid CDC common port");
    ESP_RETURN_ON_FALSE(port->dev && port->dev->dev_hdl, ESP_ERR_INVALID_STATE, TAG, "CDC device is not open");
    *dev_hdl = port->dev->dev_hdl;
    return ESP_OK;
}

esp_err_t cdc_host_common_get_intf_desc(cdc_host_common_port_handle_t port_hdl, const usb_intf_desc_t **notif_intf, const usb_intf_desc_t **data_intf)
{
    ESP_RETURN_ON_FALSE(port_hdl, ESP_ERR_INVALID_ARG, TAG, "port is NULL");
    cdc_host_common_port_t *port = (cdc_host_common_port_t *)port_hdl;
    ESP_RETURN_ON_FALSE(port_is_opened(port), ESP_ERR_INVALID_ARG, TAG, "invalid CDC common port");
    if (notif_intf) {
        *notif_intf = port->notif.intf_desc;
    }
    if (data_intf) {
        *data_intf = port->data.intf_desc;
    }
    return ESP_OK;
}

esp_err_t cdc_host_common_protocols_get(cdc_host_common_port_handle_t port_hdl, cdc_comm_protocol_t *comm, cdc_data_protocol_t *data)
{
    ESP_RETURN_ON_FALSE(port_hdl, ESP_ERR_INVALID_ARG, TAG, "port is NULL");
    cdc_host_common_port_t *port = (cdc_host_common_port_t *)port_hdl;
    ESP_RETURN_ON_FALSE(port_is_opened(port), ESP_ERR_INVALID_ARG, TAG, "invalid CDC common port");
    if (comm) {
        *comm = port->comm_protocol;
    }
    if (data) {
        *data = port->data_protocol;
    }
    return ESP_OK;
}

esp_err_t cdc_host_common_cdc_desc_get(cdc_host_common_port_handle_t port_hdl, cdc_desc_subtype_t desc_type, const usb_standard_desc_t **desc_out)
{
    ESP_RETURN_ON_FALSE(port_hdl && desc_out, ESP_ERR_INVALID_ARG, TAG, "invalid CDC desc argument");
    ESP_RETURN_ON_FALSE(desc_type < USB_CDC_DESC_SUBTYPE_MAX, ESP_ERR_INVALID_ARG, TAG, "invalid CDC desc subtype");
    cdc_host_common_port_t *port = (cdc_host_common_port_t *)port_hdl;
    ESP_RETURN_ON_FALSE(port_is_opened(port), ESP_ERR_INVALID_ARG, TAG, "invalid CDC common port");
    *desc_out = NULL;
    for (int i = 0; i < port->cdc_func_desc_cnt; i++) {
        const cdc_header_desc_t *_desc = (const cdc_header_desc_t *)((*(port->cdc_func_desc))[i]);
        if (_desc->bDescriptorSubtype == desc_type) {
            *desc_out = (const usb_standard_desc_t *)_desc;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

void cdc_host_common_desc_print(cdc_host_common_port_handle_t port_hdl)
{
    assert(port_hdl);
    cdc_host_common_port_t *port = (cdc_host_common_port_t *)port_hdl;
    const usb_device_desc_t *device_desc;
    const usb_config_desc_t *config_desc;
    ESP_ERROR_CHECK_WITHOUT_ABORT(usb_host_get_device_descriptor(port->dev->dev_hdl, &device_desc));
    ESP_ERROR_CHECK_WITHOUT_ABORT(usb_host_get_active_config_descriptor(port->dev->dev_hdl, &config_desc));
    usb_print_device_descriptor(device_desc);
    usb_print_config_descriptor(config_desc, cdc_host_common_print_desc);
}

#ifdef CDC_HOST_COMMON_REMOTE_WAKE_SUPPORTED
esp_err_t cdc_host_common_enable_remote_wakeup(cdc_host_common_port_handle_t port_hdl, bool enable)
{
    ESP_RETURN_ON_FALSE(port_hdl, ESP_ERR_INVALID_ARG, TAG, "port is NULL");
    cdc_host_common_port_t *port = (cdc_host_common_port_t *)port_hdl;
    ESP_RETURN_ON_FALSE(port_is_opened(port), ESP_ERR_INVALID_ARG, TAG, "invalid CDC common port");
    ESP_RETURN_ON_FALSE(port->dev && port->dev->dev_hdl, ESP_ERR_INVALID_STATE, TAG, "CDC device is not open");

    const usb_config_desc_t *config_desc;
    ESP_RETURN_ON_ERROR(usb_host_get_active_config_descriptor(port->dev->dev_hdl, &config_desc), TAG, "Unable to get configuration descriptor");
    ESP_RETURN_ON_FALSE((config_desc->bmAttributes & USB_BM_ATTRIBUTES_WAKEUP), ESP_ERR_NOT_SUPPORTED, TAG, "Device does not support remote wakeup");

    if (port->dev->remote_wakeup_enabled == enable) {
        return ESP_OK;
    }

    BaseType_t taken = xSemaphoreTake(port->ctrl_mux, pdMS_TO_TICKS(CDC_COMMON_CTRL_TIMEOUT_MS));
    if (!taken) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret;
    usb_setup_packet_t *req = (usb_setup_packet_t *)port->ctrl_xfer->data_buffer;
    if (enable) {
        USB_SETUP_PACKET_INIT_SET_FEATURE(req, DEVICE_REMOTE_WAKEUP);
    } else {
        USB_SETUP_PACKET_INIT_CLEAR_FEATURE(req, DEVICE_REMOTE_WAKEUP);
    }
    port->ctrl_xfer->num_bytes = sizeof(usb_setup_packet_t);
    ESP_GOTO_ON_ERROR(usb_host_transfer_submit_control(p_cdc_common->client_hdl, port->ctrl_xfer), unlock, TAG, "CTRL transfer failed");

    taken = xSemaphoreTake(port->ctrl_done_sem, pdMS_TO_TICKS(CDC_COMMON_CTRL_TIMEOUT_MS));
    if (!taken) {
        ret = ESP_ERR_TIMEOUT;
        goto unlock;
    }
    ESP_GOTO_ON_FALSE(port->ctrl_xfer->status == USB_TRANSFER_STATUS_COMPLETED, ESP_ERR_INVALID_RESPONSE, unlock, TAG, "Control transfer error");
    port->dev->remote_wakeup_enabled = enable;
    ret = ESP_OK;

unlock:
    xSemaphoreGive(port->ctrl_mux);
    return ret;
}
#endif

static void usb_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV: {
        ESP_LOGI(TAG, "New device connected, address: %d", event_msg->new_dev.address);
        usb_device_handle_t new_dev;
        esp_err_t ret = usb_host_device_open(p_cdc_common->client_hdl, event_msg->new_dev.address, &new_dev);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to open new USB device addr %u: %s", event_msg->new_dev.address, esp_err_to_name(ret));
            break;
        }

        const usb_device_desc_t *device_desc = NULL;
        const usb_config_desc_t *config_desc = NULL;
        ret = usb_host_get_device_descriptor(new_dev, &device_desc);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to get descriptor for USB device addr %u: %s", event_msg->new_dev.address, esp_err_to_name(ret));
            usb_host_device_close(p_cdc_common->client_hdl, new_dev);
            break;
        }
        ret = usb_host_get_active_config_descriptor(new_dev, &config_desc);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to get config descriptor for USB device addr %u: %s", event_msg->new_dev.address, esp_err_to_name(ret));
            usb_host_device_close(p_cdc_common->client_hdl, new_dev);
            break;
        }
        if (device_desc->bDeviceClass == USB_CLASS_HUB) {
            usb_host_device_close(p_cdc_common->client_hdl, new_dev);
            break;
        }

        cdc_host_common_dev_t *dev = calloc(1, sizeof(cdc_host_common_dev_t));
        if (!dev) {
            ESP_LOGE(TAG, "Failed to allocate CDC common device for addr %u", event_msg->new_dev.address);
            usb_host_device_close(p_cdc_common->client_hdl, new_dev);
            break;
        }
        dev->dev_hdl = new_dev;
        dev->dev_addr = event_msg->new_dev.address;
        dev->ref_count = 0;
        dev->new_dev_pending = true;

        xSemaphoreTake(p_cdc_common->open_close_mutex, portMAX_DELAY);
        SLIST_INSERT_HEAD(&p_cdc_common->dev_list, dev, list_entry);
        xSemaphoreGive(p_cdc_common->open_close_mutex);

        // The temporary device stays in dev_list so callbacks can synchronously open a CDC port.
        cdc_host_common_dev_event_cb_item_t *cb = NULL;
        cdc_host_common_dev_event_data_t event = {
            .type = CDC_HOST_COMMON_DEV_EVENT_NEW,
            .data.new_dev = {
                .dev_hdl = new_dev,
                .dev_addr = event_msg->new_dev.address,
                .device_desc = device_desc,
                .config_desc = config_desc,
            },
        };
        SLIST_FOREACH(cb, &p_cdc_common->dev_event_cb_list, list_entry) {
            cb->cb(&event, cb->user_arg);
        }

        xSemaphoreTake(p_cdc_common->open_close_mutex, portMAX_DELAY);
        dev->new_dev_pending = false;
        if (dev->ref_count == 0) {
            cdc_common_dev_free(p_cdc_common, dev);
        }
        xSemaphoreGive(p_cdc_common->open_close_mutex);
        break;
    }
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
#if USB_EVENT_SUPPORT_REMOVED
    case USB_HOST_CLIENT_EVENT_DEV_REMOVED:
#endif
    {
        usb_device_handle_t dev_hdl = NULL;
        uint8_t dev_addr = 0;

#if USB_EVENT_SUPPORT_REMOVED
        if (event_msg->event == USB_HOST_CLIENT_EVENT_DEV_REMOVED) {
            dev_addr = event_msg->dev_removed.address;
            ESP_LOGW(TAG, "USB device removed, address: %d", dev_addr);
        } else
#endif
        {
            dev_hdl = event_msg->dev_gone.dev_hdl;
            usb_device_info_t device_info;
            esp_err_t ret = usb_host_device_info(dev_hdl, &device_info);
            if (ret == ESP_OK) {
                dev_addr = device_info.dev_addr;
                ESP_LOGW(TAG, "USB device disconnected, address: %d", dev_addr);
            } else {
                ESP_LOGW(TAG, "Failed to get disconnected USB device info: %s", esp_err_to_name(ret));
            }
        }

        // GONE is the common-layer device removal event. DEV_REMOVED callers do not have a stable device handle.
        cdc_host_common_dev_event_cb_item_t *cb = NULL;
        cdc_host_common_dev_event_data_t dev_event = {
            .type = CDC_HOST_COMMON_DEV_EVENT_GONE,
            .data.dev_gone = {
                .dev_hdl = dev_hdl,
                .dev_addr = dev_addr,
            },
        };
        SLIST_FOREACH(cb, &p_cdc_common->dev_event_cb_list, list_entry) {
            cb->cb(&dev_event, cb->user_arg);
        }

        cdc_common_disconnect_removed_ports(p_cdc_common, dev_hdl, dev_addr);
        break;
    }
#ifdef CDC_HOST_COMMON_SUSPEND_RESUME_API_SUPPORTED
    case USB_HOST_CLIENT_EVENT_DEV_SUSPENDED: {
        cdc_host_common_port_t *port, *tmp;
        SLIST_FOREACH_SAFE(port, &p_cdc_common->port_list, list_entry, tmp) {
            if (port->dev->dev_hdl != event_msg->dev_suspend_resume.dev_hdl) {
                continue;
            }
            cdc_common_suspend_polling(port);
            cdc_host_common_port_event_data_t event = {
                .type = CDC_HOST_COMMON_PORT_EVENT_SUSPENDED,
                .data.port = (cdc_host_common_port_handle_t)port,
            };
            cdc_common_port_event(port, &event);
        }
        break;
    }
    case USB_HOST_CLIENT_EVENT_DEV_RESUMED: {
        cdc_host_common_port_t *port, *tmp;
        SLIST_FOREACH_SAFE(port, &p_cdc_common->port_list, list_entry, tmp) {
            if (port->dev->dev_hdl != event_msg->dev_suspend_resume.dev_hdl) {
                continue;
            }
            esp_err_t ret = cdc_common_resume_polling(port);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Resume polling failed: %s", esp_err_to_name(ret));
            }
            cdc_host_common_port_event_data_t event = {
                .type = CDC_HOST_COMMON_PORT_EVENT_RESUMED,
                .data.port = (cdc_host_common_port_handle_t)port,
            };
            cdc_common_port_event(port, &event);
        }
        break;
    }
#endif
    default:
        ESP_LOGE(TAG, "Unrecognized USB Host client event");
        assert(false);
        break;
    }
}
