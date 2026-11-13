/*
 * SPDX-FileCopyrightText: 2025-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <catch2/catch_test_macros.hpp>

#include "usb/uvc_host.h"
#include "esp_private/uvc_control.h" // For uvc_host_usb_ctrl()
#include "uvc_types_priv.h" // For uvc_host_stream_s internals

#include "mock_add_usb_device.h"
#include "test_fixtures.hpp"

#include "descriptors/logitech_c270.hpp"
#include "descriptors/customer.hpp"
#include "descriptors/customer_dual.hpp"
#include "descriptors/elp_h265.hpp"

extern "C" {
#include "Mockusb_host.h"
}

#define ADD_MOCK_DEV(addr, namespc) REQUIRE(ESP_OK == usb_host_mock_add_device(addr, (const usb_device_desc_t *)namespc::dev_desc, (const usb_config_desc_t *)namespc::cfg_desc))

/**
 * @brief Add mocked devices
 *
 * Mocked devices are defined by a device descriptor, a configuration descriptor and a device address
 */
static void _add_mocked_devices(void)
{
    // Init mocked devices list at the beginning of the test
    usb_host_mock_dev_list_init();

    // Fill mocked devices list
    // @todo add all the cameras
    ADD_MOCK_DEV(1, logitech_c270);
    ADD_MOCK_DEV(2, customer_camera);
    ADD_MOCK_DEV(3, elp_h265);
    ADD_MOCK_DEV(4, customer_camera_dual);
}

SCENARIO("UVC driver install/uninstall", "[installing]")
{
    WHEN("UVC driver is not installed") {
        THEN("Stream cannot be opened") {
            REQUIRE(ESP_ERR_INVALID_STATE == uvc_host_stream_open(nullptr, 1000, nullptr));
        }
        THEN("Uninstallation fails") {
            REQUIRE(ESP_ERR_INVALID_STATE == uvc_host_uninstall());
        }
    }

    WHEN("UVC driver is installed") {
        const uvc_host_driver_config_t uvc_driver_config = {
            .driver_task_stack_size = 4 * 1024,
            .driver_task_priority = 10,
            .xCoreID = tskNO_AFFINITY,
            .create_background_task = true,
            .event_cb = nullptr,
            .user_ctx = nullptr,
        };
        REQUIRE(ESP_OK == test_uvc_host_install(&uvc_driver_config));

        THEN("Installing again fails") {
            REQUIRE(ESP_ERR_INVALID_STATE == uvc_host_install(&uvc_driver_config));
        }

        REQUIRE(ESP_OK == test_uvc_host_uninstall());

        AND_WHEN("UVC driver is installed again") {
            THEN("Installation succeeds") {
                REQUIRE(ESP_OK == test_uvc_host_install(&uvc_driver_config));
                REQUIRE(ESP_OK == test_uvc_host_uninstall());
            }
        }
    }
}

