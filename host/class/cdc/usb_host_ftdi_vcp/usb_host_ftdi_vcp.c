/*
 * SPDX-FileCopyrightText: 2022-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "usb/vcp_ftdi.h"
#include "usb/usb_types_ch9.h"
#include "esp_log.h"
#include "esp_check.h"
#include "usb/cdc_acm_host.h"
#include "esp_private/cdc_host_common.h"

#define FTDI_READ_REQ  (USB_BM_REQUEST_TYPE_TYPE_VENDOR | USB_BM_REQUEST_TYPE_DIR_IN)
#define FTDI_WRITE_REQ (USB_BM_REQUEST_TYPE_TYPE_VENDOR | USB_BM_REQUEST_TYPE_DIR_OUT)

#define FTDI_CMD_RESET        (0x00)
#define FTDI_CMD_SET_FLOW     (0x01)
#define FTDI_CMD_SET_MHS      (0x02) // Modem handshaking
#define FTDI_CMD_SET_BAUDRATE (0x03)
#define FTDI_CMD_SET_LINE_CTL (0x04)
#define FTDI_CMD_GET_MDMSTS   (0x05) // Modem status

#define FTDI_BASE_CLK (3000000)

static const char *TAG = "FTDI";

/**
 * @brief FTDI driver context structure
 */
typedef struct {
    uint8_t intf;                                    // Interface number
    cdc_acm_data_callback_t user_data_cb;            // User's data callback
    cdc_acm_host_dev_callback_t user_event_cb;       // User's event callback
    void *user_arg;                                  // User's argument
    uint16_t uart_state;                             // UART state tracking
} ftdi_ctx_t;

/**
 * @brief FT23x's RX data handler
 *
 * First two bytes are status bytes, the RX data start at data[2].
 * Coding of status bytes:
 * Byte 0:
 *      Bit 0: Full Speed packet
 *      Bit 1: High Speed packet
 *      Bit 4: CTS
 *      Bit 5: DSR
 *      Bit 6: RI
 *      Bit 7: DCD
 * Byte 1:
 *      Bit 1: RX overflow
 *      Bit 2: Parity error
 *      Bit 3: Framing error
 *      Bit 4: Break received
 *      Bit 5: Transmitter holding register empty
 *      Bit 6: Transmitter empty
 *
 * @param[in] data     Received data
 * @param[in] data_len Received data length
 * @param[in] user_arg Pointer to FTDI context
 * @return True if data was processed, false otherwise
 */
static bool ftdi_rx(const uint8_t *data, size_t data_len, void *user_arg)
{
    ftdi_ctx_t *ctx = (ftdi_ctx_t *)user_arg;

    // Dispatch serial state if it has changed
    if (ctx->user_event_cb && data_len >= 2) {
        cdc_acm_uart_state_t new_state;
        new_state.val = 0;
        new_state.bRxCarrier =   (data[0] & 0x80u) != 0; // DCD
        new_state.bTxCarrier =   (data[0] & 0x20u) != 0; // DSR
        new_state.bBreak =       (data[1] & 0x10u) != 0;
        new_state.bRingSignal =  (data[0] & 0x40u) != 0;
        new_state.bFraming =     (data[1] & 0x08u) != 0;
        new_state.bParity =      (data[1] & 0x04u) != 0;
        new_state.bOverRun =     (data[1] & 0x02u) != 0;
        new_state.bClearToSend = (data[0] & 0x10u) != 0; // CTS

        if (ctx->uart_state != new_state.val) {
            cdc_acm_host_dev_event_data_t serial_event;
            serial_event.type = CDC_ACM_HOST_SERIAL_STATE;
            serial_event.data.serial_state = new_state;
            ctx->user_event_cb(&serial_event, ctx->user_arg);
            ctx->uart_state = new_state.val;
        }
    }

    // Dispatch data if any
    if (ctx->user_data_cb && data_len > 2) {
        return ctx->user_data_cb(&data[2], data_len - 2, ctx->user_arg);
    }
    return true;
}

/**
 * @brief Event handler wrapper to recover user's argument
 *
 * @param[in] event    Event data
 * @param[in] user_ctx Pointer to FTDI context
 */
static void ftdi_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    ftdi_ctx_t *ctx = (ftdi_ctx_t *)user_ctx;
    assert(ctx->user_event_cb);
    ctx->user_event_cb(event, ctx->user_arg);
}

