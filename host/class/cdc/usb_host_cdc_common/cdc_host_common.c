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
#include "esp_private/critical_section.h"
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
#define CDC_COMMON_PORT_CLOSED           BIT0
#define CDC_COMMON_CLOSE_RETRY_MS        10
#define CDC_COMMON_TX_TIMEOUT_MS         5000

DEFINE_CRIT_SECTION_LOCK_STATIC(cdc_common_lock);
#define CDC_COMMON_ENTER_CRITICAL()      esp_os_enter_critical(&cdc_common_lock)
#define CDC_COMMON_EXIT_CRITICAL()       esp_os_exit_critical(&cdc_common_lock)

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
        usb_intf_desc_t descriptor;
        const usb_intf_desc_t *intf_desc;
        bool polling;
        bool claimed;
    } notif;
    struct {
        usb_transfer_t *out_xfer;
        usb_transfer_t *in_xfer;
        SemaphoreHandle_t out_done_sem;
        SemaphoreHandle_t out_mux;
        uint16_t in_mps;
        uint8_t *in_data_buffer_base;
        usb_intf_desc_t descriptor;
        const usb_intf_desc_t *intf_desc;
        bool in_polling;
        bool claimed;
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
    bool close_requested;
    bool resources_freed;
    bool endpoints_stopped;
    bool opening;
    bool suspended;
    bool ctrl_inflight;
    size_t users;
    size_t close_waiters;
    EventGroupHandle_t close_event;
    // Closing ports stay alive until callbacks and API users have returned.
    SLIST_ENTRY(cdc_host_common_port_s) list_entry;
} cdc_host_common_port_t;