SCENARIO("Test mocked device opening and closing", "[opening]")
{
    // We will put device adding to the SECTION, to run it just once, not repeatedly for all the following SECTIONs
    // (if multiple sections are present)
    SECTION("Add mocked devices") {
        _add_mocked_devices();

        // Optionally, print all the devices
        //usb_host_mock_print_mocked_devices(0xFF);
    }

    SECTION("Install the UVC driver") {
        const uvc_host_driver_config_t uvc_driver_config = {
            .driver_task_stack_size = 4 * 1024,
            .driver_task_priority = 10,
            .xCoreID = tskNO_AFFINITY,
            .create_background_task = true,
            .event_cb = nullptr,
            .user_ctx = nullptr,
        };
        REQUIRE(ESP_OK == test_uvc_host_install(&uvc_driver_config));

        uvc_host_stream_config_t stream_config = {
            .event_cb = nullptr,
            .frame_cb = nullptr,
            .user_ctx = nullptr,
            .usb = {
                .dev_addr = UVC_HOST_ANY_DEV_ADDR,
                .vid = UVC_HOST_ANY_VID,
                .pid = UVC_HOST_ANY_PID,
                .uvc_stream_index = 0,
            },
            .vs_format = {
                .h_res = 1280,
                .v_res = 720,
                .fps = 15,
                .format = UVC_VS_FORMAT_MJPEG,
            },
            .advanced = {
                .number_of_frame_buffers = 3,
                .frame_size = 0,
                .frame_heap_caps = 0,
                .number_of_urbs = 4,
                .urb_size = 10 * 1024,
                .user_frame_buffers = NULL,
            },
        };

        THEN("Stream can be opened") {
            uvc_host_stream_hdl_t stream = nullptr;
            REQUIRE(ESP_OK == test_uvc_host_stream_open(&stream_config, 0, &stream, true));
            REQUIRE(ESP_OK == test_uvc_host_stream_close(stream));
        }

        THEN("Stream close cleans up after interface release failure") {
            uvc_host_stream_hdl_t stream = nullptr;
            REQUIRE(ESP_OK == test_uvc_host_stream_open(&stream_config, 0, &stream, true));
            REQUIRE(ESP_OK == test_uvc_host_stream_close(stream, ESP_ERR_INVALID_STATE));
        }

        THEN("Multiple streams can be opened") {
            uvc_host_stream_hdl_t stream[10];

            // All these cameras support 1280x720@15 MJPEG format
            stream_config.usb.dev_addr = 1; // Same as = 0
            REQUIRE(ESP_OK == test_uvc_host_stream_open(&stream_config, 0, &stream[0], true));
            stream_config.usb.dev_addr = 2;
            REQUIRE(ESP_OK == test_uvc_host_stream_open(&stream_config, 0, &stream[1], false));
            stream_config.usb.dev_addr = 3;
            REQUIRE(ESP_OK == test_uvc_host_stream_open(&stream_config, 0, &stream[2], true));

            REQUIRE(ESP_OK == test_uvc_host_stream_close(stream[0]));
            REQUIRE(ESP_OK == test_uvc_host_stream_close(stream[1]));
            REQUIRE(ESP_OK == test_uvc_host_stream_close(stream[2]));
        }

        THEN("Multiple streams from the same device can be opened") {
            uvc_host_stream_hdl_t stream[2];

            stream_config.usb.dev_addr = 4; // Dual camera
            stream_config.vs_format.h_res = 720;
            stream_config.vs_format.v_res = 1280;
            REQUIRE(ESP_OK == test_uvc_host_stream_open(&stream_config, 0, &stream[0], true));

            stream_config.vs_format.h_res = 640;
            stream_config.vs_format.v_res = 480;
            stream_config.vs_format.format = UVC_VS_FORMAT_H265;
            stream_config.usb.uvc_stream_index = 1;
            REQUIRE(ESP_OK == test_uvc_host_stream_open(&stream_config, 0, &stream[1], true));

            REQUIRE(ESP_OK == test_uvc_host_stream_close(stream[0]));
            REQUIRE(ESP_OK == test_uvc_host_stream_close(stream[1]));
        }

        THEN("Non-existent VID/PID cannot be opened") {
            uvc_host_stream_hdl_t stream = nullptr;
            stream_config.usb.vid = 1;
            stream_config.usb.pid = 1;
            REQUIRE(ESP_ERR_NOT_FOUND == uvc_host_stream_open(&stream_config, 0, &stream));
            REQUIRE(stream == nullptr);
        }

        THEN("Non-existent address cannot be opened") {
            uvc_host_stream_hdl_t stream = nullptr;
            stream_config.usb.dev_addr = 30;
            REQUIRE(ESP_ERR_NOT_FOUND == uvc_host_stream_open(&stream_config, 0, &stream));
            REQUIRE(stream == nullptr);
        }

        THEN("One stream cannot be opened twice") {
            uvc_host_stream_hdl_t stream = nullptr;
            uvc_host_stream_hdl_t stream2 = nullptr;
            stream_config.usb.vid = 0x046D;
            stream_config.usb.pid = 0x0825;
            REQUIRE(ESP_OK == test_uvc_host_stream_open(&stream_config, 0, &stream, true));

            // Second call to claim the same interface will fail in usb_host_lib
            usb_host_interface_claim_ExpectAnyArgsAndReturn(ESP_FAIL);
            REQUIRE_FALSE(ESP_OK == uvc_host_stream_open(&stream_config, 0, &stream2));

            REQUIRE(ESP_OK == test_uvc_host_stream_close(stream));
        }

        REQUIRE(ESP_OK == test_uvc_host_uninstall());
    }
}

