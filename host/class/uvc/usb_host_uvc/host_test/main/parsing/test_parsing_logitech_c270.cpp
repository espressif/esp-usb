/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <catch2/catch_test_macros.hpp>

#include "usb/uvc_host.h"
#include "uvc_descriptors_priv.h"

#include "test_parsing_helpers.hpp"
#include "descriptors/logitech_c270.hpp"
using namespace logitech_c270;

SCENARIO("Camera descriptor parsing: Logitech C270", "[logitech][c270]")
{
    const usb_config_desc_t *cfg = (const usb_config_desc_t *)cfg_desc;

    GIVEN("Logitech C270 MJPEG") {
        uvc_host_stream_format_t formats[] = {
            FORMAT_MJPEG_30_5(640, 480),
            FORMAT_MJPEG_30_5(160, 120),
            FORMAT_MJPEG_30_5(176, 144),
            FORMAT_MJPEG_30_5(320, 176),
            FORMAT_MJPEG_30_5(320, 240),
            FORMAT_MJPEG_30_5(352, 288),
            FORMAT_MJPEG_30_5(432, 240),
            FORMAT_MJPEG_30_5(544, 288),
            FORMAT_MJPEG_30_5(640, 360),
            FORMAT_MJPEG_30_5(752, 416),
            FORMAT_MJPEG_30_5(800, 448),
            FORMAT_MJPEG_30_5(800, 600),
            FORMAT_MJPEG_30_5(864, 480),
            FORMAT_MJPEG_30_5(960, 544),
            FORMAT_MJPEG_30_5(960, 720),
            FORMAT_MJPEG_30_5(1024, 576),
            FORMAT_MJPEG_30_5(1184, 656),
            FORMAT_MJPEG_30_5(1280, 720),
            FORMAT_MJPEG_30_5(1280, 960),
        };

        for (uvc_host_stream_format_t this_format : formats) {
            SECTION(std::to_string(this_format.h_res) + "x" + std::to_string(this_format.v_res) + "@" + std::to_string(this_format.fps)) {
                REQUIRE_FORMAT_SUPPORTED(cfg, this_format, 1);
            }
        }
    }

    GIVEN("Logitech C270 Uncompressed") {
        uvc_host_stream_format_t formats[] = {
            FORMAT_UNCOMPRESSED_30_5(640, 480),
            FORMAT_UNCOMPRESSED_30_5(160, 120),
            FORMAT_UNCOMPRESSED_30_5(176, 144),
            FORMAT_UNCOMPRESSED_30_5(320, 176),
            FORMAT_UNCOMPRESSED_30_5(320, 240),
            FORMAT_UNCOMPRESSED_30_5(352, 288),
            FORMAT_UNCOMPRESSED_30_5(432, 240),
            FORMAT_UNCOMPRESSED_30_5(544, 288),
            FORMAT_UNCOMPRESSED_30_5(640, 360),
            {752, 416, 25, UVC_VS_FORMAT_YUY2},
            FORMAT_UNCOMPRESSED_20_5(752, 416),
            FORMAT_UNCOMPRESSED_20_5(800, 448),
            FORMAT_UNCOMPRESSED_20_5(800, 600),
            FORMAT_UNCOMPRESSED_20_5(864, 480),
            {960, 544, 15, UVC_VS_FORMAT_YUY2},
            {960, 544, 10, UVC_VS_FORMAT_YUY2},
            {960, 544, 5, UVC_VS_FORMAT_YUY2},
            {960, 544, 0, UVC_VS_FORMAT_YUY2},
            {960, 720, 10, UVC_VS_FORMAT_YUY2},
            {960, 720, 5, UVC_VS_FORMAT_YUY2},
            {960, 720, 0, UVC_VS_FORMAT_YUY2},
            {1024, 576, 10, UVC_VS_FORMAT_YUY2},
            {1024, 576, 5, UVC_VS_FORMAT_YUY2},
            {1024, 576, 0, UVC_VS_FORMAT_YUY2},
            {1184, 656, 10, UVC_VS_FORMAT_YUY2},
            {1184, 656, 5, UVC_VS_FORMAT_YUY2},
            {1184, 656, 0, UVC_VS_FORMAT_YUY2},
            {1280, 720, 7.5f, UVC_VS_FORMAT_YUY2},
            {1280, 720, 5, UVC_VS_FORMAT_YUY2},
            {1280, 720, 0, UVC_VS_FORMAT_YUY2},
            {1280, 960, 7.5f, UVC_VS_FORMAT_YUY2},
            {1280, 960, 5, UVC_VS_FORMAT_YUY2},
            {1280, 960, 0, UVC_VS_FORMAT_YUY2},
        };

        for (uvc_host_stream_format_t this_format : formats) {
            SECTION(std::to_string(this_format.h_res) + "x" + std::to_string(this_format.v_res) + "@" + std::to_string(this_format.fps)) {
                REQUIRE_FORMAT_SUPPORTED(cfg, this_format, 1);
            }
        }
    }

    GIVEN("Default format") {
        REQUIRE_FORMAT_DEFAULT(cfg, 1);
    }

    GIVEN("Logitech C270 Unsupported") {
        uvc_host_stream_format_t formats[] = {
            {640, 480, 28, UVC_VS_FORMAT_MJPEG}, // Invalid FPS
            {645, 480, 25, UVC_VS_FORMAT_MJPEG}, // Invalid definition
            {640, 480, 20, UVC_VS_FORMAT_H264},  // Invalid format
        };

        for (uvc_host_stream_format_t this_format : formats) {
            SECTION(std::to_string(this_format.h_res) + "x" + std::to_string(this_format.v_res) + "@" + std::to_string(this_format.fps)) {
                REQUIRE_FORMAT_NOT_SUPPORTED(cfg, this_format);
            }
        }
    }

    GIVEN("Various MPS are requested") {
        // This test checks if correct alternate interface is selected based on the requested MPS
        const usb_intf_desc_t *intf_desc = nullptr;
        const usb_ep_desc_t *ep_desc = nullptr;

        // Alternate interface 1 offers MPS = 192
        REQUIRE_STREAMING_INTF_AND_EP(cfg, 1, 192, 1, 192);
        // Alternate interface 2 offers MPS = 384
        REQUIRE_STREAMING_INTF_AND_EP(cfg, 1, 384, 2, 384);
        // Alternate interface 3 offers MPS = 512
        REQUIRE_STREAMING_INTF_AND_EP(cfg, 1, 512, 3, 512);
        // Alternate interface 4 offers MPS = 640
        REQUIRE_STREAMING_INTF_AND_EP(cfg, 1, 640, 4, 640);
        // Alternate interface 5 offers MPS = 800
        REQUIRE_STREAMING_INTF_AND_EP(cfg, 1, 800, 5, 800);
        // Alternate interface 6 offers MPS = 944
        REQUIRE_STREAMING_INTF_AND_EP(cfg, 1, 944, 6, 944);
        // Alternate interface 7 offers MPS = 1280
        REQUIRE_STREAMING_INTF_AND_EP(cfg, 1, 1280, 7, 1280);
        // Alternate interface 8 offers MPS = 1600
        REQUIRE_STREAMING_INTF_AND_EP(cfg, 1, 1600, 8, 1600);
        // Alternate interface 9 offers MPS = 1984
        REQUIRE_STREAMING_INTF_AND_EP(cfg, 1, 1984, 9, 1984);
        // Alternate interface 10 offers MPS = 2688
        REQUIRE_STREAMING_INTF_AND_EP(cfg, 1, 2688, 10, 2688);
        // Alternate interface 11 offers MPS = 3060
        REQUIRE_STREAMING_INTF_AND_EP(cfg, 1, 3060, 11, 3060);

        /************** Special cases **************/
        // 1. Requested MPS is not exactly available, so we should get the closest one.
        REQUIRE_STREAMING_INTF_AND_EP(cfg, 1, 400, 2, 384);

        // 2. Requested MPS is too small, we should get not found error.
        REQUIRE(ESP_ERR_NOT_FOUND == uvc_desc_get_streaming_intf_and_ep(cfg, 1, 128, &intf_desc, &ep_desc));
    }
}
