/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <functional>
#include <catch2/catch_test_macros.hpp>

#include "usb/usb_types_stack.h"
#include "usb/uvc_host.h"
#include "esp_private/uvc_stream.h"
#include "uvc_types_priv.h"
#include "uvc_frame_priv.h"

#include "images/test_logo_jpg.hpp"
#include "test_streaming_helpers.hpp"

// `send_frame_function` is a generic std::function that acts as a wrapper for sending frames in the
// test scenarios. By assigning it either `test_streaming_bulk_send_frame` or `test_streaming_isoc_send_frame`
// before calling `run_streaming_frame_reconstruction_scenario()`, we avoid duplicating the entire scenario
// code for each transfer type (bulk or isochronous). Instead, we can dynamically set the appropriate
// frame-sending function, allowing `run_streaming_frame_reconstruction_scenario` to run identical tests
// with either transfer method.
std::function<void(size_t, void *, std::span<const uint8_t>, uint8_t, bool, bool, bool)> send_frame_function;

struct bulk_payload_test_context {
    std::vector<uint8_t> expected;
    int callback_count;
};

inline void send_function_wrapper(size_t transfer_size, void *transfer_context, std::span<const uint8_t> data, uint8_t frame_id = 0, bool error_in_sof = false, bool error_in_eof = false, bool eof_in_sof = false)
{
    send_frame_function(transfer_size, transfer_context, data, frame_id, error_in_sof, error_in_eof, eof_in_sof);
}