SCENARIO("Test mocked device format negotiation", "[opening]")
{
    // We will put device adding to the SECTION, to run it just once, not repeatedly for all the following SECTIONs
    // (if multiple sections are present)
    SECTION("Add mocked devices") {
        _add_mocked_devices();

        // Optionally, print all the devices
        //usb_host_mock_print_mocked_devices(0xFF);
    }

    SECTION("Install the UVC driver") {
        const uvc_host_driver_config_t uvc_driver_config = {
            .driver_task_stack_size = 4 * 1024,
            .driver_task_priority = 10,
            .xCoreID = tskNO_AFFINITY,
            .create_background_task = true,
            .event_cb = nullptr,
            .user_ctx = nullptr,
        };
        REQUIRE(ESP_OK == test_uvc_host_install(&uvc_driver_config));

        uvc_host_stream_config_t stream_config = {
            .event_cb = nullptr,
            .frame_cb = nullptr,
            .user_ctx = nullptr,
            .usb = {
                .dev_addr = UVC_HOST_ANY_DEV_ADDR,
                .vid = UVC_HOST_ANY_VID,
                .pid = UVC_HOST_ANY_PID,
                .uvc_stream_index = 0,
            },
            .vs_format = {
                .h_res = 1280,
                .v_res = 720,
                .fps = 15,
                .format = UVC_VS_FORMAT_MJPEG,
            },
            .advanced = {
                .number_of_frame_buffers = 3,
                .frame_size = 0,
                .frame_heap_caps = 0,
                .number_of_urbs = 4,
                .urb_size = 10 * 1024,
                .user_frame_buffers = NULL,
            },
        };

        THEN("Stream can be opened with default FPS value") {
            uvc_host_stream_hdl_t stream = nullptr;
            stream_config.vs_format.fps = 0;
            REQUIRE(ESP_OK == test_uvc_host_stream_open(&stream_config, 0, &stream, true));
            REQUIRE(ESP_OK == test_uvc_host_stream_close(stream));
        }

        // @todo this test is not working, because the mock does not support default format negotiation
        // To make it work, we must implement our own usb_host_transfer_submit_control_success_mock_callback()
        // that would return the default format
        // THEN("Stream can be opened with default format") {
        //     uvc_host_stream_hdl_t stream = nullptr;
        //     stream_config.vs_format.format = UVC_VS_FORMAT_DEFAULT;
        //     REQUIRE(ESP_OK == test_uvc_host_stream_open(&stream_config, 0, &stream, true));
        //     REQUIRE(ESP_OK == test_uvc_host_stream_close(stream));
        // }

        REQUIRE(ESP_OK == test_uvc_host_uninstall());
    }
}

