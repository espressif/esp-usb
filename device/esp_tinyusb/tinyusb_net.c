/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <assert.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb_net.h"
#include "descriptors_control.h"
#include "usb_descriptors.h"
#include "device/usbd_pvt.h"
#include "esp_check.h"

#define MAC_ADDR_LEN 6

/** Yield / 1 ms delay cadence while waiting for `tud_network_can_xmit` (caller task only). */
#define TUSB_NET_SYNC_SPIN_DELAY_EVERY 3

typedef struct packet {
    void *buffer;
    void *buff_free_arg;
    uint16_t len;
    esp_err_t result;
} packet_t;

typedef struct sync_tx_job {
    packet_t packet;
    SemaphoreHandle_t done;
} sync_tx_job_t;

struct tinyusb_net_handle {
    bool initialized;
    SemaphoreHandle_t sync_mutex;
    SemaphoreHandle_t sync_done_sem;
    tusb_net_rx_cb_t    rx_cb;
    tusb_net_free_tx_cb_t tx_buff_free_cb;
    tusb_net_init_cb_t init_cb;
    char mac_str[2 * MAC_ADDR_LEN + 1];
    void *ctx;
};

static struct tinyusb_net_handle s_net_obj = { };
static const char *TAG = "tusb_net";

/**
 * @brief Wait until TinyUSB reports TX space for this length, or RTOS tick deadline expires.
 *
 * Wait until TinyUSB reports TX space for this length, or RTOS tick deadline expires.
 * Caller task only. @p pxTicksToWait is the remaining budget for the entire send_sync call.
 * At least one @c tud_network_can_xmit check runs before @c xTaskCheckForTimeOut is evaluated
 * in the @c while (so @c timeout==0 performs a single attempt). Loop continues while the
 * deadline has not expired.

 * @param[in]    len Packet length in bytes.
 * @param[in]    pxTimeOut Pointer to the time out structure.
 * @param[inout] pxTicksToWait Pointer to the remaining budget for the entire send_sync call.
 * @return true if TX space is available, false if the deadline expires.
 */
static bool tinyusb_net_wait_tx_ready(uint16_t len, TimeOut_t *pxTimeOut, TickType_t *pxTicksToWait)
{
    unsigned spin = 0;
    do {
        if (tud_network_can_xmit(len)) {
            return true;
        }
        if (spin % TUSB_NET_SYNC_SPIN_DELAY_EVERY == (TUSB_NET_SYNC_SPIN_DELAY_EVERY - 1)) {
            const TickType_t d = pdMS_TO_TICKS(1);
            vTaskDelay(d > 0 ? d : 1);
        } else {
            taskYIELD();
        }
        spin++;
    } while (xTaskCheckForTimeOut(pxTimeOut, pxTicksToWait) == pdFALSE);
    return false;
}

static void do_send_sync(void *ctx)
{
    sync_tx_job_t *job = (sync_tx_job_t *)ctx;

    assert(tud_network_can_xmit(job->packet.len));
    tud_network_xmit(&job->packet, job->packet.len);
    job->packet.result = ESP_OK;
    xSemaphoreGive(job->done);
}

static void do_send_async(void *ctx)
{
    packet_t *packet = ctx;
    if (tud_network_can_xmit(packet->len)) {
        tud_network_xmit(packet, packet->len);
    } else if (s_net_obj.tx_buff_free_cb) {
        ESP_LOGW(TAG, "Packet cannot be accepted on USB interface, dropping");
        s_net_obj.tx_buff_free_cb(packet->buff_free_arg, s_net_obj.ctx);
    }
    free(packet);
}

