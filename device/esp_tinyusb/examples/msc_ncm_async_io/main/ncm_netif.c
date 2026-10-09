/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

// lwIP network interface on top of USB NCM, adapted from ESP-IDF examples/network/sta2eth.
//
// The host sees a USB Ethernet adapter. The device runs a DHCP server on 192.168.4.1 and gives
// the host an address in 192.168.4.x.
//
// Both directions go through the TinyUSB task:
// - RX: tud_network_recv_cb() runs in the TinyUSB task and calls netif_recv_callback().
// - TX: tinyusb_net_send_sync() defers the send to the TinyUSB task and waits for it, so the
//   lwIP task waits while the TinyUSB task is busy.

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "tinyusb_net.h"
#include "dhcpserver/dhcpserver_options.h"
#include "lwip/esp_netif_net_stack.h"
#include "example.h"

#define TX_TIMEOUT_MS   100

static const char *TAG = "ncm_netif";
static esp_netif_t *s_netif;

static const esp_netif_ip_info_t s_ip_info = {
    .ip = { .addr = ESP_IP4TOADDR(192, 168, 4, 1) },
    .netmask = { .addr = ESP_IP4TOADDR(255, 255, 255, 0) },
    .gw = { .addr = ESP_IP4TOADDR(192, 168, 4, 1) },
};
static ncm_tx_stats_t s_tx_stats;

ncm_tx_stats_t ncm_netif_tx_stats(void)
{
    return s_tx_stats;
}

uint32_t ncm_netif_tx_drops(void)
{
    return s_tx_stats.busy_drops + s_tx_stats.timeout_drops;
}

static void l2_free(void *h, void *buffer)
{
    free(buffer);
}

static esp_err_t netif_transmit(void *h, void *buffer, size_t len)
{
    // tinyusb_net_send_sync() returns ESP_FAIL at once if all IN NTB buffers are in flight,
    // and ESP_ERR_TIMEOUT if the TinyUSB task did not take the frame in time (busy with storage).
    const int64_t start = esp_timer_get_time();
    esp_err_t ret;
    while (true) {
        ret = tinyusb_net_send_sync(buffer, len, NULL, pdMS_TO_TICKS(TX_TIMEOUT_MS));
#if CONFIG_EXAMPLE_NCM_TX_RETRY_BUSY
        // Wait for a free NTB instead of dropping the frame
        if (ret == ESP_FAIL && esp_timer_get_time() - start < TX_TIMEOUT_MS * 1000) {
            s_tx_stats.busy_retries++;
            vTaskDelay(1);
            continue;
        }
#endif
        break;
    }
    const uint32_t elapsed_us = (uint32_t)(esp_timer_get_time() - start);
    if (elapsed_us > s_tx_stats.max_send_us) {
        s_tx_stats.max_send_us = elapsed_us;
    }

    // lwIP retransmits TCP segments; count instead of logging every dropped frame
    if (ret == ESP_OK) {
        s_tx_stats.sent++;
    } else if (ret == ESP_FAIL) {
        s_tx_stats.busy_drops++;
    } else {
        s_tx_stats.timeout_drops++;
    }
    return ESP_OK;
}

static esp_err_t netif_recv_callback(void *buffer, uint16_t len, void *ctx)
{
    if (s_netif == NULL) {
        return ESP_OK;
    }
    void *buf_copy = malloc(len);
    if (buf_copy == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(buf_copy, buffer, len);
    return esp_netif_receive(s_netif, buf_copy, len, NULL);
}

esp_err_t ncm_netif_init(void)
{
    const tinyusb_net_config_t net_config = {
        // Locally administered MAC of the host side of the link
        .mac_addr = {0x02, 0x02, 0x11, 0x22, 0x33, 0x01},
        .on_recv_callback = netif_recv_callback,
    };
    esp_err_t ret = tinyusb_net_init(&net_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Cannot initialize USB NCM");
        return ret;
    }

    // The device side of the link needs a different MAC
    uint8_t lwip_mac[6] = {0x02, 0x02, 0x11, 0x22, 0x33, 0x02};

    esp_netif_inherent_config_t base_cfg = {
        .flags = ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP,
        .ip_info = &s_ip_info,
        .if_key = "usb_ncm",
        .if_desc = "usb ncm",
        .route_prio = 10,
    };
    esp_netif_driver_ifconfig_t driver_cfg = {
        .handle = (void *)1,                    // NCM is a singleton, but the handle must be != NULL
        .transmit = netif_transmit,
        .driver_free_rx_buffer = l2_free,
    };
    struct esp_netif_netstack_config lwip_netif_config = {
        .lwip = {
            .init_fn = ethernetif_init,
            .input_fn = ethernetif_input,
        },
    };
    esp_netif_config_t cfg = {
        .base = &base_cfg,
        .driver = &driver_cfg,
        .stack = &lwip_netif_config,
    };

    s_netif = esp_netif_new(&cfg);
    if (s_netif == NULL) {
        return ESP_FAIL;
    }
    esp_netif_set_mac(s_netif, lwip_mac);

    uint32_t lease_opt = 1; // minutes
    esp_netif_dhcps_option(s_netif, ESP_NETIF_OP_SET, IP_ADDRESS_LEASE_TIME, &lease_opt, sizeof(lease_opt));

    // The USB driver is already running, start the interface manually
    esp_netif_action_start(s_netif, 0, 0, 0);
    return ESP_OK;
}
