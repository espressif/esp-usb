/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <catch2/catch_test_macros.hpp>

#include "usb/uvc_host.h"
#include "uvc_descriptors_priv.h"

#include "descriptors/malicious_uvc.hpp"

using namespace malicious_uvc;

SCENARIO("UVC descriptor parsing rejects malicious configs (BBP 573)", "[security][uvc]")
{
    GIVEN("Video IAD with no trailing VC Header") {
        const usb_config_desc_t *cfg = (const usb_config_desc_t *)cfg_iad_no_vc_header;

        SECTION("uvc_desc_get_frame_list returns NOT_FOUND without crashing") {
            size_t list_size = 0;
            REQUIRE(ESP_ERR_NOT_FOUND == uvc_desc_get_frame_list(cfg, 0, nullptr, &list_size));
        }

        SECTION("uvc_desc_get_streaming_interface_num returns NOT_FOUND without crashing") {
            uvc_host_stream_format_t format = {};
            format.format = UVC_VS_FORMAT_DEFAULT;
            uint8_t bInterfaceNumber = 0;
            uint16_t bcdUVC = 0;
            REQUIRE(ESP_ERR_NOT_FOUND ==
                    uvc_desc_get_streaming_interface_num(cfg, 0, &format, &bcdUVC, &bInterfaceNumber));
        }
    }

    GIVEN("Frame descriptor with bFrameIntervalType larger than bLength allows") {
        const usb_config_desc_t *cfg = (const usb_config_desc_t *)cfg_huge_frame_interval_type;

        SECTION("uvc_desc_get_frame_list clamps interval_type and does not over-read") {
            uvc_host_frame_info_t frames[4] = {};
            size_t list_size = 4;
            REQUIRE(ESP_OK == uvc_desc_get_frame_list(cfg, 0, &frames, &list_size));
            REQUIRE(list_size >= 1);
            // Declared 255 intervals, but only one dword fits in bLength
            REQUIRE(frames[0].interval_type == 1);
            REQUIRE(frames[0].h_res == 640);
            REQUIRE(frames[0].v_res == 480);
        }

        SECTION("uvc_desc_get_frame_format_by_format finds 640x480@30 without over-read") {
            uvc_host_stream_format_t format = {
                .h_res = 640,
                .v_res = 480,
                .fps = 30,
                .format = UVC_VS_FORMAT_MJPEG,
            };
            const uvc_format_desc_t *format_desc = nullptr;
            const uvc_frame_desc_t *frame_desc = nullptr;
            REQUIRE(ESP_OK == uvc_desc_get_frame_format_by_format(cfg, 1, &format, &format_desc, &frame_desc));
            REQUIRE(format_desc != nullptr);
            REQUIRE(frame_desc != nullptr);
        }
    }
}
