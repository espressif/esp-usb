/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

/**
 * @brief Malicious / malformed UVC configuration descriptors for security host_tests (BBP 573).
 *
 * Hand-crafted byte arrays: Video IAD without a following VC header, and a frame
 * descriptor whose bFrameIntervalType exceeds what bLength can cover.
 */
namespace malicious_uvc {

/**
 * Config ends with a Video Interface Collection IAD and no CS Interface VC HEADER.
 * Parser must return ESP_ERR_NOT_FOUND instead of NULL-dereferencing.
 *
 * Layout: Configuration (9) + IAD (8) = 17 bytes.
 */
const uint8_t cfg_iad_no_vc_header[] = {
    // Configuration descriptor
    0x09, 0x02, 0x11, 0x00, 0x00, 0x01, 0x00, 0x80, 0x32,
    // IAD: Video Interface Collection (bFunctionClass=0x0E, bFunctionSubClass=0x03)
    0x08, 0x0B, 0x00, 0x02, 0x0E, 0x03, 0x00, 0x00,
};

/**
 * Minimal UVC config with one MJPEG format/frame where bFrameIntervalType claims 255
 * discrete intervals but bLength only covers a single dwFrameInterval entry.
 *
 * Layout (wTotalLength = 9+8+9+13+9+14+11+30 = 103 = 0x67):
 *  Config, IAD, VC intf, VC header, VS intf, VS input header, MJPEG format, MJPEG frame
 */
const uint8_t cfg_huge_frame_interval_type[] = {
    // Configuration descriptor (wTotalLength = 0x0067)
    0x09, 0x02, 0x67, 0x00, 0x02, 0x01, 0x00, 0x80, 0x32,
    // IAD: first=0, count=2, Video Collection
    0x08, 0x0B, 0x00, 0x02, 0x0E, 0x03, 0x00, 0x00,
    // Interface 0 alt 0: VideoControl, 0 endpoints
    0x09, 0x04, 0x00, 0x00, 0x00, 0x0E, 0x01, 0x00, 0x00,
    // VC Header (CS_INTERFACE / HEADER), bcdUVC=1.00, wTotalLength=0x000D,
    // dwClockFrequency unused, bInCollection=1, baInterfaceNr[0]=1
    0x0D, 0x24, 0x01, 0x00, 0x01, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01,
    // Interface 1 alt 0: VideoStreaming, 0 endpoints (still image / probe path unused)
    0x09, 0x04, 0x01, 0x00, 0x00, 0x0E, 0x02, 0x00, 0x00,
    // VS Input Header: bNumFormats=1, wTotalLength=0x0037 (header+format+frame),
    // bEndpointAddress=0x81, bmInfo=0, bTerminalLink=1, bStillCaptureMethod=0,
    // bTriggerSupport=0, bTriggerUsage=0, bControlSize=1, bmaControls=0
    0x0E, 0x24, 0x01, 0x01, 0x37, 0x00, 0x81, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x00,
    // VS Format MJPEG: bFormatIndex=1, bNumFrameDescriptors=1, bmFlags=0,
    // bDefaultFrameIndex=1, aspect 0/0, interlace 0, copyprotect 0
    0x0B, 0x24, 0x06, 0x01, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
    // VS Frame MJPEG: bLength=30 (covers only ONE interval dword after type),
    // 640x480, bitrates, max buffer, default interval 333333 (30 fps),
    // bFrameIntervalType=0xFF (claims 255), then a single interval 333333
    0x1E, 0x24, 0x07, 0x01, 0x00, 0x80, 0x02, 0xE0, 0x01,
    0x00, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x0A, 0x00,
    0x00, 0x60, 0x09, 0x00, 0x15, 0x16, 0x05, 0x00,
    0xFF,
    0x15, 0x16, 0x05, 0x00,
};

} // namespace malicious_uvc
