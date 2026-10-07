/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

// Servers the host measures against:
// - UDP echo on port 7: the host measures round-trip latency.
// - TCP sink on port 5001: reads and discards everything, the host measures upload throughput.
//   Also works as a server for an iperf2 client (iperf -c 192.168.4.1).
// - TCP source on port 5002: sends zeros until the host closes, the host measures download throughput.
// - HTTP on port 80: GET /status returns the async IO mode and counters as JSON.

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include "example.h"

#define UDP_ECHO_PORT   7
#define TCP_SINK_PORT   5001
#define TCP_SOURCE_PORT 5002
#define TCP_CHUNK       4096

#if CONFIG_EXAMPLE_NCM_TX_RETRY_BUSY
#define TX_RETRY_BUSY   1
#else
#define TX_RETRY_BUSY   0
#endif

static const char *TAG = "net_servers";

static void udp_echo_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(UDP_ECHO_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "UDP echo: socket/bind failed");
        vTaskDelete(NULL);
    }
    uint8_t buf[256];
    while (true) {
        struct sockaddr_storage from;
        socklen_t from_len = sizeof(from);
        const int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (len > 0) {
            sendto(sock, buf, len, 0, (struct sockaddr *)&from, from_len);
        }
    }
}

static int tcp_listen(uint16_t port)
{
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        return -1;
    }
    const int on = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(sock, 1) != 0) {
        close(sock);
        return -1;
    }
    return sock;
}

static void tcp_server_task(void *arg)
{
    const bool is_source = (bool)arg;
    const uint16_t port = is_source ? TCP_SOURCE_PORT : TCP_SINK_PORT;
    static uint8_t bufs[2][TCP_CHUNK]; // one per task, zeros for the source
    uint8_t *buf = bufs[is_source];

    const int listen_sock = tcp_listen(port);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "TCP %d: socket/bind/listen failed", port);
        vTaskDelete(NULL);
    }
    while (true) {
        const int sock = accept(listen_sock, NULL, NULL);
        if (sock < 0) {
            continue;
        }
        uint64_t total = 0;
        int len;
        do {
            len = is_source ? send(sock, buf, TCP_CHUNK, 0) : recv(sock, buf, TCP_CHUNK, 0);
            if (len > 0) {
                total += len;
            }
        } while (len > 0);
        ESP_LOGI(TAG, "TCP %s on port %d done: %.1f MB", is_source ? "source" : "sink", port, total / 1e6);
        close(sock);
    }
}

static esp_err_t status_handler(httpd_req_t *req)
{
    const io_bytes_t io = io_bytes_get();
    const ncm_tx_stats_t tx = ncm_netif_tx_stats();
    char json[384];
    const int len = snprintf(json, sizeof(json),
                             "{\"async_io\":\"%s\",\"storage\":\"%s\",\"medium_delay_ms\":%d,"
                             "\"msc_read\":%" PRIu64 ",\"msc_written\":%" PRIu64 ",\"ncm_tx_drops\":%" PRIu32 ","
                             "\"ncm_tx_sent\":%" PRIu32 ",\"ncm_tx_busy_drops\":%" PRIu32 ",\"ncm_tx_timeout_drops\":%" PRIu32 ","
                             "\"ncm_tx_busy_retries\":%" PRIu32 ",\"ncm_tx_max_send_us\":%" PRIu32 ",\"ncm_tx_retry_busy\":%d}",
                             IO_MODE_STR, STORAGE_STR, CONFIG_EXAMPLE_MEDIUM_DELAY_MS,
                             io.read, io.written, ncm_netif_tx_drops(),
                             tx.sent, tx.busy_drops, tx.timeout_drops, tx.busy_retries, tx.max_send_us,
                             TX_RETRY_BUSY);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, len);
}

void net_servers_start(void)
{
    xTaskCreate(udp_echo_task, "udp_echo", 3072, NULL, 5, NULL);
    xTaskCreate(tcp_server_task, "tcp_sink", 3072, (void *)false, 5, NULL);
    xTaskCreate(tcp_server_task, "tcp_source", 3072, (void *)true, 5, NULL);

    httpd_handle_t server = NULL;
    const httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(httpd_start(&server, &config));
    const httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_handler,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status_uri));
}
