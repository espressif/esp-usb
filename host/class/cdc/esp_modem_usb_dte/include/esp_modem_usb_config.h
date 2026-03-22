/*
 * SPDX-FileCopyrightText: 2022-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief USB modem terminal configuration.
 *
 * @see USB host CDC-ACM driver documentation for interface selection details.
 */
struct esp_modem_usb_term_config {
    uint16_t vid;                /*!< USB vendor ID. */
    uint16_t pid;                /*!< USB product ID. */
    int interface_idx;           /*!< USB interface index used for the primary AT terminal. */
    int secondary_interface_idx; /*!< USB interface index used for the secondary data terminal.
                                      Set to -1 for modems with a single AT port. */
    uint32_t timeout_ms;         /*!< Time in milliseconds to wait for the modem to connect.
                                      Set to 0 to wait forever. */
    int xCoreID;                 /*!< Core affinity for the created tasks, including the CDC-ACM driver task and the
                                      optional USB Host task. */
    bool cdc_compliant __attribute__((deprecated("Deprecated: CDC compliance is auto-detected"))); /*!< Deprecated. */
    bool install_usb_host;       /*!< Set to true to install the USB Host driver automatically. */
};

/**
 * @brief Build a default DTE configuration that references a USB terminal
 *        configuration.
 *
 * @param[in] _usb_config `struct esp_modem_usb_term_config` variable, such as
 *                        one created with ESP_MODEM_DEFAULT_USB_CONFIG().
 *
 */
#define ESP_MODEM_DTE_DEFAULT_USB_CONFIG(_usb_config) \
    {                                                 \
        .dte_buffer_size = 512,                       \
        .task_stack_size = 4096,                      \
        .task_priority = 5,                           \
        .extension_config = &_usb_config              \
    }

/**
 * @brief Build a default USB modem terminal configuration for dual-port
 *        devices.
 *
 * @param[in] _vid USB vendor ID.
 * @param[in] _pid USB product ID.
 * @param[in] _intf USB interface index of the primary AT port.
 * @param[in] _intf2 USB interface index of the secondary data port.
 *
 * @see USB host CDC-ACM driver documentation for interface selection details.
 */
#define ESP_MODEM_DEFAULT_USB_CONFIG_DUAL(_vid, _pid, _intf, _intf2) \
    {                                                                \
        .vid = _vid,                                                 \
        .pid = _pid,                                                 \
        .interface_idx = _intf,                                      \
        .secondary_interface_idx = _intf2,                           \
        .timeout_ms = 0,                                             \
        .xCoreID = 0,                                                \
        .cdc_compliant = false,                                      \
        .install_usb_host = true                                     \
    }

/**
 * @brief Build a default USB modem terminal configuration for single-port
 *        devices.
 *
 * @param[in] _vid USB vendor ID.
 * @param[in] _pid USB product ID.
 * @param[in] _intf USB interface index of the primary AT port.
 */
#define ESP_MODEM_DEFAULT_USB_CONFIG(_vid, _pid, _intf) ESP_MODEM_DEFAULT_USB_CONFIG_DUAL(_vid, _pid, _intf, -1)

/**
 * @brief Default USB configuration for the Quectel BG96 modem.
 */
#define ESP_MODEM_BG96_USB_CONFIG()    ESP_MODEM_DEFAULT_USB_CONFIG(0x2C7C, 0x0296, 2)

/**
 * @brief Default USB configuration for the SimCom SIM7600E modem.
 */
#define ESP_MODEM_SIM7600_USB_CONFIG() ESP_MODEM_DEFAULT_USB_CONFIG(0x1E0E, 0x9001, 3)

/**
 * @brief Default USB configuration for the SimCom A7670E modem.
 */
#define ESP_MODEM_A7670_USB_CONFIG()   ESP_MODEM_DEFAULT_USB_CONFIG_DUAL(0x1E0E, 0x9011, 4, 5)

/**
 * @brief Default USB configuration for the SimCom SIM7070G modem.
 */
#define ESP_MODEM_SIM7070G_USB_CONFIG()   ESP_MODEM_DEFAULT_USB_CONFIG(0x1E0E, 0x9206, 2)

/**
 * @brief Default USB configuration for the SimCom SIM7080 modem.
 */
#define ESP_MODEM_SIM7080_USB_CONFIG()   ESP_MODEM_DEFAULT_USB_CONFIG(0x1E0E, 0x9205, 2)

/**
 * @brief Default USB configuration for the SimCom SIMA7672E modem.
 */
#define ESP_MODEM_SIMA7672E_USB_CONFIG()   ESP_MODEM_DEFAULT_USB_CONFIG_DUAL(0x1E0E, 0x9011, 4, 5)

/**
 * @brief Default USB configuration for the Quectel EC20 modem.
 */
#define ESP_MODEM_EC20_USB_CONFIG()    ESP_MODEM_DEFAULT_USB_CONFIG(0x2C7C, 0x0125, 3)