/**
 * @brief Calculate baudrate for FTDI devices
 *
 * A Baud rate for the FT232R, FT2232 (UART mode) or FT232B is generated using the chips
 * internal 48MHz clock. This is input to Baud rate generator circuitry where it is then divided by 16
 * and fed into a prescaler as a 3MHz reference clock. This 3MHz reference clock is then divided
 * down to provide the required Baud rate for the device's on chip UART. The value of the Baud rate
 * divisor is an integer plus a sub-integer prescaler.
 * Allowed values for the Baud rate divisor are:
 * Divisor = n + 0, 0.125, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875; where n is an integer between 2 and
 * 16384 (214).
 *
 * Note: Divisor = 1 and Divisor = 0 are special cases. A divisor of 0 will give 3 MBaud, and a divisor
 * of 1 will give 2 MBaud. Sub-integer divisors between 0 and 2 are not allowed.
 * Therefore the value of the divisor needed for a given Baud rate is found by dividing 3000000 by the
 * required Baud rate.
 *
 * @see FTDI AN232B-05 Configuring FT232R, FT2232 and FT232B Baud Rates
 * @param[in]  baudrate Baud rate to set
 * @param[out] wValue wValue for the FTDI_CMD_SET_BAUDRATE request
 * @param[out] wIndex wIndex for the FTDI_CMD_SET_BAUDRATE request
 * @return Real baud rate set
 */
static int calculate_baudrate(uint32_t baudrate, uint16_t *wValue, uint16_t *wIndex)
{
    int baudrate_real;
    if (baudrate > 2000000) {
        // set to 3000000
        *wValue = 0;
        *wIndex = 0;
        baudrate_real = 3000000;
    } else if (baudrate >= 1000000) {
        // set to 1000000
        *wValue = 1;
        *wIndex = 0;
        baudrate_real = 1000000;
    } else {
        const float ftdi_fractal[] = {0, 0.125, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875, 1};
        const uint8_t ftdi_fractal_bits[] = {0, 0x03, 0x02, 0x04, 0x01, 0x05, 0x06, 0x07};
        uint16_t divider_n = FTDI_BASE_CLK / baudrate; // integer value
        int ftdi_fractal_idx = 0;
        float divider = FTDI_BASE_CLK / (float)baudrate; // float value
        float divider_fractal = divider - (float)divider_n;

        // Find closest bigger FT23x fractal divider
        for (ftdi_fractal_idx = 0; ftdi_fractal[ftdi_fractal_idx] <= divider_fractal; ftdi_fractal_idx++) {};

        // Calculate baudrate errors for two closest fractal divisors
        int diff1 = baudrate - (int)(FTDI_BASE_CLK / (divider_n + ftdi_fractal[ftdi_fractal_idx]));     // Greater than required baudrate
        int diff2 = (int)(FTDI_BASE_CLK / (divider_n + ftdi_fractal[ftdi_fractal_idx - 1])) - baudrate; // Lesser than required baudrate

        // Chose divider and fractal divider with smallest error
        if (diff2 < diff1) {
            ftdi_fractal_idx--;
        } else {
            if (ftdi_fractal_idx == 8) {
                ftdi_fractal_idx = 0;
                divider_n++;
            }
        }

        baudrate_real = FTDI_BASE_CLK / (float)((float)divider_n + ftdi_fractal[ftdi_fractal_idx]);
        *wValue = ((0x3FFFF) & divider_n) | (ftdi_fractal_bits[ftdi_fractal_idx] << 14);
        *wIndex = ftdi_fractal_bits[ftdi_fractal_idx] >> 2;
    }
    ESP_LOGV(TAG, "wValue: 0x%04X wIndex: 0x%04X", *wValue, *wIndex);
    ESP_LOGD(TAG, "Baudrate required: %" PRIu32", set: %d", baudrate, baudrate_real);

    return baudrate_real;
}

// This is implementation of USB CDC-ACM compliant functions.
// It strictly follows interface defined in interface/usb/cdc_acm_host_interface.h
static esp_err_t ftdi_line_coding_set(cdc_acm_dev_hdl_t cdc_hdl, const cdc_acm_line_coding_t *line_coding)
{
    assert(line_coding);

    if (line_coding->dwDTERate != 0) {
        uint16_t wIndex, wValue;
        calculate_baudrate(line_coding->dwDTERate, &wValue, &wIndex);
        ESP_RETURN_ON_ERROR(cdc_acm_host_send_custom_request(cdc_hdl, FTDI_WRITE_REQ, FTDI_CMD_SET_BAUDRATE, wValue, wIndex, 0, NULL), TAG,);
    }

    if (line_coding->bDataBits != 0) {
        ftdi_ctx_t *ctx = (ftdi_ctx_t *)cdc_hdl->intf_func.user_data;
        const uint16_t wValue = (line_coding->bDataBits) | (line_coding->bParityType << 8) | (line_coding->bCharFormat << 11);
        return cdc_acm_host_send_custom_request(cdc_hdl, FTDI_WRITE_REQ, FTDI_CMD_SET_LINE_CTL, wValue, ctx->intf, 0, NULL);
    }
    return ESP_OK;
}