void run_streaming_frame_reconstruction_scenario(void)
{
    // Variables reused in all tests
    constexpr int user_arg = 0x12345678;
    uvc_stream_t stream = {}; // Define mock stream
    stream.constant.cb_arg = (void *)&user_arg;

    // We don't expect any stream events or frames by default
    stream.constant.stream_cb = [](const uvc_host_stream_event_data_t *event, void *user_ctx) {
        FAIL("Unexpected event " + std::to_string(event->type));
    };
    stream.constant.frame_cb = [](const uvc_host_frame_t *frame, void *user_ctx) -> bool {
        FAIL("Got unexpected frame");
        return true;
    };

    GIVEN("Streaming enabled") {
        REQUIRE(uvc_host_stream_unpause(&stream) == ESP_OK);
        AND_GIVEN("Frame buffer is allocated") {
            int frame_callback_called;
            frame_callback_called = 0;
            // We expect valid frame data
            stream.constant.cb_arg = (void *)&frame_callback_called;
            stream.constant.frame_cb = [](const uvc_host_frame_t *frame, void *user_ctx) -> bool {
                int *frame_callback_called = static_cast<int *>(user_ctx);
                (*frame_callback_called)++;

                REQUIRE(frame->data_len         == logo_jpg.size());
                REQUIRE(frame->vs_format.h_res  == logo_jpg_format.h_res);
                REQUIRE(frame->vs_format.v_res  == logo_jpg_format.v_res);
                REQUIRE(frame->vs_format.fps    == logo_jpg_format.fps);
                REQUIRE(frame->vs_format.format == logo_jpg_format.format);

                std::vector<uint8_t> frame_data(frame->data, frame->data + frame->data_len);
                std::vector<uint8_t> original_data(logo_jpg.begin(), logo_jpg.end());
                REQUIRE(frame_data == original_data);

                return true;
            };

            REQUIRE(uvc_frame_allocate(&stream, 1, 100 * 1024, 0, NULL) == ESP_OK);
            uvc_frame_format_update(&stream, &logo_jpg_format);

            // Test
            for (size_t transfer_size = 512; transfer_size <= 8192; transfer_size += 512) {
                WHEN("The frame data contain no errors, transfer_size = " +  std::to_string(transfer_size)) {
                    send_function_wrapper(transfer_size, &stream, std::span(logo_jpg));

                    THEN("The frame callback is called with expected frame data") {
                        REQUIRE(frame_callback_called == 1);
                    }

                    AND_WHEN("Next frame is send") {
                        send_function_wrapper(transfer_size, &stream, std::span(logo_jpg), 1);

                        THEN("The frame callback is called with expected frame data") {
                            REQUIRE(frame_callback_called == 2);
                        }
                    }
                }
            }

            WHEN("The frame contains error in SoF") {
                send_function_wrapper(1024, &stream, std::span(logo_jpg), 0, true);
                THEN("The frame callback is not called") {
                    REQUIRE(frame_callback_called == 0);
                }

                AND_WHEN("Next frame is send") {
                    send_function_wrapper(1024, &stream, std::span(logo_jpg), 1);

                    THEN("The frame callback is called with expected frame data") {
                        REQUIRE(frame_callback_called == 1);
                    }
                }
            }

            WHEN("The frame contains error in EoF") {
                send_function_wrapper(1024, &stream, std::span(logo_jpg), 0, false, true);
                THEN("The frame callback is not called") {
                    REQUIRE(frame_callback_called == 0);
                }

                AND_WHEN("Next frame is send") {
                    send_function_wrapper(1024, &stream, std::span(logo_jpg), 1);

                    THEN("The frame callback is called with expected frame data") {
                        REQUIRE(frame_callback_called == 1);
                    }
                }
            }

            WHEN("The frame contains error in SoF and EoF") {
                send_function_wrapper(1024, &stream, std::span(logo_jpg), 0, true, true);
                THEN("The frame callback is not called") {
                    REQUIRE(frame_callback_called == 0);
                }

                AND_WHEN("Next frame is send") {
                    send_function_wrapper(1024, &stream, std::span(logo_jpg), 1);

                    THEN("The frame callback is called with expected frame data") {
                        REQUIRE(frame_callback_called == 1);
                    }
                }
            }
            REQUIRE(uvc_frame_are_all_returned(&stream));
            uvc_frame_free(&stream);
        }

        AND_GIVEN("Frame buffer is too small") {
            // We expect overflow stream event
            enum uvc_host_dev_event event_type = static_cast<enum uvc_host_dev_event>(-1); // Explicitly set to invalid value
            stream.constant.cb_arg = (void *)&event_type;
            stream.constant.stream_cb = [](const uvc_host_stream_event_data_t *event, void *user_ctx) {
                enum uvc_host_dev_event *event_type = static_cast<enum uvc_host_dev_event *>(user_ctx);
                *event_type = event->type;
            };
            REQUIRE(uvc_frame_allocate(&stream, 1, logo_jpg.size() - 100, 0, NULL) == ESP_OK);

            WHEN("The frame is too big") {
                send_function_wrapper(1024, &stream, std::span(logo_jpg));
                THEN("Buffer overflow event is generated") {
                    REQUIRE(event_type == UVC_HOST_FRAME_BUFFER_OVERFLOW);
                }
            }

            REQUIRE(uvc_frame_are_all_returned(&stream));
            uvc_frame_free(&stream);
        }

        AND_GIVEN("There is no free frame buffer") {
            // We expect overflow stream event
            enum uvc_host_dev_event event_type = static_cast<enum uvc_host_dev_event>(-1); // Explicitly set to invalid value
            stream.constant.cb_arg = (void *)&event_type;
            stream.constant.stream_cb = [](const uvc_host_stream_event_data_t *event, void *user_ctx) {
                enum uvc_host_dev_event *event_type = static_cast<enum uvc_host_dev_event *>(user_ctx);
                *event_type = event->type;
            };

            uvc_host_frame_t *temp_frame = nullptr;
            REQUIRE(uvc_frame_allocate(&stream, 1, 100 * 1024, 0, NULL) == ESP_OK);
            temp_frame = uvc_frame_get_empty(&stream);
            REQUIRE(temp_frame != nullptr);

            WHEN("The frame callback is called with expected frame data") {
                send_function_wrapper(1024, &stream, std::span(logo_jpg));
                THEN("Buffer underflow event is generated") {
                    REQUIRE(event_type == UVC_HOST_FRAME_BUFFER_UNDERFLOW);
                    REQUIRE_FALSE(uvc_frame_are_all_returned(&stream));
                }
            }

            REQUIRE(uvc_host_frame_return(&stream, temp_frame) == ESP_OK);
            REQUIRE(uvc_frame_are_all_returned(&stream));
            uvc_frame_free(&stream);
        }
    }

    GIVEN("Streaming disabled") {
        WHEN("New data is received") {
            usb_transfer_t transfer = {
                .data_buffer = nullptr,
                .data_buffer_size = 0,
                .num_bytes = 0,
                .actual_num_bytes = 0,
                .flags = 0,
                .device_handle = nullptr,
                .bEndpointAddress = 0,
                .status = USB_TRANSFER_STATUS_COMPLETED,
                .timeout_ms = 0,
                .callback = nullptr,
                .context = &stream,
                .num_isoc_packets = 0,
            };
            // usb_host_transfer_submit_ExpectNever(); // We don't have this CMock extensions
            bulk_transfer_callback(&transfer);
            THEN("Transfer is not resubmitted") {
                SUCCEED("Transfer was not resubmitted");
            }
        }
    }

    /*
    @todo test
    - Error in USB transfer status
    */
}