#define NUMBER_OF_URBS 4
SCENARIO("Test mocked device URB allocation", "[opening]")
{
    // We will put device adding to the SECTION, to run it just once, not repeatedly for all the following SECTIONs
    // (if multiple sections are present)
    SECTION("Add mocked devices") {
        _add_mocked_devices();

        // Optionally, print all the devices
        //usb_host_mock_print_mocked_devices(0xFF);
    }

    SECTION("Install the UVC driver") {
        const uvc_host_driver_config_t uvc_driver_config = {
            .driver_task_stack_size = 4 * 1024,
            .driver_task_priority = 10,
            .xCoreID = tskNO_AFFINITY,
            .create_background_task = true,
            .event_cb = nullptr,
            .user_ctx = nullptr,
        };
        REQUIRE(ESP_OK == test_uvc_host_install(&uvc_driver_config));

        uvc_host_stream_config_t stream_config = {
            .event_cb = nullptr,
            .frame_cb = nullptr,
            .user_ctx = nullptr,
            .usb = {
                .dev_addr = UVC_HOST_ANY_DEV_ADDR,
                .vid = UVC_HOST_ANY_VID,
                .pid = UVC_HOST_ANY_PID,
                .uvc_stream_index = 0,
            },
            .vs_format = {
                .h_res = 1280,
                .v_res = 720,
                .fps = 15,
                .format = UVC_VS_FORMAT_MJPEG,
            },
            .advanced = {
                .number_of_frame_buffers = 3,
                .frame_size = 0,
                .frame_heap_caps = 0,
                .number_of_urbs = NUMBER_OF_URBS,
                .urb_size = 0,
                .user_frame_buffers = NULL,
            },
        };

        THEN("URB size default") {
            uvc_host_stream_hdl_t stream = nullptr;
            REQUIRE(ESP_OK == test_uvc_host_stream_open(&stream_config, 0, &stream, true));
            REQUIRE(stream->constant.num_of_xfers == NUMBER_OF_URBS);
            // This camera offers 1020 MPS with 2 extra transaction in a microframe, so 3 * 1020 = 3060
            // Default URB size is 4 * MPS, so 4 * 3060 = 12240
            REQUIRE(stream->constant.xfers[0]->data_buffer_size == 4 * 3060);
            REQUIRE(ESP_OK == test_uvc_host_stream_close(stream));
        }

        THEN("URB size greater than MPS") {
            uvc_host_stream_hdl_t stream = nullptr;
            stream_config.advanced.urb_size = 2 * 3060; // 2 * MPS
            REQUIRE(ESP_OK == test_uvc_host_stream_open(&stream_config, 0, &stream, true));
            REQUIRE(stream->constant.num_of_xfers == NUMBER_OF_URBS);
            REQUIRE(stream->constant.xfers[0]->data_buffer_size == 2 * 3060);
            REQUIRE(ESP_OK == test_uvc_host_stream_close(stream));
        }

        THEN("URB size greater than default") {
            uvc_host_stream_hdl_t stream = nullptr;
            stream_config.advanced.urb_size = 6 * 3060; // 6 * MPS
            REQUIRE(ESP_OK == test_uvc_host_stream_open(&stream_config, 0, &stream, true));
            REQUIRE(stream->constant.num_of_xfers == NUMBER_OF_URBS);
            REQUIRE(stream->constant.xfers[0]->data_buffer_size == 6 * 3060);
            REQUIRE(ESP_OK == test_uvc_host_stream_close(stream));
        }

        THEN("URB size cannot be smaller than MPS") {
            uvc_host_stream_hdl_t stream = nullptr;
            stream_config.advanced.urb_size = 500;
            REQUIRE(ESP_OK == test_uvc_host_stream_open(&stream_config, 0, &stream, true));
            REQUIRE(stream->constant.num_of_xfers == NUMBER_OF_URBS);
            // This camera offers 1020 MPS with 2 extra transaction in a microframe, so 3 * 1020 = 3060
            // This is the minimum URB size, even if user requested smaller
            REQUIRE(stream->constant.xfers[0]->data_buffer_size == 3060);
            REQUIRE(ESP_OK == test_uvc_host_stream_close(stream));
        }

        THEN("URB size must be integer multiple of MPS") {
            uvc_host_stream_hdl_t stream = nullptr;
            stream_config.advanced.urb_size = 3500;
            REQUIRE(ESP_OK == test_uvc_host_stream_open(&stream_config, 0, &stream, true));
            REQUIRE(stream->constant.num_of_xfers == NUMBER_OF_URBS);
            // This camera offers 1020 MPS with 2 extra transaction in a microframe, so 3 * 1020 = 3060
            // The driver should round up the requested URB size to the nearest multiple of MPS, which is 6120
            REQUIRE(stream->constant.xfers[0]->data_buffer_size == 2 * 3060);
            REQUIRE(ESP_OK == test_uvc_host_stream_close(stream));
        }

        REQUIRE(ESP_OK == test_uvc_host_uninstall());
    }
}

/* A control transfer that the device never answers stays in flight in the USB Host Library
 * after uvc_host_usb_ctrl() gives up on it. These helpers let the test decide when the library returns it. */
static usb_transfer_t *s_inflight_ctrl_xfer = nullptr;
static int s_device_close_count = 0;

static esp_err_t _submit_control_no_response(usb_host_client_handle_t client_hdl, usb_transfer_t *transfer, int call_count)
{
    s_inflight_ctrl_xfer = transfer;
    return ESP_OK; // The transfer callback is not called: the device does not respond
}

static void _return_inflight_ctrl_xfer(usb_transfer_status_t status)
{
    REQUIRE(s_inflight_ctrl_xfer != nullptr);
    usb_transfer_t *transfer = s_inflight_ctrl_xfer;
    s_inflight_ctrl_xfer = nullptr;
    transfer->status = status;
    transfer->actual_num_bytes = (status == USB_TRANSFER_STATUS_COMPLETED) ? transfer->num_bytes : 0;
    transfer->callback(transfer);
}

static esp_err_t _device_close_counting_callback(usb_host_client_handle_t client_hdl, usb_device_handle_t dev_hdl, int call_count)
{
    s_device_close_count++;
    return usb_host_device_close_mock_callback(client_hdl, dev_hdl, call_count);
}

static esp_err_t _send_ctrl_request(uvc_host_stream_hdl_t stream)
{
    // Any request will do, use SET_INTERFACE(0) with no data stage
    return uvc_host_usb_ctrl(stream,
                             USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_STANDARD | USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
                             USB_B_REQUEST_SET_INTERFACE, 0, stream->constant.bInterfaceNumber, 0, nullptr);
}

