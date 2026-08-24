/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <catch2/catch_test_macros.hpp>

#include "usb/uac.h"
#include "descriptors/malicious_uac.hpp"

using namespace malicious_uac;

SCENARIO("UAC descriptor print tolerates Feature Unit bControlSize==0 (BBP 575)", "[security][uac]")
{
    GIVEN("Config with Feature Unit bControlSize = 0") {
        const usb_config_desc_t *cfg = (const usb_config_desc_t *)cfg_feature_unit_bcontrolsize_0;

        SECTION("print_uac_descriptors does not divide-by-zero") {
            // Must return (and not abort) when walking the malicious Feature Unit.
            REQUIRE_NOTHROW(print_uac_descriptors(cfg));
        }
    }
}