SCENARIO("Bulk stream frame reconstruction", "[streaming][bulk]")
{
    /* Tests that are same for ISOC and BULK */
    send_frame_function = test_streaming_bulk_send_frame;
    run_streaming_frame_reconstruction_scenario();

    /* BULK specific tests */
    int frame_callback_called = 0;
    uvc_stream_t stream = {}; // Define mock stream
    stream.constant.cb_arg = (void *)&frame_callback_called;
    stream.constant.frame_cb = [](const uvc_host_frame_t *frame, void *user_ctx) -> bool {
        int *fb_called = static_cast<int *>(user_ctx);
        (*fb_called)++;
        return true;
    };

    GIVEN("Streaming enabled and frame allocated") {
        REQUIRE(uvc_host_stream_unpause(&stream) == ESP_OK);
        REQUIRE(uvc_frame_allocate(&stream, 1, 100 * 1024, 0, NULL) == ESP_OK);
        uvc_frame_format_update(&stream, &logo_jpg_format);

        WHEN("Expected SoF but got EoF") {
            test_streaming_bulk_send_frame(512, &stream, std::span(logo_jpg), 0, false, false, true);
            THEN("The frame callback is called") {
                REQUIRE(frame_callback_called == 1);
            }
        }
    }

    GIVEN("A frame spans multiple maximum-size UVC payloads") {
        constexpr size_t transfer_size = 48;
        constexpr size_t max_payload_size = 64;
        constexpr size_t payload_data_size = max_payload_size - HEADER_LEN;

        bulk_payload_test_context context = {
            .expected = std::vector<uint8_t>(2 * payload_data_size),
            .callback_count = 0,
        };
        for (size_t i = 0; i < context.expected.size(); ++i) {
            context.expected[i] = static_cast<uint8_t>(i);
        }

        uvc_stream_t payload_stream = {};
        payload_stream.constant.bulk_mps = 16;
        payload_stream.constant.cb_arg = &context;
        payload_stream.dynamic.dwMaxPayloadTransferSize = max_payload_size;
        payload_stream.constant.frame_cb = [](const uvc_host_frame_t *frame, void *user_ctx) -> bool {
            auto *ctx = static_cast<bulk_payload_test_context *>(user_ctx);
            REQUIRE(std::vector<uint8_t>(frame->data, frame->data + frame->data_len) == ctx->expected);
            ctx->callback_count++;
            return true;
        };

        REQUIRE(uvc_frame_allocate(&payload_stream, 1, context.expected.size(), 0, nullptr) == ESP_OK);
        REQUIRE(uvc_host_stream_unpause(&payload_stream) == ESP_OK);

        std::vector<uint8_t> wire_data;
        for (size_t offset = 0; offset < context.expected.size(); offset += payload_data_size) {
            uvc_payload_header_t header = {};
            header.bHeaderLength = HEADER_LEN;
            header.bmHeaderInfo.end_of_header = 1;
            const auto *header_bytes = reinterpret_cast<const uint8_t *>(&header);
            wire_data.insert(wire_data.end(), header_bytes, header_bytes + HEADER_LEN);
            wire_data.insert(wire_data.end(), context.expected.begin() + offset,
                             context.expected.begin() + offset + payload_data_size);
        }

        std::vector<uint8_t> transfer_buffer(transfer_size);
        usb_transfer_t transfer = {
            .data_buffer = transfer_buffer.data(),
            .data_buffer_size = transfer_size,
            .num_bytes = 0,
            .actual_num_bytes = 0,
            .flags = 0,
            .device_handle = nullptr,
            .bEndpointAddress = 0,
            .status = USB_TRANSFER_STATUS_COMPLETED,
            .timeout_ms = 0,
            .callback = nullptr,
            .context = &payload_stream,
            .num_isoc_packets = 0,
        };

        for (size_t offset = 0; offset < wire_data.size(); offset += transfer_size) {
            const size_t chunk_size = std::min(transfer_size, wire_data.size() - offset);
            std::copy_n(wire_data.begin() + offset, chunk_size, transfer_buffer.begin());
            transfer.actual_num_bytes = chunk_size;
            usb_host_transfer_submit_ExpectAndReturn(&transfer, ESP_OK);
            bulk_transfer_callback(&transfer);
        }

        uvc_payload_header_t eof_header = {};
        eof_header.bHeaderLength = HEADER_LEN;
        eof_header.bmHeaderInfo.end_of_header = 1;
        eof_header.bmHeaderInfo.end_of_frame = 1;
        std::copy_n(reinterpret_cast<const uint8_t *>(&eof_header), HEADER_LEN, transfer_buffer.begin());
        transfer.actual_num_bytes = HEADER_LEN;
        usb_host_transfer_submit_ExpectAndReturn(&transfer, ESP_OK);
        bulk_transfer_callback(&transfer);

        THEN("Each payload header is removed from the reconstructed frame") {
            REQUIRE(context.callback_count == 1);
        }

        REQUIRE(uvc_frame_are_all_returned(&payload_stream));
        uvc_frame_free(&payload_stream);
    }
}

SCENARIO("Isochronous stream frame reconstruction", "[streaming][isoc]")
{
    /* Tests that are same for ISOC and BULK */
    send_frame_function = test_streaming_isoc_send_frame;
    run_streaming_frame_reconstruction_scenario();

    /* ISOC specific tests */
    /*
    @todo ISOC test
    - Missed SoF
    - Missed EoF
    - Error in data packet
    - Messed up frame_id
    - Unaligned USB transfer sizes
    */
}
