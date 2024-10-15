/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <catch2/catch_test_macros.hpp>

#include "usb/uvc_host.h"
#include "uvc_descriptors_priv.h"

#include "descriptors/logitech_c270.hpp"
using namespace logitech_c270;

SCENARIO("Camera descriptor parsing: Logitech C270", "[logitech][c270]")
{
    GIVEN("Logitech C270") {
        const usb_device_desc_t *dev = (const usb_device_desc_t *)dev_desc;
        const usb_config_desc_t *cfg = (const usb_config_desc_t *)cfg_desc;

        REQUIRE(dev != nullptr);
        REQUIRE(cfg != nullptr);
    }
}