SCENARIO("Control transfer timeout", "[opening]")
{
    // We will put device adding to the SECTION, to run it just once, not repeatedly for all the following SECTIONs
    // (if multiple sections are present)
    SECTION("Add mocked devices") {
        _add_mocked_devices();
    }

    SECTION("Install the UVC driver") {
        const uvc_host_driver_config_t uvc_driver_config = {
            .driver_task_stack_size = 4 * 1024,
            .driver_task_priority = 10,
            .xCoreID = tskNO_AFFINITY,
            .create_background_task = true,
            .event_cb = nullptr,
            .user_ctx = nullptr,
        };
        REQUIRE(ESP_OK == test_uvc_host_install(&uvc_driver_config));

        uvc_host_stream_config_t stream_config = {
            .event_cb = nullptr,
            .frame_cb = nullptr,
            .user_ctx = nullptr,
            .usb = {
                .dev_addr = 1,
                .vid = UVC_HOST_ANY_VID,
                .pid = UVC_HOST_ANY_PID,
                .uvc_stream_index = 0,
            },
            .vs_format = {
                .h_res = 1280,
                .v_res = 720,
                .fps = 15,
                .format = UVC_VS_FORMAT_MJPEG,
            },
            .advanced = {
                .number_of_frame_buffers = 3,
                .frame_size = 0,
                .frame_heap_caps = 0,
                .number_of_urbs = 4,
                .urb_size = 10 * 1024,
                .user_frame_buffers = NULL,
            },
        };

        uvc_host_stream_hdl_t stream = nullptr;
        REQUIRE(ESP_OK == test_uvc_host_stream_open(&stream_config, 0, &stream, true));
        usb_host_device_close_Stub(_device_close_counting_callback);
        s_device_close_count = 0;

        // The device does not respond, uvc_host_usb_ctrl() times out while the transfer stays in flight
        usb_host_transfer_submit_control_AddCallback(_submit_control_no_response);
        usb_host_transfer_submit_control_ExpectAnyArgsAndReturn(ESP_OK);
        REQUIRE(ESP_ERR_TIMEOUT == _send_ctrl_request(stream));

        THEN("No control transfer is submitted until the timed out one is returned") {
            // usb_host_transfer_submit_control() is not expected here: the CTRL transfer is still in flight
            REQUIRE(ESP_ERR_NOT_FINISHED == _send_ctrl_request(stream));

            _return_inflight_ctrl_xfer(USB_TRANSFER_STATUS_CANCELED);
            usb_host_transfer_submit_control_AddCallback(usb_host_transfer_submit_control_success_mock_callback);
            usb_host_transfer_submit_control_ExpectAnyArgsAndReturn(ESP_OK);
            REQUIRE(ESP_OK == _send_ctrl_request(stream));

            REQUIRE(ESP_OK == test_uvc_host_stream_close(stream));
            REQUIRE(s_device_close_count == 1);
        }

        THEN("Late completion of the timed out transfer is not taken for completion of the next one") {
            _return_inflight_ctrl_xfer(USB_TRANSFER_STATUS_COMPLETED);

            // The device does not respond to the next request either
            usb_host_transfer_submit_control_ExpectAnyArgsAndReturn(ESP_OK);
            REQUIRE(ESP_ERR_TIMEOUT == _send_ctrl_request(stream));

            _return_inflight_ctrl_xfer(USB_TRANSFER_STATUS_COMPLETED);
            REQUIRE(ESP_OK == test_uvc_host_stream_close(stream));
            REQUIRE(s_device_close_count == 1);
        }

        THEN("Device is not closed until the timed out transfer is returned") {
            REQUIRE(ESP_OK == test_uvc_host_stream_close(stream));
            // Closing the device now would trip the in-flight CTRL transfer check in usbh_dev_close()
            REQUIRE(s_device_close_count == 0);
            // The driver cannot free the CTRL transfer while the USB Host Library owns it
            REQUIRE(ESP_ERR_NOT_FINISHED == uvc_host_uninstall());

            // The USB Host Library returns the transfer, e.g. when the device is disconnected
            _return_inflight_ctrl_xfer(USB_TRANSFER_STATUS_NO_DEVICE);
            REQUIRE(s_device_close_count == 1);
        }

        REQUIRE(ESP_OK == test_uvc_host_uninstall());
    }
}
