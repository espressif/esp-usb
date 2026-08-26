/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

/**
 * @brief Malicious UAC configuration with Feature Unit bControlSize == 0 (BBP 575).
 *
 * Exercised via print_uac_descriptors() which walks Feature Unit descriptors and
 * previously divided by bControlSize unconditionally.
 */
namespace malicious_uac {

/**
 * Minimal Audio Control config containing a Feature Unit with bControlSize = 0.
 *
 * Layout: Config (9) + AC Interface (9) + AC Header (8) + Feature Unit (7) = 33 = 0x21
 */
const uint8_t cfg_feature_unit_bcontrolsize_0[] = {
    // Configuration descriptor
    0x09, 0x02, 0x21, 0x00, 0x01, 0x01, 0x00, 0x80, 0x32,
    // Interface 0 alt 0: AudioControl, 0 endpoints
    0x09, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,
    // AC Header: bLength=8, CS_INTERFACE, HEADER, bcdADC=1.00, wTotalLength=15, bInCollection=0
    0x08, 0x24, 0x01, 0x00, 0x01, 0x0F, 0x00, 0x00,
    // Feature Unit: bLength=7, CS_INTERFACE, FEATURE_UNIT=0x06, bUnitID=5, bSourceID=1, bControlSize=0, iFeature=0
    0x07, 0x24, 0x06, 0x05, 0x01, 0x00, 0x00,
};

} // namespace malicious_uac
