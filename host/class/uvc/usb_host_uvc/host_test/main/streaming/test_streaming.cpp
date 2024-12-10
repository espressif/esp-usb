/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <functional>
#include <catch2/catch_test_macros.hpp>

#include "usb/usb_types_stack.h"
#include "usb/uvc_host.h"
#include "uvc_types_priv.h"
#include "uvc_frame_priv.h"

#include "images/test_logo_jpg.hpp"
#include "test_streaming_helpers.hpp"

std::function<bool(const uvc_host_frame_t *, void *)> frame_callback; // Define a std::function to hold the lambda with captures
std::function<void(const uvc_host_stream_event_data_t *, void *)> stream_callback; // Define a std::function to hold the lambda with captures

// `send_frame_function` is a generic std::function that acts as a wrapper for sending frames in the
// test scenarios. By assigning it either `test_streaming_bulk_send_frame` or `test_streaming_isoc_send_frame`
// before calling `run_streaming_frame_reconstruction_scenario()`, we avoid duplicating the entire scenario
// code for each transfer type (bulk or isochronous). Instead, we can dynamically set the appropriate
// frame-sending function, allowing `run_streaming_frame_reconstruction_scenario` to run identical tests
// with either transfer method.
std::function<void(size_t, void *, std::span<const uint8_t>, uint8_t, bool, bool)> send_frame_function;
inline void send_function_wrapper(size_t transfer_size, void *transfer_context, std::span<const uint8_t> data, uint8_t frame_id = 0, bool error_in_sof = false, bool error_in_eof = false)
{
    send_frame_function(transfer_size, transfer_context, data, frame_id, error_in_sof, error_in_eof);
}

void run_streaming_frame_reconstruction_scenario(void)
{
    // Variables reused in all tests
    constexpr int user_arg = 0x12345678;
    uvc_stream_t stream = {}; // Define mock stream
    stream.constant.cb_arg = (void *)&user_arg;
    // @todo instead of doing this we can
    // - uvc_transfers_allocate   to allocate all transfers
    // - uvc_host_stream_unpause  to start the stream

    stream.single_thread.current_frame_id = 2; // Start with invalid frame ID
    stream.constant.stream_cb = [](const uvc_host_stream_event_data_t *event, void *user_ctx) {
        return stream_callback(event, user_ctx);
    };
    stream.constant.frame_cb = [](const uvc_host_frame_t *frame, void *user_ctx) -> bool {
        // We cannot have catching lambdas here, so we must call this std::function...
        return frame_callback(frame, user_ctx);
    };

    // We don't expect any stream events or frames be default
    stream_callback = [&](const uvc_host_stream_event_data_t *event, void *user_ctx) {
        FAIL("Unexpected event " + std::to_string(event->type));
    };
    frame_callback = [&](const uvc_host_frame_t *frame, void *user_ctx) -> bool {
        FAIL("Got unexpected frame");
        return true;
    };

    GIVEN("Streaming enabled") {
        stream.dynamic.streaming = true;
        stream.constant.vs_format = {
            .h_res = 46,
            .v_res = 46,
            .fps = 15,
            .format = UVC_VS_FORMAT_MJPEG,
        };

        AND_GIVEN("Frame buffer is allocated") {
            int frame_callback_called;
            frame_callback_called = 0;
            // We expect valid frame data
            frame_callback = [&](const uvc_host_frame_t *frame, void *user_ctx) -> bool {
                frame_callback_called++;

                REQUIRE(user_ctx == stream.constant.cb_arg); // Sanity check
                REQUIRE(frame->data_len == logo_jpg.size());
                REQUIRE(frame->vs_format.h_res == stream.constant.vs_format.h_res);
                REQUIRE(frame->vs_format.v_res == stream.constant.vs_format.v_res);
                REQUIRE(frame->vs_format.fps == stream.constant.vs_format.fps);
                REQUIRE(frame->vs_format.format == stream.constant.vs_format.format);

                std::vector<uint8_t> frame_data(frame->data, frame->data + frame->data_len);
                std::vector<uint8_t> original_data(logo_jpg.begin(), logo_jpg.end());
                REQUIRE(frame_data == original_data);

                return true;
            };

            REQUIRE(uvc_frame_allocate(&stream, 1, 100 * 1024, 0) == ESP_OK);

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
            stream_callback = [&](const uvc_host_stream_event_data_t *event, void *user_ctx) {
                REQUIRE(user_ctx == stream.constant.cb_arg); // Sanity check
                event_type = event->type;
            };
            REQUIRE(uvc_frame_allocate(&stream, 1, logo_jpg.size() - 100, 0) == ESP_OK);

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
            stream_callback = [&](const uvc_host_stream_event_data_t *event, void *user_ctx) {
                REQUIRE(user_ctx == stream.constant.cb_arg); // Sanity check
                event_type = event->type;
            };

            uvc_host_frame_t *temp_frame = nullptr;
            REQUIRE(uvc_frame_allocate(&stream, 1, 100 * 1024, 0) == ESP_OK);
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
        stream.dynamic.streaming = false;

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
    send_frame_function = test_streaming_bulk_send_frame;
    run_streaming_frame_reconstruction_scenario();
}

SCENARIO("Isochronous stream frame reconstruction", "[streaming][isoc]")
{
    send_frame_function = test_streaming_isoc_send_frame;
    run_streaming_frame_reconstruction_scenario();

    /*
    @todo ISOC test
    - Missed SoF
    - Missed EoF
    - Error in data packet
    - Messed up frame_id
    - Unaligned USB transfer sizes
    */
}
