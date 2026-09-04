/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "Mockusb_dwc_hal.h"
#include "hcd_test_helpers.h"

/**
 * @brief Mock callback for usb_dwc_hal_init_with_config()
 *
 * The real HAL init fills the constant config from read-only HW registers.
 * hcd_port_init() uses it to allocate the channel handles and to calculate the
 * default FIFO sizes, so the mocked HAL init must provide sane values.
 */
static void hal_init_with_config_cb(usb_dwc_hal_context_t *hal, int port_id, const usb_dwc_hal_config_t *config, int cmock_num_calls)
{
    hal->constant_config.chan_num_total = 8;    // Number of host channels
    hal->constant_config.hsphy_type = 1;        // HS PHY: 1024 line FIFO depth
    hal->constant_config.fifo_size = 1024;      // Total FIFO size in lines
}

/**
 * @brief Mock callback for usb_dwc_hal_deinit()
 */
static void hal_deinit_cb(usb_dwc_hal_context_t *hal, int cmock_num_calls)
{
}

void hcd_mock_register_hal_callbacks(void)
{
    usb_dwc_hal_init_with_config_StubWithCallback(hal_init_with_config_cb);
    usb_dwc_hal_deinit_StubWithCallback(hal_deinit_cb);
}