typedef struct cdc_host_common_driver_s {
    usb_host_client_handle_t client_hdl;
    TaskHandle_t task_hdl;
    SemaphoreHandle_t open_close_mutex;
    EventGroupHandle_t event_group;
    size_t ref_count;
    bool stopping;
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
static bool cdc_common_cleanup_ports(cdc_host_common_driver_t *driver);

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

static esp_err_t cdc_common_port_get(cdc_host_common_port_handle_t handle, cdc_host_common_port_t **port_ret)
{
    cdc_host_common_driver_t *driver = p_cdc_common;
    ESP_RETURN_ON_FALSE(driver && handle, ESP_ERR_INVALID_ARG, TAG, "invalid CDC common port");
    xSemaphoreTake(driver->open_close_mutex, portMAX_DELAY);
    cdc_host_common_port_t *port = (cdc_host_common_port_t *)handle;
    esp_err_t ret = ESP_ERR_INVALID_ARG;
    if (port_is_opened(port)) {
        ret = port->to_close ? ESP_ERR_INVALID_STATE : ESP_OK;
        if (ret == ESP_OK) {
            port->users++;
            *port_ret = port;
        }
    }
    xSemaphoreGive(driver->open_close_mutex);
    return ret;
}

static void cdc_common_port_put(cdc_host_common_port_t *port)
{
    xSemaphoreTake(p_cdc_common->open_close_mutex, portMAX_DELAY);
    assert(port->users > 0);
    port->users--;
    xSemaphoreGive(p_cdc_common->open_close_mutex);
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

    TickType_t wait_ticks = portMAX_DELAY;
    while (1) {
        usb_host_client_handle_events(driver->client_hdl, wait_ticks);
        // USB Host updates endpoint accounting only after transfer callbacks return.
        wait_ticks = cdc_common_cleanup_ports(driver) ? pdMS_TO_TICKS(CDC_COMMON_CLOSE_RETRY_MS) : portMAX_DELAY;
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

static esp_err_t cdc_common_ringbuf_io(cdc_host_common_port_t *port, uint8_t *data, size_t length, size_t *read_length, TickType_t ticks_to_wait)
{
    TimeOut_t timeout;
    vTaskSetTimeOutState(&timeout);
    TickType_t remaining = ticks_to_wait;
    const TickType_t retry_ticks = pdMS_TO_TICKS(CDC_COMMON_CLOSE_RETRY_MS) ? pdMS_TO_TICKS(CDC_COMMON_CLOSE_RETRY_MS) : 1;
    do {
        if (port->to_close) {
            return ESP_ERR_INVALID_STATE;
        }
        TickType_t wait_ticks = remaining < retry_ticks ? remaining : retry_ticks;
        esp_err_t ret = read_length ? cdc_common_ringbuf_pop(port->data.rx_ringbuf, data, length, read_length, wait_ticks) :
                        (xRingbufferSend(port->data.tx_ringbuf, data, length, wait_ticks) == pdTRUE ? ESP_OK : ESP_FAIL);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
    } while (xTaskCheckForTimeOut(&timeout, &remaining) == pdFALSE);
    return ESP_ERR_TIMEOUT;
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

    *polling_active = true;
    esp_err_t err = usb_host_transfer_submit(xfer);
    if (err == ESP_OK) {
        return ESP_OK;
    }

    if (err == ESP_ERR_NOT_FINISHED) {
        *polling_active = true;
        ESP_LOGD(TAG, "%s poll already in-flight, treat as active", xfer_name);
        return ESP_OK;
    }

    *polling_active = false;
    ESP_LOGE(TAG, "%s poll submit failed: %s", xfer_name, esp_err_to_name(err));
    return err;
}

static esp_err_t cdc_common_submit(cdc_host_common_port_t *port, usb_transfer_t *transfer, bool control)
{
    cdc_host_common_driver_t *driver = p_cdc_common;
    xSemaphoreTake(driver->open_close_mutex, portMAX_DELAY);
    bool *inflight = control ? &port->ctrl_inflight : &port->data.tx_inflight;
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (!port->to_close) {
        if (*inflight) {
            ret = ESP_ERR_NOT_FINISHED;
        } else {
            *inflight = true;
            ret = control ? usb_host_transfer_submit_control(driver->client_hdl, transfer) : usb_host_transfer_submit(transfer);
            if (ret != ESP_OK) {
                *inflight = false;
            }
        }
    }
    xSemaphoreGive(driver->open_close_mutex);
    return ret;
}

#ifdef CDC_HOST_COMMON_SUSPEND_RESUME_API_SUPPORTED
static void cdc_common_suspend_polling(cdc_host_common_port_t *port)
{
    port->suspended = true;
}

static esp_err_t cdc_common_resume_polling(cdc_host_common_port_t *port)
{
    esp_err_t ret = ESP_OK;
    if (port->to_close) {
        return ESP_OK;
    }
    port->suspended = false;
    if (port->opening) {
        return ESP_OK;
    }
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
    while (true) {
        cdc_host_common_port_t *port;
        xSemaphoreTake(driver->open_close_mutex, portMAX_DELAY);
        SLIST_FOREACH(port, &driver->port_list, list_entry) {
            bool dev_matched = port->dev && ((dev_hdl && port->dev->dev_hdl == dev_hdl) || (!dev_hdl && dev_addr && port->dev->dev_addr == dev_addr));
            if (dev_matched && !port->to_close) {
                break;
            }
        }
        if (!port) {
            xSemaphoreGive(driver->open_close_mutex);
            break;
        }
        port->to_close = true;
        cdc_host_common_port_event_cb_t event_cb = port->event_cb;
        void *user_arg = port->user_arg;
        xSemaphoreGive(driver->open_close_mutex);

        // Disconnect releases USB resources; only explicit close releases the handle.
        const cdc_host_common_port_event_data_t event = {
            .type = CDC_HOST_COMMON_PORT_EVENT_DISCONNECTED,
            .data.port = (cdc_host_common_port_handle_t)port,
        };
        if (event_cb) {
            event_cb((cdc_host_common_port_handle_t)port, &event, user_arg);
        }
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

    size_t data_len;
    while ((data_len = cdc_common_ringbuf_len(port->data.tx_ringbuf)) == 0) {
        port->data.tx_inflight = false;
        xSemaphoreGive(port->data.out_done_sem);
        // Recheck after publishing idle so a concurrent enqueue cannot lose its wakeup.
        if (cdc_common_ringbuf_len(port->data.tx_ringbuf) == 0 || xSemaphoreTake(port->data.out_done_sem, 0) != pdTRUE) {
            return ESP_OK;
        }
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
    ret = cdc_common_submit(port, port->data.out_xfer, false);
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

    if (port->to_close || port->suspended || !cdc_common_is_transfer_completed(port, transfer)) {
        return;
    }

    bool data_processed = true;
    port->data.current_data = transfer->data_buffer;
    port->data.current_data_len = transfer->actual_num_bytes;
    if (port->data.rx_ringbuf && transfer->actual_num_bytes > 0 &&
            xRingbufferSend(port->data.rx_ringbuf, transfer->data_buffer, transfer->actual_num_bytes, 0) != pdTRUE) {
        ESP_LOGW(TAG, "CDC RX ringbuffer full, dropped %u bytes", (unsigned)transfer->actual_num_bytes);
        const cdc_host_common_port_event_data_t event = {
            .type = CDC_HOST_COMMON_PORT_EVENT_RX_OVERFLOW,
            .data.port = (cdc_host_common_port_handle_t)port,
        };
        cdc_common_port_event(port, &event);
    }
    if (!port->to_close && port->data_cb) {
        data_processed = port->data_cb((cdc_host_common_port_handle_t)port, transfer->data_buffer, transfer->actual_num_bytes, port->user_arg);
    }
    if (port->data.rx_ringbuf) {
        data_processed = true;
    }
    port->data.current_data = NULL;
    port->data.current_data_len = 0;

    if (!data_processed) {
#if !SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE && !CONFIG_IDF_TARGET_ESP32S31
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

    xSemaphoreTake(p_cdc_common->open_close_mutex, portMAX_DELAY);
    if (!port->to_close && !port->suspended) {
        cdc_common_submit_poll(port->data.in_xfer, &port->data.in_polling, "BULK IN");
    }
    xSemaphoreGive(p_cdc_common->open_close_mutex);
}

static void notif_xfer_cb(usb_transfer_t *transfer)
{
    cdc_host_common_port_t *port = (cdc_host_common_port_t *)transfer->context;
    port->notif.polling = false;

    if (!port->to_close && !port->suspended && cdc_common_is_transfer_completed(port, transfer)) {
        cdc_host_common_port_event_data_t event = {
            .type = CDC_HOST_COMMON_PORT_EVENT_NOTIFICATION,
            .data.notification = {
                .data = transfer->data_buffer,
                .data_len = transfer->actual_num_bytes,
            },
        };
        cdc_common_port_event(port, &event);
        xSemaphoreTake(p_cdc_common->open_close_mutex, portMAX_DELAY);
        if (!port->to_close && !port->suspended) {
            cdc_common_submit_poll(port->notif.xfer, &port->notif.polling, "INTR IN");
        }
        xSemaphoreGive(p_cdc_common->open_close_mutex);
    }
}

static void out_xfer_cb(usb_transfer_t *transfer)
{
    cdc_host_common_port_t *port = (cdc_host_common_port_t *)transfer->context;
    port->data.tx_inflight = false;
    if (port->data.tx_ringbuf) {
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
    port->ctrl_inflight = false;
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

static void cdc_common_port_free(cdc_host_common_driver_t *driver, cdc_host_common_port_t *port)
{
    CDC_COMMON_ENTER_CRITICAL();
    SLIST_REMOVE(&driver->port_list, port, cdc_host_common_port_s, list_entry);
    CDC_COMMON_EXIT_CRITICAL();
    vEventGroupDelete(port->close_event);
    free(port);
}

static esp_err_t cdc_common_wait_closed(cdc_host_common_driver_t *driver, cdc_host_common_port_t *port, bool retain_handle_on_timeout)
{
    xEventGroupWaitBits(port->close_event, CDC_COMMON_PORT_CLOSED, pdFALSE, pdTRUE, pdMS_TO_TICKS(CDC_COMMON_CTRL_TIMEOUT_MS));
    xSemaphoreTake(driver->open_close_mutex, portMAX_DELAY);
    esp_err_t ret = port->resources_freed ? ESP_OK : ESP_ERR_TIMEOUT;
    if (ret != ESP_OK) {
        // A late completion must not destroy the handle after a failed close.
        if (retain_handle_on_timeout) {
            port->close_requested = false;
        }
        ESP_LOGE(TAG, "CDC close timed out waiting for pending I/O");
    }
    if (--port->close_waiters == 0 && ret == ESP_OK) {
        cdc_common_port_free(driver, port);
    }
    xSemaphoreGive(driver->open_close_mutex);
    return ret;
}

static bool cdc_common_cleanup_ports(cdc_host_common_driver_t *driver)
{
    bool pending = false;
    xSemaphoreTake(driver->open_close_mutex, portMAX_DELAY);
    cdc_host_common_dev_event_cb_item_t *cb, *next_cb;
    CDC_COMMON_ENTER_CRITICAL();
    SLIST_FOREACH_SAFE(cb, &driver->dev_event_cb_list, list_entry, next_cb) {
        if (!cb->cb) {
            SLIST_REMOVE(&driver->dev_event_cb_list, cb, cdc_host_common_dev_event_cb_s, list_entry);
            free(cb);
        }
    }
    CDC_COMMON_EXIT_CRITICAL();
    cdc_host_common_port_t *port, *next;
    SLIST_FOREACH_SAFE(port, &driver->port_list, list_entry, next) {
        if (!port->to_close) {
            continue;
        }
        if (!port->resources_freed) {
            pending = true;
            if (!port->endpoints_stopped) {
                usb_transfer_t *transfers[] = {
                    port->data.claimed ? port->data.in_xfer : NULL,
                    port->data.claimed ? port->data.out_xfer : NULL,
                    (port->notif.claimed || port->notif.intf_desc == port->data.intf_desc) ? port->notif.xfer : NULL,
                };
                bool stopped = true;
                for (size_t i = 0; i < sizeof(transfers) / sizeof(transfers[0]); i++) {
                    if (transfers[i] && cdc_common_reset_transfer_endpoint(port->dev->dev_hdl, transfers[i]) != ESP_OK) {
                        stopped = false;
                    }
                }
                port->endpoints_stopped = stopped;
                // Flushed completions must pass through handle_events before releasing interfaces.
                continue;
            }
            if (port->users || port->data.in_polling || port->notif.polling || port->data.tx_inflight || port->ctrl_inflight) {
                continue;
            }
            if (port->data.claimed) {
                esp_err_t ret = usb_host_interface_release(driver->client_hdl, port->dev->dev_hdl, port->data.intf_desc->bInterfaceNumber);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Data interface release failed: %s", esp_err_to_name(ret));
                    continue;
                }
                port->data.claimed = false;
            }
            if (port->notif.claimed) {
                esp_err_t ret = usb_host_interface_release(driver->client_hdl, port->dev->dev_hdl, port->notif.intf_desc->bInterfaceNumber);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Notification interface release failed: %s", esp_err_to_name(ret));
                    continue;
                }
                port->notif.claimed = false;
            }
            free(port->cdc_func_desc);
            port->cdc_func_desc = NULL;
            cdc_common_transfers_free(port);
            cdc_common_dev_ref_give(driver, port->dev);
            port->dev = NULL;
            port->resources_freed = true;
            xEventGroupSetBits(port->close_event, CDC_COMMON_PORT_CLOSED);
        }
        if (port->close_requested && port->close_waiters == 0) {
            cdc_common_port_free(driver, port);
        }
    }
    xSemaphoreGive(driver->open_close_mutex);
    return pending;
}

esp_err_t cdc_host_common_acquire(const cdc_host_common_driver_config_t *config, cdc_host_common_driver_handle_t *driver_ret)
{
    ESP_RETURN_ON_FALSE(driver_ret, ESP_ERR_INVALID_ARG, TAG, "driver_ret is NULL");

    CDC_COMMON_ENTER_CRITICAL();
    if (p_cdc_common) {
        if (p_cdc_common->stopping) {
            CDC_COMMON_EXIT_CRITICAL();
            return ESP_ERR_INVALID_STATE;
        }
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

    driver->task_hdl = task_h;
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
    ESP_RETURN_ON_FALSE(xTaskGetCurrentTaskHandle() != driver->task_hdl, ESP_ERR_INVALID_STATE, TAG, "Release from CDC callback is not supported");

    xSemaphoreTake(driver->open_close_mutex, portMAX_DELAY);
    cdc_host_common_port_t *port;
    SLIST_FOREACH(port, &driver->port_list, list_entry) {
        if (!port->close_requested) {
            xSemaphoreGive(driver->open_close_mutex);
            ESP_LOGE(TAG, "Cannot release common driver while ports are open");
            return ESP_ERR_INVALID_STATE;
        }
    }
    while ((port = SLIST_FIRST(&driver->port_list)) != NULL) {
        port->close_waiters++;
        xSemaphoreGive(driver->open_close_mutex);
        esp_err_t ret = cdc_common_wait_closed(driver, port, false);
        if (ret != ESP_OK) {
            return ret;
        }
        xSemaphoreTake(driver->open_close_mutex, portMAX_DELAY);
    }

    CDC_COMMON_ENTER_CRITICAL();
    if (--driver->ref_count > 0) {
        CDC_COMMON_EXIT_CRITICAL();
        xSemaphoreGive(driver->open_close_mutex);
        return ESP_OK;
    }
    driver->stopping = true;
    CDC_COMMON_EXIT_CRITICAL();
    xEventGroupSetBits(driver->event_group, CDC_COMMON_TEARDOWN);
    xSemaphoreGive(driver->open_close_mutex);
    usb_host_client_unblock(driver->client_hdl);

    // The client may still be finishing a callback that needs open_close_mutex.
    xEventGroupWaitBits(driver->event_group, CDC_COMMON_TEARDOWN_COMPLETE, pdFALSE, pdTRUE, portMAX_DELAY);
    CDC_COMMON_ENTER_CRITICAL();
    p_cdc_common = NULL;
    CDC_COMMON_EXIT_CRITICAL();

    cdc_host_common_dev_event_cb_item_t *cb, *tmp_cb;
    SLIST_FOREACH_SAFE(cb, &driver->dev_event_cb_list, list_entry, tmp_cb) {
        free(cb);
    }
    vEventGroupDelete(driver->event_group);
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
        if (current == item && current->cb) {
            current->cb = NULL;
            CDC_COMMON_EXIT_CRITICAL();
            usb_host_client_unblock(p_cdc_common->client_hdl);
            return ESP_OK;
        }
    }
    CDC_COMMON_EXIT_CRITICAL();
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t cdc_common_reset_interface_endpoints(cdc_host_common_port_t *port, const usb_config_desc_t *config_desc, const usb_intf_desc_t *intf_desc)
{
    for (int i = 0; i < intf_desc->bNumEndpoints; i++) {
        int offset = (const uint8_t *)intf_desc - (const uint8_t *)config_desc;
        const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(intf_desc, i, config_desc->wTotalLength, &offset);
        ESP_RETURN_ON_FALSE(ep, ESP_ERR_NOT_FOUND, TAG, "Interface %u endpoint %d was not found", intf_desc->bInterfaceNumber, i);
        if (USB_EP_DESC_GET_XFERTYPE(ep) == USB_TRANSFER_TYPE_BULK || USB_EP_DESC_GET_XFERTYPE(ep) == USB_TRANSFER_TYPE_INTR) {
            ESP_RETURN_ON_ERROR(cdc_host_common_send_control((cdc_host_common_port_handle_t)port, USB_BM_REQUEST_TYPE_RECIP_ENDPOINT,
                                                             USB_B_REQUEST_CLEAR_FEATURE, 0, ep->bEndpointAddress, 0, NULL),
                                TAG, "Endpoint 0x%02x synchronization failed", ep->bEndpointAddress);
        }
    }
    return ESP_OK;
}

esp_err_t cdc_host_common_open(cdc_host_common_driver_handle_t driver_hdl, const cdc_host_common_open_config_t *open_config,
                               cdc_host_common_port_handle_t *port_ret)
{
    ESP_RETURN_ON_FALSE(driver_hdl && open_config && port_ret, ESP_ERR_INVALID_ARG, TAG, "invalid open argument");
    *port_ret = NULL;
    cdc_host_common_driver_t *driver = (cdc_host_common_driver_t *)driver_hdl;
    ESP_RETURN_ON_FALSE(driver == p_cdc_common, ESP_ERR_INVALID_ARG, TAG, "invalid common driver");
    ESP_RETURN_ON_FALSE(xTaskGetCurrentTaskHandle() != driver->task_hdl, ESP_ERR_INVALID_STATE, TAG, "Cannot open a CDC port from a driver callback");
    esp_err_t ret;

    xSemaphoreTake(driver->open_close_mutex, portMAX_DELAY);
    ESP_GOTO_ON_FALSE(!driver->stopping, ESP_ERR_INVALID_STATE, exit, TAG, "CDC driver is stopping");
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
    port->close_event = xEventGroupCreate();
    ESP_GOTO_ON_FALSE(port->close_event, ESP_ERR_NO_MEM, err_port, TAG, "Failed to allocate close event");
    port->opening = true;
    port->data.descriptor = *cdc_info.data_intf;
    port->data.intf_desc = &port->data.descriptor;
    if (cdc_info.notif_intf == cdc_info.data_intf) {
        port->notif.intf_desc = port->data.intf_desc;
    } else if (cdc_info.notif_intf) {
        port->notif.descriptor = *cdc_info.notif_intf;
        port->notif.intf_desc = &port->notif.descriptor;
    }
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
    port->data.claimed = true;
    if (port->notif.xfer && port->notif.intf_desc != port->data.intf_desc) {
        ESP_GOTO_ON_ERROR(usb_host_interface_claim(driver->client_hdl, dev->dev_hdl, port->notif.intf_desc->bInterfaceNumber,
                                                   port->notif.intf_desc->bAlternateSetting),
                          err_claim, TAG, "Could not claim notification interface");
        port->notif.claimed = true;
    }

    CDC_COMMON_ENTER_CRITICAL();
    SLIST_INSERT_HEAD(&driver->port_list, port, list_entry);
    CDC_COMMON_EXIT_CRITICAL();

    // Newly claimed host endpoints start at DATA0; synchronize the device before polling.
    port->users++;
    xSemaphoreGive(driver->open_close_mutex);
    ret = cdc_common_reset_interface_endpoints(port, config_desc, cdc_info.data_intf);
    if (ret == ESP_OK && port->notif.claimed) {
        ret = cdc_common_reset_interface_endpoints(port, config_desc, cdc_info.notif_intf);
    }
    xSemaphoreTake(driver->open_close_mutex, portMAX_DELAY);
    port->users--;
    ESP_GOTO_ON_ERROR(ret, err_started, TAG, "Failed to synchronize CDC endpoints");
    ESP_GOTO_ON_FALSE(!port->to_close && !port->suspended, ESP_ERR_INVALID_STATE, err_started, TAG, "CDC device became unavailable during open");
    port->opening = false;
    port->data_cb = open_config->data_cb;
    port->event_cb = open_config->event_cb;
    port->user_arg = open_config->user_arg;

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
    // Failed opens transfer ownership to cleanup, even when cancellation is delayed.
    port->to_close = true;
    port->close_requested = true;
    port->close_waiters++;
    xSemaphoreGive(driver->open_close_mutex);
    usb_host_client_unblock(driver->client_hdl);
    cdc_common_wait_closed(driver, port, false);
    *port_ret = NULL;
    return ret;
err_claim:
    CDC_COMMON_ENTER_CRITICAL();
    SLIST_INSERT_HEAD(&driver->port_list, port, list_entry);
    CDC_COMMON_EXIT_CRITICAL();
    goto err_started;
err_port:
    cdc_common_transfers_free(port);
    if (port->close_event) {
        vEventGroupDelete(port->close_event);
    }
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

static esp_err_t cdc_common_close(cdc_host_common_port_handle_t port_hdl, bool wait)
{
    ESP_RETURN_ON_FALSE(p_cdc_common && port_hdl, ESP_ERR_INVALID_ARG, TAG, "invalid close argument");
    cdc_host_common_driver_t *driver = p_cdc_common;
    cdc_host_common_port_t *port = (cdc_host_common_port_t *)port_hdl;

    xSemaphoreTake(driver->open_close_mutex, portMAX_DELAY);

    if (!port_is_opened(port)) {
        xSemaphoreGive(driver->open_close_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    port->to_close = true;
    port->close_requested = true;
    wait = wait && xTaskGetCurrentTaskHandle() != driver->task_hdl;
    if (wait) {
        port->close_waiters++;
    }
    xSemaphoreGive(driver->open_close_mutex);
    usb_host_client_unblock(driver->client_hdl);
    return wait ? cdc_common_wait_closed(driver, port, true) : ESP_OK;
}

esp_err_t cdc_host_common_close(cdc_host_common_port_handle_t port_hdl)
{
    return cdc_common_close(port_hdl, true);
}

esp_err_t cdc_host_common_close_async(cdc_host_common_port_handle_t port_hdl)
{
    return cdc_common_close(port_hdl, false);
}

static esp_err_t cdc_common_tx_blocking(cdc_host_common_port_t *port, const uint8_t *data, size_t data_len, TickType_t ticks_to_wait)
{
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_FALSE(port->data.out_xfer, ESP_ERR_NOT_SUPPORTED, exit, TAG, "CDC port is read-only");
    ESP_GOTO_ON_FALSE(!port->data.tx_ringbuf, ESP_ERR_INVALID_STATE, exit, TAG, "TX ringbuffer is enabled");

    const size_t buffer_size = port->data.out_xfer->data_buffer_size;
    const uint8_t *data_ptr = data;
    size_t remaining = data_len;
    TickType_t start_ticks = xTaskGetTickCount();
    TickType_t timeout_ticks = ticks_to_wait;
    int remaining_timeout_ticks = timeout_ticks;

    BaseType_t taken = xSemaphoreTake(port->data.out_mux, remaining_timeout_ticks);
    if (taken != pdTRUE) {
        ret = ESP_ERR_TIMEOUT;
        goto exit;
    }

    ESP_GOTO_ON_FALSE(!port->data.tx_inflight, ESP_ERR_NOT_FINISHED, unblock, TAG, "Previous TX transfer is still pending");
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
        ESP_GOTO_ON_ERROR(cdc_common_submit(port, port->data.out_xfer, false), unblock, TAG, "BULK OUT submit failed");

        taken = xSemaphoreTake(port->data.out_done_sem, remaining_timeout_ticks);
        remaining_timeout_ticks = timeout_ticks - (xTaskGetTickCount() - start_ticks);
        if (!taken || remaining_timeout_ticks < 0) {
            // Closing owns endpoint cancellation; keep the transfer alive until its callback returns.
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

exit:
    return ret;
}

esp_err_t cdc_host_common_write_bytes(cdc_host_common_port_handle_t port_hdl, const uint8_t *data, size_t data_len, TickType_t ticks_to_wait)
{
    ESP_RETURN_ON_FALSE(port_hdl && data && data_len > 0, ESP_ERR_INVALID_ARG, TAG, "invalid write argument");
    cdc_host_common_port_t *port;
    esp_err_t ret = cdc_common_port_get(port_hdl, &port);
    ESP_RETURN_ON_ERROR(ret, TAG, "CDC port is unavailable");
    ESP_GOTO_ON_FALSE(port->data.out_xfer, ESP_ERR_NOT_SUPPORTED, exit, TAG, "CDC port is read-only");

    if (!port->data.tx_ringbuf) {
        TickType_t effective_ticks = (ticks_to_wait == portMAX_DELAY) ? pdMS_TO_TICKS(CDC_COMMON_TX_TIMEOUT_MS) : ticks_to_wait;
        ret = cdc_common_tx_blocking(port, data, data_len, effective_ticks);
        goto exit;
    }

    BaseType_t taken = xSemaphoreTake(port->data.out_mux, ticks_to_wait);
    if (taken != pdTRUE) {
        ret = ESP_ERR_TIMEOUT;
        goto exit;
    }

    ret = cdc_common_ringbuf_io(port, (uint8_t *)data, data_len, NULL, ticks_to_wait);
    ESP_GOTO_ON_ERROR(ret, unblock, TAG, "CDC TX ringbuffer push failed");

    if (xSemaphoreTake(port->data.out_done_sem, 0) == pdTRUE) {
        ret = cdc_common_tx_submit_next(port);
    }

unblock:
    xSemaphoreGive(port->data.out_mux);

exit:
    cdc_common_port_put(port);
    return ret;
}

esp_err_t cdc_host_common_read_bytes(cdc_host_common_port_handle_t port_hdl, uint8_t *buf, size_t *length, TickType_t ticks_to_wait)
{
    ESP_RETURN_ON_FALSE(length, ESP_ERR_INVALID_ARG, TAG, "length is NULL");
    if (!port_hdl || !buf) {
        *length = 0;
        return ESP_ERR_INVALID_ARG;
    }
    cdc_host_common_port_t *port;
    esp_err_t ret = cdc_common_port_get(port_hdl, &port);
    if (ret != ESP_OK) {
        *length = 0;
        return ret;
    }

    if (port->data.rx_ringbuf) {
        size_t data_len = *length;
        if (data_len > port->data.rx_ringbuf_size) {
            data_len = port->data.rx_ringbuf_size;
        }
        ret = cdc_common_ringbuf_io(port, buf, data_len, length, ticks_to_wait);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "CDC RX ringbuffer read failed");
            goto fail;
        }
        ret = ESP_OK;
        goto exit;
    }

    if (*length != port->data.current_data_len) {
        ESP_LOGE(TAG, "length is invalid, length = %u, actual_num_bytes = %u", (unsigned)*length, (unsigned)port->data.current_data_len);
        *length = port->data.current_data_len;
        ret = ESP_ERR_INVALID_ARG;
        goto exit;
    }
    if (!port->data.current_data || port->data.current_data_len == 0) {
        *length = 0;
        ret = ESP_ERR_INVALID_STATE;
        goto exit;
    }
    memcpy(buf, port->data.current_data, port->data.current_data_len);
    *length = port->data.current_data_len;
    ret = ESP_OK;
    goto exit;

fail:
    *length = 0;

exit:
    cdc_common_port_put(port);
    return ret;
}

esp_err_t cdc_host_common_flush_rx_buffer(cdc_host_common_port_handle_t port_hdl)
{
    ESP_RETURN_ON_FALSE(port_hdl, ESP_ERR_INVALID_ARG, TAG, "port is NULL");
    cdc_host_common_port_t *port;
    esp_err_t ret = cdc_common_port_get(port_hdl, &port);
    ESP_RETURN_ON_ERROR(ret, TAG, "CDC port is unavailable");
    ESP_GOTO_ON_FALSE(port->data.rx_ringbuf, ESP_ERR_NOT_SUPPORTED, exit, TAG, "RX ringbuffer is not created");
    cdc_common_ringbuf_flush(port->data.rx_ringbuf);
    ret = ESP_OK;

exit:
    cdc_common_port_put(port);
    return ret;
}

esp_err_t cdc_host_common_flush_tx_buffer(cdc_host_common_port_handle_t port_hdl)
{
    ESP_RETURN_ON_FALSE(port_hdl, ESP_ERR_INVALID_ARG, TAG, "port is NULL");
    cdc_host_common_port_t *port;
    esp_err_t ret = cdc_common_port_get(port_hdl, &port);
    ESP_RETURN_ON_ERROR(ret, TAG, "CDC port is unavailable");
    ESP_GOTO_ON_FALSE(port->data.tx_ringbuf, ESP_ERR_NOT_SUPPORTED, exit, TAG, "TX ringbuffer is not created");

    BaseType_t taken = xSemaphoreTake(port->data.out_mux, pdMS_TO_TICKS(CDC_COMMON_TX_TIMEOUT_MS));
    if (taken != pdTRUE) {
        ESP_LOGW(TAG, "CDC TX ringbuffer flush timeout");
        ret = ESP_ERR_TIMEOUT;
        goto exit;
    }
    cdc_common_ringbuf_flush(port->data.tx_ringbuf);
    xSemaphoreGive(port->data.out_mux);
    ret = ESP_OK;

exit:
    cdc_common_port_put(port);
    return ret;
}

esp_err_t cdc_host_common_get_rx_buffer_size(cdc_host_common_port_handle_t port_hdl, size_t *size)
{
    ESP_RETURN_ON_FALSE(port_hdl && size, ESP_ERR_INVALID_ARG, TAG, "invalid RX size argument");
    cdc_host_common_port_t *port;
    esp_err_t ret = cdc_common_port_get(port_hdl, &port);
    ESP_RETURN_ON_ERROR(ret, TAG, "CDC port is unavailable");
    if (port->data.rx_ringbuf) {
        *size = cdc_common_ringbuf_len(port->data.rx_ringbuf);
    } else {
        *size = port->data.current_data_len;
    }
    cdc_common_port_put(port);
    return ESP_OK;
}

esp_err_t cdc_host_common_send_control(cdc_host_common_port_handle_t port_hdl, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue,
                                       uint16_t wIndex, uint16_t wLength, uint8_t *data)
{
    ESP_RETURN_ON_FALSE(port_hdl, ESP_ERR_INVALID_ARG, TAG, "port is NULL");
    if (wLength > 0) {
        ESP_RETURN_ON_FALSE(data, ESP_ERR_INVALID_ARG, TAG, "data is NULL");
    }
    cdc_host_common_port_t *port;
    esp_err_t ret = cdc_common_port_get(port_hdl, &port);
    ESP_RETURN_ON_ERROR(ret, TAG, "CDC port is unavailable");
    ESP_GOTO_ON_FALSE(port->dev && port->dev->dev_hdl, ESP_ERR_INVALID_STATE, exit, TAG, "CDC device is not open");
    ESP_GOTO_ON_FALSE(wLength <= port->ctrl_data_size, ESP_ERR_INVALID_SIZE, exit, TAG, "Control payload is too large");

    BaseType_t taken = xSemaphoreTake(port->ctrl_mux, pdMS_TO_TICKS(CDC_COMMON_CTRL_TIMEOUT_MS));
    if (!taken) {
        ret = ESP_ERR_TIMEOUT;
        goto exit;
    }

    ESP_GOTO_ON_FALSE(!port->ctrl_inflight && !port->to_close, ESP_ERR_INVALID_STATE, unblock, TAG, "Control transfer is unavailable");
    xSemaphoreTake(port->ctrl_done_sem, 0);
    usb_setup_packet_t *req = (usb_setup_packet_t *)port->ctrl_xfer->data_buffer;
    uint8_t *start_of_data = (uint8_t *)req + sizeof(usb_setup_packet_t);
    req->bmRequestType = bmRequestType;
    req->bRequest = bRequest;
    req->wValue = wValue;
    req->wIndex = wIndex;
    req->wLength = wLength;

    const bool in_transfer = bmRequestType & USB_BM_REQUEST_TYPE_DIR_IN;
    if (!in_transfer && wLength) {
        memcpy(start_of_data, data, wLength);
    }

    port->ctrl_xfer->num_bytes = wLength + sizeof(usb_setup_packet_t);
    ESP_GOTO_ON_ERROR(cdc_common_submit(port, port->ctrl_xfer, true), unblock, TAG, "CTRL transfer failed");

    taken = xSemaphoreTake(port->ctrl_done_sem, pdMS_TO_TICKS(CDC_COMMON_CTRL_TIMEOUT_MS));
    if (!taken) {
        ret = ESP_ERR_TIMEOUT;
        goto unblock;
    }

    ESP_GOTO_ON_FALSE(port->ctrl_xfer->status == USB_TRANSFER_STATUS_COMPLETED, ESP_ERR_INVALID_RESPONSE, unblock, TAG, "Control transfer error");
    ESP_GOTO_ON_FALSE(port->ctrl_xfer->actual_num_bytes == port->ctrl_xfer->num_bytes, ESP_ERR_INVALID_RESPONSE, unblock, TAG, "Incorrect control transfer length");
    if (in_transfer && wLength) {
        memcpy(data, start_of_data, wLength);
    }
    ret = ESP_OK;

unblock:
    xSemaphoreGive(port->ctrl_mux);

exit:
    cdc_common_port_put(port);
    return ret;
}

esp_err_t cdc_host_common_get_rx_data(cdc_host_common_port_handle_t port_hdl, const uint8_t **data, size_t *data_len)
{
    ESP_RETURN_ON_FALSE(port_hdl && data && data_len, ESP_ERR_INVALID_ARG, TAG, "invalid RX data argument");
    cdc_host_common_port_t *port;
    esp_err_t ret = cdc_common_port_get(port_hdl, &port);
    ESP_RETURN_ON_ERROR(ret, TAG, "CDC port is unavailable");
    *data = port->data.current_data;
    *data_len = port->data.current_data_len;
    ret = (*data && *data_len) ? ESP_OK : ESP_ERR_INVALID_STATE;

    cdc_common_port_put(port);
    return ret;
}

esp_err_t cdc_host_common_get_dev_handle(cdc_host_common_port_handle_t port_hdl, usb_device_handle_t *dev_hdl)
{
    ESP_RETURN_ON_FALSE(port_hdl && dev_hdl, ESP_ERR_INVALID_ARG, TAG, "invalid dev handle argument");
    cdc_host_common_port_t *port;
    esp_err_t ret = cdc_common_port_get(port_hdl, &port);
    ESP_RETURN_ON_ERROR(ret, TAG, "CDC port is unavailable");
    ESP_GOTO_ON_FALSE(port->dev && port->dev->dev_hdl, ESP_ERR_INVALID_STATE, exit, TAG, "CDC device is not open");
    *dev_hdl = port->dev->dev_hdl;
    ret = ESP_OK;

exit:
    cdc_common_port_put(port);
    return ret;
}

esp_err_t cdc_host_common_get_intf_desc(cdc_host_common_port_handle_t port_hdl, const usb_intf_desc_t **notif_intf, const usb_intf_desc_t **data_intf)
{
    ESP_RETURN_ON_FALSE(port_hdl, ESP_ERR_INVALID_ARG, TAG, "port is NULL");
    cdc_host_common_port_t *port;
    esp_err_t ret = cdc_common_port_get(port_hdl, &port);
    ESP_RETURN_ON_ERROR(ret, TAG, "CDC port is unavailable");
    if (notif_intf) {
        *notif_intf = port->notif.intf_desc;
    }
    if (data_intf) {
        *data_intf = port->data.intf_desc;
    }
    cdc_common_port_put(port);
    return ESP_OK;
}

esp_err_t cdc_host_common_protocols_get(cdc_host_common_port_handle_t port_hdl, cdc_comm_protocol_t *comm, cdc_data_protocol_t *data)
{
    ESP_RETURN_ON_FALSE(port_hdl, ESP_ERR_INVALID_ARG, TAG, "port is NULL");
    cdc_host_common_port_t *port;
    esp_err_t ret = cdc_common_port_get(port_hdl, &port);
    ESP_RETURN_ON_ERROR(ret, TAG, "CDC port is unavailable");
    if (comm) {
        *comm = port->comm_protocol;
    }
    if (data) {
        *data = port->data_protocol;
    }
    cdc_common_port_put(port);
    return ESP_OK;
}

esp_err_t cdc_host_common_cdc_desc_get(cdc_host_common_port_handle_t port_hdl, cdc_desc_subtype_t desc_type, const usb_standard_desc_t **desc_out)
{
    ESP_RETURN_ON_FALSE(port_hdl && desc_out, ESP_ERR_INVALID_ARG, TAG, "invalid CDC desc argument");
    ESP_RETURN_ON_FALSE(desc_type < USB_CDC_DESC_SUBTYPE_MAX, ESP_ERR_INVALID_ARG, TAG, "invalid CDC desc subtype");
    cdc_host_common_port_t *port;
    esp_err_t ret = cdc_common_port_get(port_hdl, &port);
    ESP_RETURN_ON_ERROR(ret, TAG, "CDC port is unavailable");
    *desc_out = NULL;
    for (int i = 0; i < port->cdc_func_desc_cnt; i++) {
        const cdc_header_desc_t *_desc = (const cdc_header_desc_t *)((*(port->cdc_func_desc))[i]);
        if (_desc->bDescriptorSubtype == desc_type) {
            *desc_out = (const usb_standard_desc_t *)_desc;
            ret = ESP_OK;
            goto exit;
        }
    }
    ret = ESP_ERR_NOT_FOUND;

exit:
    cdc_common_port_put(port);
    return ret;
}

void cdc_host_common_desc_print(cdc_host_common_port_handle_t port_hdl)
{
    cdc_host_common_port_t *port;
    if (cdc_common_port_get(port_hdl, &port) != ESP_OK) {
        return;
    }
    const usb_device_desc_t *device_desc;
    const usb_config_desc_t *config_desc;
    if (usb_host_get_device_descriptor(port->dev->dev_hdl, &device_desc) == ESP_OK &&
            usb_host_get_active_config_descriptor(port->dev->dev_hdl, &config_desc) == ESP_OK) {
        usb_print_device_descriptor(device_desc);
        usb_print_config_descriptor(config_desc, cdc_host_common_print_desc);
    } else {
        ESP_LOGE(TAG, "Failed to get CDC device descriptors");
    }
    cdc_common_port_put(port);
}

#ifdef CDC_HOST_COMMON_REMOTE_WAKE_SUPPORTED
esp_err_t cdc_host_common_enable_remote_wakeup(cdc_host_common_port_handle_t port_hdl, bool enable)
{
    ESP_RETURN_ON_FALSE(port_hdl, ESP_ERR_INVALID_ARG, TAG, "port is NULL");
    cdc_host_common_port_t *port;
    esp_err_t ret = cdc_common_port_get(port_hdl, &port);
    ESP_RETURN_ON_ERROR(ret, TAG, "CDC port is unavailable");
    ESP_GOTO_ON_FALSE(port->dev && port->dev->dev_hdl, ESP_ERR_INVALID_STATE, exit, TAG, "CDC device is not open");

    const usb_config_desc_t *config_desc;
    ESP_GOTO_ON_ERROR(usb_host_get_active_config_descriptor(port->dev->dev_hdl, &config_desc), exit, TAG, "Unable to get configuration descriptor");
    ESP_GOTO_ON_FALSE((config_desc->bmAttributes & USB_BM_ATTRIBUTES_WAKEUP), ESP_ERR_NOT_SUPPORTED, exit, TAG, "Device does not support remote wakeup");

    if (port->dev->remote_wakeup_enabled == enable) {
        ret = ESP_OK;
        goto exit;
    }

    BaseType_t taken = xSemaphoreTake(port->ctrl_mux, pdMS_TO_TICKS(CDC_COMMON_CTRL_TIMEOUT_MS));
    if (!taken) {
        ret = ESP_ERR_TIMEOUT;
        goto exit;
    }

    ESP_GOTO_ON_FALSE(!port->ctrl_inflight && !port->to_close, ESP_ERR_INVALID_STATE, unlock, TAG, "Control transfer is unavailable");
    xSemaphoreTake(port->ctrl_done_sem, 0);
    usb_setup_packet_t *req = (usb_setup_packet_t *)port->ctrl_xfer->data_buffer;
    if (enable) {
        USB_SETUP_PACKET_INIT_SET_FEATURE(req, DEVICE_REMOTE_WAKEUP);
    } else {
        USB_SETUP_PACKET_INIT_CLEAR_FEATURE(req, DEVICE_REMOTE_WAKEUP);
    }
    port->ctrl_xfer->num_bytes = sizeof(usb_setup_packet_t);
    ESP_GOTO_ON_ERROR(cdc_common_submit(port, port->ctrl_xfer, true), unlock, TAG, "CTRL transfer failed");

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

exit:
    cdc_common_port_put(port);
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

        // Keep the device available while subscribers schedule their open requests.
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
            CDC_COMMON_ENTER_CRITICAL();
            cdc_host_common_dev_event_cb_t callback = cb->cb;
            void *user_arg = cb->user_arg;
            CDC_COMMON_EXIT_CRITICAL();
            if (callback) {
                callback(&event, user_arg);
            }
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
            CDC_COMMON_ENTER_CRITICAL();
            cdc_host_common_dev_event_cb_t callback = cb->cb;
            void *user_arg = cb->user_arg;
            CDC_COMMON_EXIT_CRITICAL();
            if (callback) {
                callback(&dev_event, user_arg);
            }
        }

        cdc_common_disconnect_removed_ports(p_cdc_common, dev_hdl, dev_addr);
        break;
    }
#ifdef CDC_HOST_COMMON_SUSPEND_RESUME_API_SUPPORTED
    case USB_HOST_CLIENT_EVENT_DEV_SUSPENDED:
    case USB_HOST_CLIENT_EVENT_DEV_RESUMED: {
        bool suspended = event_msg->event == USB_HOST_CLIENT_EVENT_DEV_SUSPENDED;
        while (true) {
            xSemaphoreTake(p_cdc_common->open_close_mutex, portMAX_DELAY);
            cdc_host_common_port_t *port;
            SLIST_FOREACH(port, &p_cdc_common->port_list, list_entry) {
                if (!port->to_close && port->dev && port->dev->dev_hdl == event_msg->dev_suspend_resume.dev_hdl && port->suspended != suspended) {
                    break;
                }
            }
            if (!port) {
                xSemaphoreGive(p_cdc_common->open_close_mutex);
                break;
            }
            if (suspended) {
                cdc_common_suspend_polling(port);
            } else {
                esp_err_t ret = cdc_common_resume_polling(port);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Resume polling failed: %s", esp_err_to_name(ret));
                }
            }
            xSemaphoreGive(p_cdc_common->open_close_mutex);
            cdc_host_common_port_event_data_t event = {
                .type = suspended ? CDC_HOST_COMMON_PORT_EVENT_SUSPENDED : CDC_HOST_COMMON_PORT_EVENT_RESUMED,
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