static esp_err_t ftdi_set_control_line_state(cdc_acm_dev_hdl_t cdc_hdl, bool dtr, bool rts)
{
    ftdi_ctx_t *ctx = (ftdi_ctx_t *)cdc_hdl->intf_func.user_data;
    ESP_RETURN_ON_ERROR(cdc_acm_host_send_custom_request(cdc_hdl, FTDI_WRITE_REQ, FTDI_CMD_SET_MHS, dtr ? 0x11 : 0x10, ctx->intf, 0, NULL), TAG,); // DTR
    return cdc_acm_host_send_custom_request(cdc_hdl, FTDI_WRITE_REQ, FTDI_CMD_SET_MHS, rts ? 0x21 : 0x20, ctx->intf, 0, NULL); // RTS
}

/**
 * @brief Cleanup function called when device is closed
 *
 * Frees the FTDI context structure allocated in ftdi_vcp_open()
 *
 * @param[in] cdc_hdl CDC device handle
 */
static void ftdi_del(cdc_acm_dev_hdl_t cdc_hdl)
{
    if (cdc_hdl && cdc_hdl->intf_func.user_data) {
        free(cdc_hdl->intf_func.user_data);
        cdc_hdl->intf_func.user_data = NULL;
    }
}

esp_err_t ftdi_vcp_open(uint16_t pid, uint8_t interface_idx, const cdc_acm_host_device_config_t *dev_config, cdc_acm_dev_hdl_t *cdc_hdl_ret)
{
    esp_err_t ret;
    cdc_acm_dev_hdl_t cdc_hdl = NULL;
    ESP_RETURN_ON_FALSE(cdc_hdl_ret && dev_config, ESP_ERR_INVALID_ARG, TAG, "Invalid arguments");

    // Allocate context structure to hold private data
    ftdi_ctx_t *ctx = (ftdi_ctx_t *)calloc(1, sizeof(ftdi_ctx_t));
    if (ctx == NULL) {
        return ESP_ERR_NO_MEM;
    }

    // Initialize context
    ctx->intf = interface_idx;
    ctx->user_data_cb = dev_config->data_cb;
    ctx->user_event_cb = dev_config->event_cb;
    ctx->user_arg = dev_config->user_arg;
    ctx->uart_state = 0;

    // FTDI chips report modem status in first two bytes of RX data
    // so we need to override the RX handler with our own.
    // user_arg is common for both data_cb and event_cb, so we must override event_cb as well.
    cdc_acm_host_device_config_t ftdi_config;
    memcpy(&ftdi_config, dev_config, sizeof(cdc_acm_host_device_config_t));
    ftdi_config.data_cb = ftdi_rx;
    ftdi_config.user_arg = ctx;
    if (dev_config->event_cb) {
        ftdi_config.event_cb = ftdi_event;
    }

    if (ftdi_config.in_buffer_size > 0) {
        ESP_LOGW(TAG, "RX FIFO size %d is not supported, setting to 0", ftdi_config.in_buffer_size);
        ftdi_config.in_buffer_size = 0;
    }

    if (pid == FTDI_PID_AUTO) {
        static const uint16_t supported_pids[] = {FT232_PID, FT231_PID};
        static const size_t num_pids = sizeof(supported_pids) / sizeof(supported_pids[0]);

        ret = ESP_ERR_NOT_FOUND;
        for (size_t i = 0; i < num_pids; i++) {
            ret = cdc_acm_host_open(FTDI_VID, supported_pids[i], interface_idx, &ftdi_config, &cdc_hdl);
            if (ret == ESP_OK) {
                break;
            }
        }
    } else {
        ret = cdc_acm_host_open(FTDI_VID, pid, interface_idx, &ftdi_config, &cdc_hdl);
    }

    if (ret != ESP_OK) {
        free(ctx);
        return ret;
    }

    // Set custom interface functions
    cdc_hdl->intf_func.user_data = ctx;
    cdc_hdl->intf_func.line_coding_set = ftdi_line_coding_set;
    cdc_hdl->intf_func.set_control_line_state = ftdi_set_control_line_state;
    cdc_hdl->intf_func.del = ftdi_del;

    // FT23x interface must be first reset and configured (115200 8N1)
    ret = cdc_acm_host_send_custom_request(cdc_hdl, FTDI_WRITE_REQ, FTDI_CMD_RESET, 0, interface_idx + 1, 0, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Reset failed");
        cdc_acm_host_close(cdc_hdl); // ctx is freed by cdc_acm_host_close
        return ret;
    }

    cdc_acm_line_coding_t line_coding = {
        .dwDTERate = 115200,
        .bCharFormat = 0,
        .bParityType = 0,
        .bDataBits = 8,
    };
    ret = ftdi_line_coding_set(cdc_hdl, &line_coding);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set line coding failed");
        cdc_acm_host_close(cdc_hdl); // ctx is freed by cdc_acm_host_close
        return ret;
    }

    *cdc_hdl_ret = cdc_hdl;
    return ret;
}