esp_err_t tinyusb_net_send_async(void *buffer, uint16_t len, void *buff_free_arg)
{
    if (!tud_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    packet_t *packet = calloc(1, sizeof(packet_t));
    packet->len = len;
    packet->buffer = buffer;
    packet->buff_free_arg = buff_free_arg;
    ESP_RETURN_ON_FALSE(packet, ESP_ERR_NO_MEM, TAG, "Failed to allocate packet to send");
    usbd_defer_func(do_send_async, packet, false);
    return ESP_OK;
}

esp_err_t tinyusb_net_send_sync(void *buffer, uint16_t len, void *buff_free_arg, TickType_t  timeout)
{
    if (!tud_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    // Lazy init the flags and semaphores, as they might not be needed (if async approach is used)
    if (!s_net_obj.sync_mutex) {
        s_net_obj.sync_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_net_obj.sync_mutex, ESP_ERR_NO_MEM, TAG, "Failed to allocate sync mutex");
    }
    if (!s_net_obj.sync_done_sem) {
        s_net_obj.sync_done_sem = xSemaphoreCreateBinary();
        ESP_RETURN_ON_FALSE(s_net_obj.sync_done_sem, ESP_ERR_NO_MEM, TAG, "Failed to allocate sync done semaphore");
    }

    TimeOut_t time_out;
    TickType_t ticks_remaining = timeout;
    vTaskSetTimeOutState(&time_out);

    if (!tinyusb_net_wait_tx_ready(len, &time_out, &ticks_remaining)) {
        return ESP_ERR_TIMEOUT;
    }
    if (xSemaphoreTake(s_net_obj.sync_mutex, ticks_remaining) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    (void)xTaskCheckForTimeOut(&time_out, &ticks_remaining);

    // Defensive programming: make sure the semaphore is not given by another task.
    while (xSemaphoreTake(s_net_obj.sync_done_sem, 0) == pdTRUE);

    sync_tx_job_t job = {
        .packet = {
            .buffer = buffer,
            .len = len,
            .buff_free_arg = buff_free_arg,
            .result = ESP_OK,
        },
        .done = s_net_obj.sync_done_sem,
    };

    usbd_defer_func(do_send_sync, &job, false);

    BaseType_t waited = xSemaphoreTake(job.done, ticks_remaining);
    if (waited != pdTRUE) {
        (void)xSemaphoreTake(job.done, portMAX_DELAY);
        xSemaphoreGive(s_net_obj.sync_mutex);
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t out = job.packet.result;
    xSemaphoreGive(s_net_obj.sync_mutex);
    return out;
}

esp_err_t tinyusb_net_init(const tinyusb_net_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(s_net_obj.initialized == false, ESP_ERR_INVALID_STATE, TAG, "TinyUSB Net class is already initialized");

    s_net_obj.rx_cb = cfg->on_recv_callback;
    s_net_obj.init_cb = cfg->on_init_callback;
    s_net_obj.tx_buff_free_cb = cfg->free_tx_buffer;
    s_net_obj.ctx = cfg->user_context;

    const uint8_t *mac = &cfg->mac_addr[0];
    snprintf(s_net_obj.mac_str, sizeof(s_net_obj.mac_str), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    uint8_t mac_id = tusb_get_mac_string_id();
    tinyusb_descriptors_set_string(s_net_obj.mac_str, mac_id);

    s_net_obj.initialized = true;

    return ESP_OK;
}

void tinyusb_net_deinit(void)
{
    if (s_net_obj.sync_done_sem) {
        vSemaphoreDelete(s_net_obj.sync_done_sem);
        s_net_obj.sync_done_sem = NULL;
    }
    if (s_net_obj.sync_mutex) {
        vSemaphoreDelete(s_net_obj.sync_mutex);
        s_net_obj.sync_mutex = NULL;
    }
    s_net_obj.initialized = false;
    s_net_obj.rx_cb = NULL;
    s_net_obj.init_cb = NULL;
    s_net_obj.tx_buff_free_cb = NULL;
    s_net_obj.ctx = NULL;
    memset(s_net_obj.mac_str, 0, sizeof(s_net_obj.mac_str));
}

//--------------------------------------------------------------------+
// tinyusb callbacks
//--------------------------------------------------------------------+
bool tud_network_recv_cb(const uint8_t *src, uint16_t size)
{
    if (s_net_obj.rx_cb) {
        s_net_obj.rx_cb((void *)src, size, s_net_obj.ctx);
    }
    tud_network_recv_renew();
    return true;
}

uint16_t tud_network_xmit_cb(uint8_t *dst, void *ref, uint16_t arg)
{
    packet_t *packet = ref;
    uint16_t len = arg;

    memcpy(dst, packet->buffer, packet->len);
    if (s_net_obj.tx_buff_free_cb) {
        s_net_obj.tx_buff_free_cb(packet->buff_free_arg, s_net_obj.ctx);
    }
    return len;
}

void tud_network_init_cb(void)
{
    if (s_net_obj.init_cb) {
        s_net_obj.init_cb(s_net_obj.ctx);
    }
}
