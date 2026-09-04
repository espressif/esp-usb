/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Dummy USB-DWC peripheral info for Linux host-based HCD layer mocking.
 *
 * On real chips, usb_dwc_info is defined in the target-specific
 * usb_dwc_periph.c (esp_hal_usb component) with GPIO matrix signal indexes
 * and interrupt sources. Those do not exist on the Linux target, so this
 * dummy definition is provided instead. Interrupt and PHY related values
 * have no meaning in the mock.
 */

#include "soc/usb_periph.h"

const usb_dwc_info_t usb_dwc_info = {};
