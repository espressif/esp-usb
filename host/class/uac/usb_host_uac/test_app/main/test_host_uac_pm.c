/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "unity.h"
#include "usb/usb_host.h"
#include "usb/uac_host.h"
#include "test_host_uac_common.h"

#ifdef UAC_HOST_SUSPEND_RESUME_API_SUPPORTED

const static char *TAG = "UAC_TEST_PM";

#define TEST_EVENT_CLIENT_WAIT_MS               500     // Wait for a client event delivery
#define TEST_EVENT_TRANSFER_WAIT_MS             1000    // Wait for a transfer event delivery

// Expect suspend event
static const event_queue_t expect_suspend_evt = {
    .event_group = UAC_DEVICE_EVENT,
    .device_evt.event = UAC_HOST_DEVICE_EVENT_SUSPENDED,
};

// Expect resume event
static const event_queue_t expect_resume_evt = {
    .event_group = UAC_DEVICE_EVENT,
    .device_evt.event = UAC_HOST_DEVICE_EVENT_RESUMED,
};

// Expect disconnect event
static const event_queue_t expect_disconn_evt = {
    .event_group = UAC_DEVICE_EVENT,
    .device_evt.event = UAC_HOST_DRIVER_EVENT_DISCONNECTED,
};

// Expect RX done event
static const event_queue_t expect_rx_done_evt = {
    .event_group = UAC_DEVICE_EVENT,
    .device_evt.event = UAC_HOST_DEVICE_EVENT_RX_DONE,
};

// Device audio buffer settings
typedef struct {
    const uint32_t threshold;   /* Audio buffer threshold*/
    const uint32_t size;        /* Audio buffer size*/
} uac_dev_buf_params_t;

// Default device audi buffer settings
static uac_dev_buf_params_t audio_buf_default = {
    .threshold = 4800,
    .size = 19200,
};

/**
 * @brief: Test basic suspend/resume signaling multiple interfaces
 *
 * Purpose:
 *   - Test client suspend/resume event delivery for multiple opened interfaces
 *
 * Procedure:
 *   - Connect device and open 2 interfaces
 *   - Suspend the root port and expect 2 suspend events
 *   - Resume the root port and expect 2 resume events
 *   - Close both interfaces of the device
 */
TEST_CASE("Test basic suspend/resume multiple ifaces", "[uac_host][power_management]")
{
    uint8_t mic_iface_num = 0;
    uint8_t spk_iface_num = 0;
    uint8_t if_rx = false;
    test_handle_dev_connection(&mic_iface_num, &if_rx);
    if (!if_rx) {
        spk_iface_num = mic_iface_num;
        test_handle_dev_connection(&mic_iface_num, &if_rx);
        TEST_ASSERT_EQUAL(if_rx, true);
    } else {
        test_handle_dev_connection(&spk_iface_num, &if_rx);
        TEST_ASSERT_EQUAL(if_rx, false);
    }

    // Open 2 interfaces of the device
    uac_host_device_handle_t mic_device_handle = NULL, spk_device_handle = NULL;
    test_open_mic_device(mic_iface_num, audio_buf_default.size, audio_buf_default.threshold, &mic_device_handle);
    test_open_spk_device(spk_iface_num, audio_buf_default.size, audio_buf_default.threshold, &spk_device_handle);

    // Suspend the root port
    printf("Issue suspend\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());

    // Expect 2 Suspend events -> one for each iface
    expect_client_event(&expect_suspend_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));
    expect_client_event(&expect_suspend_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));

    // Stay suspended for a while
    vTaskDelay(100);

    // Resume the root port
    printf("Issue resume\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_resume());

    // Expect 2 Resume events -> one for each iface
    expect_client_event(&expect_resume_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));
    expect_client_event(&expect_resume_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));

    // Close both interfaces of the device
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_close(spk_device_handle));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_close(mic_device_handle));
}

/**
 * @brief: Test basic suspend/resume signaling single interface
 *
 * Purpose:
 *   - Test client suspend/resume event delivery for single opened interface
 *
 * Procedure:
 *   - Connect device and open one interface
 *   - Suspend the root port and expect 1 suspend event
 *   - Resume the root port and expect 1 resume event
 *   - Close the opened interface
 */
TEST_CASE("Test basic suspend/resume single iface", "[uac_host][power_management]")
{
    uint8_t mic_iface_num = 0;
    uint8_t spk_iface_num = 0;
    uint8_t if_rx = false;
    test_handle_dev_connection(&mic_iface_num, &if_rx);
    if (!if_rx) {
        spk_iface_num = mic_iface_num;
        test_handle_dev_connection(&mic_iface_num, &if_rx);
        TEST_ASSERT_EQUAL(if_rx, true);
    } else {
        test_handle_dev_connection(&spk_iface_num, &if_rx);
        TEST_ASSERT_EQUAL(if_rx, false);
    }

    // Open device's interface
    uac_host_device_handle_t mic_device_handle = NULL;
    test_open_mic_device(mic_iface_num, audio_buf_default.size, audio_buf_default.threshold, &mic_device_handle);

    // Suspend the root port
    printf("Issue suspend\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());

    // Expect 1 Suspend event -> only one iface is opened
    expect_client_event(&expect_suspend_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));
    expect_client_event(NULL, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));

    // Stay suspended for a while
    vTaskDelay(100);

    // Resume the root port
    printf("Issue resume\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_resume());

    // Expect 1 Resume event -> only one iface is opened
    expect_client_event(&expect_resume_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));
    expect_client_event(NULL, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));

    // Close the opened interface
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_close(mic_device_handle));
}

/**
 * @brief: Test suspend/resume signaling no interface opened
 *
 * Purpose:
 *   - Test client suspend/resume event delivery with a device connected, but no interface opened by the driver
 *
 * Procedure:
 *   - Connect device, but, don't open any interface
 *   - Suspend the root port and expect no events (no interface opened)
 *   - Resume the root port and expect no events (no interface opened)
 */
TEST_CASE("Test suspend/resume no interface opened", "[uac_host][power_management]")
{
    uint8_t mic_iface_num = 0;
    uint8_t spk_iface_num = 0;
    uint8_t if_rx = false;
    test_handle_dev_connection(&mic_iface_num, &if_rx);
    if (!if_rx) {
        spk_iface_num = mic_iface_num;
        test_handle_dev_connection(&mic_iface_num, &if_rx);
        TEST_ASSERT_EQUAL(if_rx, true);
    } else {
        test_handle_dev_connection(&spk_iface_num, &if_rx);
        TEST_ASSERT_EQUAL(if_rx, false);
    }

    // Suspend the root port
    printf("Issue suspend\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());

    // Expect NO Suspend event -> No interface was opened by the client
    expect_client_event(NULL, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS * 2));

    // Resume the root port
    printf("Issue resume\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_resume());

    // Expect NO Resume event -> No interface was opened by the client
    expect_client_event(NULL, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS * 2));
}

/**
 * @brief: Test close opened interface while the root port is in suspended state
 *
 * Purpose:
 *   - Test client reaction to closing device while the root port is in suspended state
 *
 * Procedure:
 *   - Connect device and open one interface
 *   - Suspend the root port and expect suspend event
 *   - Close the opened interface while the root port is in suspended state
 */
TEST_CASE("Test close opened interface in suspended state", "[uac_host][power_management]")
{
    uint8_t mic_iface_num = 0;
    uint8_t spk_iface_num = 0;
    uint8_t if_rx = false;
    test_handle_dev_connection(&mic_iface_num, &if_rx);
    if (!if_rx) {
        spk_iface_num = mic_iface_num;
        test_handle_dev_connection(&mic_iface_num, &if_rx);
        TEST_ASSERT_EQUAL(if_rx, true);
    } else {
        test_handle_dev_connection(&spk_iface_num, &if_rx);
        TEST_ASSERT_EQUAL(if_rx, false);
    }

    // Open device's interface
    uac_host_device_handle_t mic_device_handle = NULL;
    test_open_mic_device(mic_iface_num, audio_buf_default.size, audio_buf_default.threshold, &mic_device_handle);

    // Suspend the root port
    printf("Issue suspend\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());

    // Expect 1 Suspend event -> only one iface is opened
    expect_client_event(&expect_suspend_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));

    // Close opened interface, while the root port is in suspended state
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_close(mic_device_handle));
}

/**
 * @brief: Test close opened and started interface while the root port is in suspended state
 *
 * Purpose:
 *   - Test client reaction to closing started device while the root port is in suspended state
 *
 * Procedure:
 *   - Connect device, open and start one interface
 *   - Suspend the root port and expect suspend event
 *   - Close the started interface while the root port is in suspended state
 */
TEST_CASE("Test close started interface in suspended device", "[uac_host][power_management]")
{
    uint8_t mic_iface_num = 0;
    uint8_t spk_iface_num = 0;
    uint8_t if_rx = false;
    test_handle_dev_connection(&mic_iface_num, &if_rx);
    if (!if_rx) {
        spk_iface_num = mic_iface_num;
        test_handle_dev_connection(&mic_iface_num, &if_rx);
        TEST_ASSERT_EQUAL(if_rx, true);
    } else {
        test_handle_dev_connection(&spk_iface_num, &if_rx);
        TEST_ASSERT_EQUAL(if_rx, false);
    }

    // Open device's interface
    uac_host_device_handle_t mic_device_handle = NULL;
    test_open_mic_device(mic_iface_num, audio_buf_default.size, audio_buf_default.threshold, &mic_device_handle);

    // Get mic alt interface 1 params
    uac_host_dev_alt_param_t mic_alt_params;
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_get_device_alt_param(mic_device_handle, 1, &mic_alt_params));

    // Start mic interface
    uac_host_stream_config_t stream_config = {
        .channels = mic_alt_params.channels,
        .bit_resolution = mic_alt_params.bit_resolution,
        .sample_freq = mic_alt_params.sample_freq[0],
        .flags = FLAG_STREAM_PAUSE_AFTER_START,
    };
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_start(mic_device_handle, &stream_config));

    // Keep the device in started state for some time
    vTaskDelay(100);

    // Suspend the root port
    printf("Issue suspend\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());

    // Expect 1 Suspend event -> only one iface is opened
    expect_client_event(&expect_suspend_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));

    // Close opened and started interface, while the root port is in suspended state
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_close(mic_device_handle));
}

/**
 * @brief: Test suspending/resuming the root port after starting an interface
 *
 * Purpose:
 *   - Reaction of the client to suspend/resume event while the interface has been started
 *
 * Procedure:
 *   - Connect, open and start interface
 *   - Suspend the root port and expect suspend event
 *   - Resume the root port and expect resume event
 *   - Stop and close the interface
 */
TEST_CASE("Test suspend/resume, after starting an interface", "[uac_host][power_management]")
{
    uint8_t mic_iface_num = 0;
    uint8_t spk_iface_num = 0;
    uint8_t if_rx = false;
    test_handle_dev_connection(&mic_iface_num, &if_rx);
    if (!if_rx) {
        spk_iface_num = mic_iface_num;
        test_handle_dev_connection(&mic_iface_num, &if_rx);
        TEST_ASSERT_EQUAL(if_rx, true);
    } else {
        test_handle_dev_connection(&spk_iface_num, &if_rx);
        TEST_ASSERT_EQUAL(if_rx, false);
    }

    // Open device's interface
    uac_host_device_handle_t mic_device_handle = NULL;
    test_open_mic_device(mic_iface_num, audio_buf_default.size, audio_buf_default.threshold, &mic_device_handle);

    // Get mic alt interface 1 params
    uac_host_dev_alt_param_t mic_alt_params;
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_get_device_alt_param(mic_device_handle, 1, &mic_alt_params));

    // Start the mic interface
    uac_host_stream_config_t stream_config = {
        .channels = mic_alt_params.channels,
        .bit_resolution = mic_alt_params.bit_resolution,
        .sample_freq = mic_alt_params.sample_freq[0],
        .flags = FLAG_STREAM_PAUSE_AFTER_START,
    };
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_start(mic_device_handle, &stream_config));

    // Keep the device in started state for some time
    vTaskDelay(100);

    // Suspend the root port
    printf("Issue suspend\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());

    // Expect 1 Suspend event -> only one iface is opened
    expect_client_event(&expect_suspend_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));
    expect_client_event(NULL, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));

    // Stay suspended for a while
    vTaskDelay(100);

    // Resume the root port
    printf("Issue resume\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_resume());

    // Expect 1 Resume event -> only one iface is opened
    expect_client_event(&expect_resume_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));
    expect_client_event(NULL, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));

    // Expect the interface to be started (to be in UAC_INTERFACE_STATE_READY), stop the device
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_stop(mic_device_handle));
    // Close opened and already stopped interface
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_close(mic_device_handle));
}

/**
 * @brief: Test auto-resume suspended root port by ctrl transfer
 *
 * Purpose:
 *   - Reaction of the client to auto-resume by a transfer submit
 *
 * Procedure:
 *   - Connect, open and start interface
 *   - Suspend the root port and expect suspend event
 *   - Send a ctrl transfer to a device, while the root port is in suspended state
 *   - Expect resume event, caused by auto resume by transfer submit
 *   - Unpause the interface and expect transfer events, pause the interface and expect no transfer events
 *   - Stop and close the interface
 */
TEST_CASE("Test auto-resume by ctrl transfer", "[uac_host][power_management]")
{
    uint8_t mic_iface_num = 0;
    uint8_t spk_iface_num = 0;
    uint8_t if_rx = false;
    test_handle_dev_connection(&mic_iface_num, &if_rx);
    if (!if_rx) {
        spk_iface_num = mic_iface_num;
        test_handle_dev_connection(&mic_iface_num, &if_rx);
        TEST_ASSERT_EQUAL(if_rx, true);
    } else {
        test_handle_dev_connection(&spk_iface_num, &if_rx);
        TEST_ASSERT_EQUAL(if_rx, false);
    }

    // Open device's interface
    uac_host_device_handle_t mic_device_handle = NULL;
    test_open_mic_device(mic_iface_num, audio_buf_default.size, audio_buf_default.threshold, &mic_device_handle);

    // Get mic alt interface 1 params
    uac_host_dev_alt_param_t mic_alt_params;
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_get_device_alt_param(mic_device_handle, 1, &mic_alt_params));

    // Start the device
    uac_host_stream_config_t stream_config = {
        .channels = mic_alt_params.channels,
        .bit_resolution = mic_alt_params.bit_resolution,
        .sample_freq = mic_alt_params.sample_freq[0],
        .flags = FLAG_STREAM_PAUSE_AFTER_START,
    };
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_start(mic_device_handle, &stream_config));

    // Keep the device in started state for some time
    vTaskDelay(10);

    // Suspend the root port
    printf("Issue suspend\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());

    // Expect 1 Suspend event -> only one iface is opened
    expect_client_event(&expect_suspend_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));

    // Stay suspended for a while
    vTaskDelay(100);

    // Send ctrl transfer to auto resume the device
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_set_mute(mic_device_handle, 0));

    // Expect 1 Resume event (resume by transfer submit) -> only one iface is opened
    expect_client_event(&expect_resume_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));

    // Some time for the usb host lib to process the auto resume by transfer
    vTaskDelay(100);

    // Unpause the interface
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_unpause(mic_device_handle));

    // Expect transfer events after unpausing the interface
    TEST_ASSERT(expect_transfer_event(&expect_rx_done_evt, pdMS_TO_TICKS(TEST_EVENT_TRANSFER_WAIT_MS)));

    // Expect the interface to be active (to be in UAC_INTERFACE_STATE_READY),
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_pause(mic_device_handle));

    // Expect no transfer events after pausing the interface
    TEST_ASSERT_FALSE(expect_transfer_event(NULL, pdMS_TO_TICKS(TEST_EVENT_TRANSFER_WAIT_MS)));

    // Stop and close the interface
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_stop(mic_device_handle));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_close(mic_device_handle));
}

/**
 * @brief: Test start an interface while the root port is suspended, FLAG_STREAM_PAUSE_AFTER_START flag is set
 *
 * Purpose:
 *   - Reaction of the client to starting the interface multiple times, when the root port is in suspended state
 *   - Usage of FLAG_STREAM_PAUSE_AFTER_START flag in combination with suspended state
 *
 * Procedure:
 *   - Connect and open one interface
 *   - Suspend the root port and expect suspend event
 *   - Fail to start the interface while the root port is in suspended state
 *   - Resume the root port manually and expect resume event
 *   - Start the interface normally with the FLAG_STREAM_PAUSE_AFTER_START set
 *   - Suspend the root port and expect suspend event
 *   - Start previously started interface while the root port is in suspended state
 *   - Expect resume event, caused by auto resume by transfer submit
 *   - Stop and close the interface
 */
TEST_CASE("Test start (pause after start) suspended interface", "[uac_host][power_management]")
{
    uint8_t mic_iface_num = 0;
    uint8_t spk_iface_num = 0;
    uint8_t if_rx = false;
    test_handle_dev_connection(&mic_iface_num, &if_rx);
    if (!if_rx) {
        spk_iface_num = mic_iface_num;
        test_handle_dev_connection(&mic_iface_num, &if_rx);
        TEST_ASSERT_EQUAL(if_rx, true);
    } else {
        test_handle_dev_connection(&spk_iface_num, &if_rx);
        TEST_ASSERT_EQUAL(if_rx, false);
    }

    // Open device's interface
    uac_host_device_handle_t mic_device_handle = NULL;
    test_open_mic_device(mic_iface_num, audio_buf_default.size, audio_buf_default.threshold, &mic_device_handle);

    // Get mic alt interface 1 params
    uac_host_dev_alt_param_t mic_alt_params;
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_get_device_alt_param(mic_device_handle, 1, &mic_alt_params));

    // Suspend the root port
    printf("Issue suspend\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());

    // Expect 1 Suspend event -> only one iface is opened
    expect_client_event(&expect_suspend_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));

    // Stay suspended for a while
    vTaskDelay(100);

    uac_host_stream_config_t stream_config = {
        .channels = mic_alt_params.channels,
        .bit_resolution = mic_alt_params.bit_resolution,
        .sample_freq = mic_alt_params.sample_freq[0],
        .flags = FLAG_STREAM_PAUSE_AFTER_START,
    };
    // Try to start device's interface from suspended state, when the previous state was IDLE
    // Expect to fail, since the usb_host_lib must claim device's EPs, for which the root port must not be in suspended state
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, uac_host_device_start(mic_device_handle, &stream_config));

    // Resume the root port
    printf("Issue resume\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_resume());

    // Expect 1 Resume event -> only one iface is opened
    expect_client_event(&expect_resume_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));

    // Start the device's interface with the device in resumed state
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_start(mic_device_handle, &stream_config));

    // Suspend the root port
    printf("Issue suspend\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());

    // Expect 1 Suspend event -> only one iface is opened
    expect_client_event(&expect_suspend_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));

    // Start the interface with the root port in suspended state (after it has already been successfully started)
    // The function issues a ctrl transfer to the device, which effectively resumes the root port
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_start(mic_device_handle, &stream_config));

    // Expect 1 Resume event (resume by transfer submit) -> only one iface is opened
    expect_client_event(&expect_resume_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));

    // Some time for the usb_host_lib to process the auto resume by transfer
    vTaskDelay(100);

    // Expect the interface to be started (in UAC_INTERFACE_STATE_READY), stop the interface
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_stop(mic_device_handle));
    // Close opened and stopped interface
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_close(mic_device_handle));
}

/**
 * @brief: Test start an interface while the root port is suspended, FLAG_STREAM_PAUSE_AFTER_START flag not set
 *
 * Purpose:
 *   - Reaction of the client to starting the interface multiple times, when the root port is in suspended state
 *   - Usage of FLAG_STREAM_PAUSE_AFTER_START flag in combination with suspended state
 *
 * Procedure:
 *   - Connect and open one interface
 *   - Suspend the root port and expect suspend event
 *   - Fail to start the interface while the root port is in suspended state
 *   - Resume the root port manually and expect resume event
 *   - Start the interface normally with the FLAG_STREAM_PAUSE_AFTER_START not set
 *   - Expect transfer events for the interface (interface has been unpaused after start)
 *   - Suspend the root port and expect suspend event
 *   - Expect NO transfer events for the interface
 *   - Start previously started interface while the root port is in suspended state
 *   - Expect resume event, caused by auto resume by transfer submit
 *   - Expect transfer events for the interface
 *   - Stop and close the interface
 */
TEST_CASE("Test start (unpause after start) suspended interface", "[uac_host][power_management]")
{
    uint8_t mic_iface_num = 0;
    uint8_t spk_iface_num = 0;
    uint8_t if_rx = false;
    test_handle_dev_connection(&mic_iface_num, &if_rx);
    if (!if_rx) {
        spk_iface_num = mic_iface_num;
        test_handle_dev_connection(&mic_iface_num, &if_rx);
        TEST_ASSERT_EQUAL(if_rx, true);
    } else {
        test_handle_dev_connection(&spk_iface_num, &if_rx);
        TEST_ASSERT_EQUAL(if_rx, false);
    }

    // Open device's interface
    uac_host_device_handle_t mic_device_handle = NULL;
    test_open_mic_device(mic_iface_num, audio_buf_default.size, audio_buf_default.threshold, &mic_device_handle);

    // Get mic alt interface 1 params
    uac_host_dev_alt_param_t mic_alt_params;
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_get_device_alt_param(mic_device_handle, 1, &mic_alt_params));

    // Suspend the root port
    printf("Issue suspend\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());

    // Expect 1 Suspend event -> only one iface is opened
    expect_client_event(&expect_suspend_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));

    // Stay suspended for a while
    vTaskDelay(100);

    uac_host_stream_config_t stream_config = {
        .channels = mic_alt_params.channels,
        .bit_resolution = mic_alt_params.bit_resolution,
        .sample_freq = mic_alt_params.sample_freq[0],
    };
    // Try to start interface from suspended state, when the previous state was IDLE
    // Expect to fail, since the usb_host_lib must claim device's EPs, for which the root port must not be in suspended state
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, uac_host_device_start(mic_device_handle, &stream_config));

    // Resume the root port
    printf("Issue resume\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_resume());

    // Expect 1 Resume event -> only one iface is opened
    expect_client_event(&expect_resume_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));

    // Start the interface with the root port in resumed state
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_start(mic_device_handle, &stream_config));

    // Interface has been started without FLAG_STREAM_PAUSE_AFTER_START flag, expect transfer events after starting the device
    TEST_ASSERT(expect_transfer_event(&expect_rx_done_evt, pdMS_TO_TICKS(TEST_EVENT_TRANSFER_WAIT_MS)));

    // Suspend the root port
    printf("Issue suspend\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());

    // Expect 1 Suspend event -> only one iface is opened
    expect_client_event(&expect_suspend_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));

    // Expect no transfer events
    TEST_ASSERT_FALSE(expect_transfer_event(NULL, pdMS_TO_TICKS(TEST_EVENT_TRANSFER_WAIT_MS)));

    // Start the interface with the root port in suspended state (after it has been already successfully started)
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_start(mic_device_handle, &stream_config));

    // Expect 1 Resume event (resume by transfer submit) -> only one iface is opened
    expect_client_event(&expect_resume_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));

    // Expect transfer events after starting the device
    TEST_ASSERT(expect_transfer_event(&expect_rx_done_evt, pdMS_TO_TICKS(TEST_EVENT_TRANSFER_WAIT_MS)));

    // Expect the interface to be started (in UAC_INTERFACE_STATE_READY), stop the interface
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_stop(mic_device_handle));
    // Close the opened and stopped interface
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_close(mic_device_handle));
}

/**
 * @brief: Test stopping interface from suspended state
 *
 * Purpose:
 *   - Reaction of the client to stopping paused interface, while the root port is in suspended state
 *   - Reaction of the client to stopping unpaused interface, while the root port is in suspended state
 *
 * Procedure:
 *   - Connect, open and start (pause after start) interface
 *   - Suspend the root port and expect suspend event
 *   - Stop the interface while the root port is in suspended state
 *   - Resume the root port and expect resume event
 *   - Start, unpause the interface and make sure the data are being transferred
 *   - Suspend the root port and expect suspend event
 *   - Stop and close the interface while the root port is in suspended state
 */
TEST_CASE("Test interface stop from suspended state", "[uac_host][power_management][rx]")
{
    uint8_t mic_iface_num = 0;
    uint8_t spk_iface_num = 0;
    uint8_t if_rx = false;
    test_handle_dev_connection(&mic_iface_num, &if_rx);
    if (!if_rx) {
        spk_iface_num = mic_iface_num;
        test_handle_dev_connection(&mic_iface_num, &if_rx);
        TEST_ASSERT_EQUAL(if_rx, true);
    } else {
        test_handle_dev_connection(&spk_iface_num, &if_rx);
        TEST_ASSERT_EQUAL(if_rx, false);
    }

    // Open device's interface
    uac_host_device_handle_t mic_device_handle = NULL;
    test_open_mic_device(mic_iface_num, audio_buf_default.size, audio_buf_default.threshold, &mic_device_handle);

    // Get mic alt interface 1 params
    uac_host_dev_alt_param_t mic_alt_params;
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_get_device_alt_param(mic_device_handle, 1, &mic_alt_params));

    // Start the interface
    uac_host_stream_config_t stream_config = {
        .channels = mic_alt_params.channels,
        .bit_resolution = mic_alt_params.bit_resolution,
        .sample_freq = mic_alt_params.sample_freq[0],
        .flags = FLAG_STREAM_PAUSE_AFTER_START,
    };
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_start(mic_device_handle, &stream_config));

    // Suspend the root port and expect 1 Suspend event -> only one iface is opened
    printf("Issue suspend\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());
    expect_client_event(&expect_suspend_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));

    // Stay suspended for a while
    vTaskDelay(100);

    // Stop the started interface while the root port is suspended
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_stop(mic_device_handle));

    // Resume the root port and expect 1 Resume event -> only one iface is opened
    printf("Issue resume\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_resume());
    expect_client_event(&expect_resume_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));

    // Start and unpause the interface
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_start(mic_device_handle, &stream_config));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_unpause(mic_device_handle));

    // Check that data events are being sent
    TEST_ASSERT(expect_transfer_event(&expect_rx_done_evt, pdMS_TO_TICKS(TEST_EVENT_TRANSFER_WAIT_MS)));

    // Suspend the root port and expect 1 Suspend event -> only one iface is opened
    printf("Issue suspend\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());
    expect_client_event(&expect_suspend_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));

    // Stop and close unpaused interface while the root port is in suspended state
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_stop(mic_device_handle));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_close(mic_device_handle));
}

/**
 * @brief: Test unpause interface, which was previously in ready state while the root port is in suspended state
 *
 * Purpose:
 *   - Reaction of the client to unpausing ready interface, while the root port is in suspended state
 *
 * Procedure:
 *   - Connect, open and start (pause after start) interface
 *   - Suspend the root port and expect suspend event
 *   - Unpause started interface while the root port is in suspended state
 *   - Expect resume event, caused by ctrl transfer submit, as the uac_host_device_unpause issues a ctrl transfer
 *   - Expect data transfer events from the device
 *   - Pause, stop and close the interface
 */
TEST_CASE("Test unpause ready interface from suspended state", "[uac_host][power_management][rx]")
{
    uint8_t mic_iface_num = 0;
    uint8_t spk_iface_num = 0;
    uint8_t if_rx = false;
    test_handle_dev_connection(&mic_iface_num, &if_rx);
    if (!if_rx) {
        spk_iface_num = mic_iface_num;
        test_handle_dev_connection(&mic_iface_num, &if_rx);
        TEST_ASSERT_EQUAL(if_rx, true);
    } else {
        test_handle_dev_connection(&spk_iface_num, &if_rx);
        TEST_ASSERT_EQUAL(if_rx, false);
    }

    // Open device's interface
    uac_host_device_handle_t mic_device_handle = NULL;
    test_open_mic_device(mic_iface_num, audio_buf_default.size, audio_buf_default.threshold, &mic_device_handle);

    // Get mic alt interface 1 params
    uac_host_dev_alt_param_t mic_alt_params;
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_get_device_alt_param(mic_device_handle, 1, &mic_alt_params));

    // Start the interface
    uac_host_stream_config_t stream_config = {
        .channels = mic_alt_params.channels,
        .bit_resolution = mic_alt_params.bit_resolution,
        .sample_freq = mic_alt_params.sample_freq[0],
        .flags = FLAG_STREAM_PAUSE_AFTER_START,
    };
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_start(mic_device_handle, &stream_config));

    // Suspend the root port and expect 1 Suspend event -> only one iface is opened
    printf("Issue suspend\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());
    expect_client_event(&expect_suspend_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));

    // Unpause started interface while the root port is in suspended state
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_unpause(mic_device_handle));

    // Expect 1 Resume event (resume by transfer submit) -> only one iface is opened
    expect_client_event(&expect_resume_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));

    // Check that data events are being sent
    TEST_ASSERT(expect_transfer_event(&expect_rx_done_evt, pdMS_TO_TICKS(TEST_EVENT_TRANSFER_WAIT_MS)));

    // Pause, stop and close the interface
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_pause(mic_device_handle));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_stop(mic_device_handle));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_close(mic_device_handle));
}

/**
 * @brief: Test unpause interface, which was previously in active state while the root port is in suspended state
 *
 * Purpose:
 *   - Reaction of the client to unpausing active interface, while the root port is in suspended state
 *
 * Procedure:
 *   - Connect, open and start (unpause after start) interface
 *   - Expect data transfer events
 *   - Suspend the root port and expect suspend event
 *   - Expect no data transfer events
 *   - Unpause the interface, which was previously unpaused, while the root port is in suspended state
 *   - Expect resume event, caused by ctrl transfer submit, as the uac_host_device_unpause issues a ctrl transfer
 *   - Expect data transfer events
 *   - Pause, stop and close the interface
 */
TEST_CASE("Test unpause active interface from suspended state", "[uac_host][power_management][rx]")
{
    uint8_t mic_iface_num = 0;
    uint8_t spk_iface_num = 0;
    uint8_t if_rx = false;
    test_handle_dev_connection(&mic_iface_num, &if_rx);
    if (!if_rx) {
        spk_iface_num = mic_iface_num;
        test_handle_dev_connection(&mic_iface_num, &if_rx);
        TEST_ASSERT_EQUAL(if_rx, true);
    } else {
        test_handle_dev_connection(&spk_iface_num, &if_rx);
        TEST_ASSERT_EQUAL(if_rx, false);
    }

    // Open device's interface
    uac_host_device_handle_t mic_device_handle = NULL;
    test_open_mic_device(mic_iface_num, audio_buf_default.size, audio_buf_default.threshold, &mic_device_handle);

    // Get mic alt interface 1 params
    uac_host_dev_alt_param_t mic_alt_params;
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_get_device_alt_param(mic_device_handle, 1, &mic_alt_params));

    // Start and unpause the interface
    uac_host_stream_config_t stream_config = {
        .channels = mic_alt_params.channels,
        .bit_resolution = mic_alt_params.bit_resolution,
        .sample_freq = mic_alt_params.sample_freq[0],
        .flags = FLAG_STREAM_PAUSE_AFTER_START,
    };
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_start(mic_device_handle, &stream_config));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_unpause(mic_device_handle));

    // Check that data events are being sent
    TEST_ASSERT(expect_transfer_event(&expect_rx_done_evt, pdMS_TO_TICKS(TEST_EVENT_TRANSFER_WAIT_MS)));

    // Suspend the root port and expect 1 Suspend event -> only one iface is opened
    printf("Issue suspend\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());
    expect_client_event(&expect_suspend_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));

    // Check that data events are NOT being sent anymore
    TEST_ASSERT_FALSE(expect_transfer_event(NULL, pdMS_TO_TICKS(TEST_EVENT_TRANSFER_WAIT_MS)));

    // Unpause interface from an active state, while the root port is in suspended state
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_unpause(mic_device_handle));

    // Expect 1 Resume event (resume by transfer submit) -> only one iface is opened
    expect_client_event(&expect_resume_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));

    // Check that data events are being sent
    TEST_ASSERT(expect_transfer_event(&expect_rx_done_evt, pdMS_TO_TICKS(TEST_EVENT_TRANSFER_WAIT_MS)));

    // Pause, stop and close the interface
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_pause(mic_device_handle));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_stop(mic_device_handle));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_close(mic_device_handle));
}

/**
 * @brief: Test pause interface, which was previously in active state while the root por is in suspended state
 *
 * Purpose:
 *   - Reaction of the client to pausing active interface, while the root port is in suspended state
 *
 * Procedure:
 *   - Connect, open and start (unpause after start) interface
 *   - Expect data transfer events
 *   - Suspend the root port and expect suspend event
 *   - Expect no data transfer events
 *   - Pause the interface which was previously unpaused, while the root port is in suspended state
 *   - Resume the root port and expect resume event
 *   - Unpause the interface and expect data transfer events
 *   - Pause, stop and close the interface
 */
TEST_CASE("Test pause active interface from suspended state", "[uac_host][power_management][rx]")
{
    uint8_t mic_iface_num = 0;
    uint8_t spk_iface_num = 0;
    uint8_t if_rx = false;
    test_handle_dev_connection(&mic_iface_num, &if_rx);
    if (!if_rx) {
        spk_iface_num = mic_iface_num;
        test_handle_dev_connection(&mic_iface_num, &if_rx);
        TEST_ASSERT_EQUAL(if_rx, true);
    } else {
        test_handle_dev_connection(&spk_iface_num, &if_rx);
        TEST_ASSERT_EQUAL(if_rx, false);
    }

    // Open device's interface
    uac_host_device_handle_t mic_device_handle = NULL;
    test_open_mic_device(mic_iface_num, audio_buf_default.size, audio_buf_default.threshold, &mic_device_handle);

    // Get mic alt interface 1 params
    uac_host_dev_alt_param_t mic_alt_params;
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_get_device_alt_param(mic_device_handle, 1, &mic_alt_params));

    // Start and unpause the interface
    uac_host_stream_config_t stream_config = {
        .channels = mic_alt_params.channels,
        .bit_resolution = mic_alt_params.bit_resolution,
        .sample_freq = mic_alt_params.sample_freq[0],
        .flags = FLAG_STREAM_PAUSE_AFTER_START,
    };
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_start(mic_device_handle, &stream_config));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_unpause(mic_device_handle));

    // Check that data events are being sent
    TEST_ASSERT(expect_transfer_event(&expect_rx_done_evt, pdMS_TO_TICKS(TEST_EVENT_TRANSFER_WAIT_MS)));

    // Suspend the root port and expect 1 Suspend event -> only one iface is opened
    printf("Issue suspend\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());
    expect_client_event(&expect_suspend_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));

    // Check that data events are NOT being sent anymore
    TEST_ASSERT_FALSE(expect_transfer_event(NULL, pdMS_TO_TICKS(TEST_EVENT_TRANSFER_WAIT_MS)));

    // Pause previously unpaused interface, while the root port is in suspended state
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_pause(mic_device_handle));

    // Resume the root port and expect 1 Resume event -> only one iface is opened
    printf("Issue resume\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_resume());
    expect_client_event(&expect_resume_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));

    // Unpause the interface back
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_unpause(mic_device_handle));

    // Check that data events are being sent
    TEST_ASSERT(expect_transfer_event(&expect_rx_done_evt, pdMS_TO_TICKS(TEST_EVENT_TRANSFER_WAIT_MS)));

    // Pause, stop and close the interface
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_pause(mic_device_handle));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_stop(mic_device_handle));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_close(mic_device_handle));
}

/**
 * @brief Notification events used to control the RX data handling task from the test case
 */
enum test_notify_event {
    TEST_NOTIFY_RX_START,           /**< Start receiving data from device */
    TEST_NOTIFY_RX_STOP,            /**< Stop receiving data from device */
    TEST_NOTIFY_DEVICE_SUSPENDED,   /**< Device suspended, stop expecting new data from device */
    TEST_NOTIFY_FINISH,             /**< Finish the test: close device and task cleanup */
};

#define TEST_SUPEND_RESUME_HOLD_MS  1000    // Time to keep the device suspended or resumed

/**
 * @brief RX data handling task
 *
 * Open a device and start receiving data from the device's ring buffer
 * Processing task is controlled from the main task by test_notify_event
 */
static void test_rx_data_handling_task(void *args)
{
    TaskHandle_t main_task_hdl = (TaskHandle_t)args;
    uint8_t mic_iface_num = 0;
    uint8_t spk_iface_num = 0;
    uint8_t if_rx = false;
    test_handle_dev_connection(&mic_iface_num, &if_rx);
    if (!if_rx) {
        spk_iface_num = mic_iface_num;
        test_handle_dev_connection(&mic_iface_num, &if_rx);
        TEST_ASSERT_EQUAL(if_rx, true);
    } else {
        test_handle_dev_connection(&spk_iface_num, &if_rx);
        TEST_ASSERT_EQUAL(if_rx, false);
    }

    // Open device's interface
    uac_host_device_handle_t mic_device_handle = NULL;
    test_open_mic_device(mic_iface_num, audio_buf_default.size, audio_buf_default.threshold, &mic_device_handle);

    // Get mic alt interface 1 params
    uac_host_dev_alt_param_t mic_alt_params;
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_get_device_alt_param(mic_device_handle, 1, &mic_alt_params));

    // Start and pause the device
    uac_host_stream_config_t stream_config = {
        .channels = mic_alt_params.channels,
        .bit_resolution = mic_alt_params.bit_resolution,
        .sample_freq = mic_alt_params.sample_freq[0],
        .flags = FLAG_STREAM_PAUSE_AFTER_START,
    };
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_start(mic_device_handle, &stream_config));

    // Allocate RX buffer
    uint8_t *rx_buffer = (uint8_t *)calloc(1, audio_buf_default.threshold);
    TEST_ASSERT_NOT_NULL(rx_buffer);
    uint32_t rx_size = 0;

    // Notify the main task, that the device is connected and initialized
    xTaskNotifyGive(main_task_hdl);

    // Start RX data handling loop
    while (!ulTaskNotifyTakeIndexed(TEST_NOTIFY_FINISH, pdTRUE, 0)) {

        // Wait for notification from the main task to start receiving data from the device
        TEST_ASSERT_EQUAL(pdTRUE, ulTaskNotifyTakeIndexed(TEST_NOTIFY_RX_START, pdTRUE, pdMS_TO_TICKS(2000)));
        TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, uac_host_device_unpause(mic_device_handle), "Device unpause not successful");

        // Start receiving data, until the main task does not set task notification to stop receiving data
        while (!ulTaskNotifyTakeIndexed(TEST_NOTIFY_RX_STOP, pdTRUE, 0)) {
            event_queue_t transfer_event;

            if (xQueueReceive(transfer_event_queue, &transfer_event, pdMS_TO_TICKS(TEST_EVENT_TRANSFER_WAIT_MS)) == pdPASS) {

                // Validate data received from the transfer event queue
                TEST_ASSERT_EQUAL_MESSAGE(transfer_event.event_group, UAC_DEVICE_EVENT, "Incorrect event group");
                TEST_ASSERT_EQUAL_MESSAGE(transfer_event.device_evt.event, UAC_HOST_DEVICE_EVENT_RX_DONE, "Incorrect event type");

                // Read data from the device's ring buffer
                TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_read(mic_device_handle, rx_buffer, audio_buf_default.threshold, &rx_size, 0));
                TEST_ASSERT_EQUAL(audio_buf_default.threshold, rx_size);
                ESP_LOGD(TAG, "New RX event");

            } else {
                if ( pdTRUE == ulTaskNotifyTakeIndexed(TEST_NOTIFY_DEVICE_SUSPENDED, pdTRUE, 0)) {
                    // Device was suspended while receiving data, exit loop from here
                    break;
                } else {
                    TEST_FAIL_MESSAGE("RX event not received on time");
                }
            }
        }
    }

    // Cleanup
    printf("Task cleanup\n");
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, uac_host_device_pause(mic_device_handle), "Device can't be paused");
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, uac_host_device_stop(mic_device_handle), "Device can't be stopped");
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, uac_host_device_close(mic_device_handle), "Device can't be closed");
    free(rx_buffer);
    xTaskNotifyGive(main_task_hdl);
    vTaskDelete(NULL);
}

/**
 * @brief: Test suspend/resume the root port while the host is receiving data from the device
 *
 * Purpose:
 *   - Reaction of the client to continuously suspend/resume the root port while the data reception is ongoing
 *   - Concurrent task access
 *
 * Procedure:
 *   - Create data handling task, where connect, open and start (pause after start) interface
 *   - The task is controlled from the main task in the test case
 *   - The test case is repeatedly suspending and resuming the root port
 *   - The data handling task is receiving data from the device
 */
TEST_CASE("Test suspend/resume while receiving data", "[uac_host][power_management]")
{
    // Create RX data handling task
    TaskHandle_t data_handling_task_hdl = NULL;
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(test_rx_data_handling_task, "rx_data_handing", 4096, (void *)xTaskGetCurrentTaskHandle(), 4, &data_handling_task_hdl));
    TEST_ASSERT_NOT_NULL(data_handling_task_hdl);

    // Wait for the device to be connected and initialized by the data handling task
    TEST_ASSERT_EQUAL_MESSAGE(pdTRUE, ulTaskNotifyTake(true, pdMS_TO_TICKS(2000)), "Device was not initialized on time");

    // Start the RX data handling -> suspend the root port while receiving data -> resume the root port -> Start the RX data handling
    for (int i = 0; i < 5; i++) {

        // Start the data handling and keep it running for a while
        xTaskNotifyGiveIndexed(data_handling_task_hdl, TEST_NOTIFY_RX_START);
        vTaskDelay(pdMS_TO_TICKS(TEST_SUPEND_RESUME_HOLD_MS * 2));

        // Suspend the root port and wait for the suspend event
        TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());
        expect_client_event(&expect_suspend_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));    // expect suspend event

        // Notify the data handling task, that the device is suspended, to stop expecting new data
        xTaskNotifyGiveIndexed(data_handling_task_hdl, TEST_NOTIFY_DEVICE_SUSPENDED);

        // Keep the device suspended for a while
        vTaskDelay(pdMS_TO_TICKS(TEST_SUPEND_RESUME_HOLD_MS));

        // Resume the root port and wait for the resume event
        TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_resume());
        expect_client_event(&expect_resume_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));    // expect resume event
    }

    // Start the data handling again and keep it running for a while
    xTaskNotifyGiveIndexed(data_handling_task_hdl, TEST_NOTIFY_RX_START);
    vTaskDelay(pdMS_TO_TICKS(TEST_SUPEND_RESUME_HOLD_MS));

    // Stop and close the data handling
    xTaskNotifyGiveIndexed(data_handling_task_hdl, TEST_NOTIFY_RX_STOP);
    xTaskNotifyGiveIndexed(data_handling_task_hdl, TEST_NOTIFY_FINISH);

    // Wait for the cleanup of the data handling task
    TEST_ASSERT_EQUAL_MESSAGE(pdTRUE, ulTaskNotifyTake(true, pdMS_TO_TICKS(2000)), "RX data handling task was not deleted on time");
    vTaskDelay(10);
}


#define TEST_UVC_SUSPEND_TIMER_INTERVAL_MS   1000
#define TEST_UVC_SUSPEND_TIMER_MARGIN_MS     50

/**
 * @brief: Test automatic suspend timer with the UAC Host
 *
 * Purpose:
 *   - Reaction of the client to one-shot and periodic auto suspend timer
 *
 * Procedure:
 *   - Connect, open and start (unpause after start) interface
 *   - Set auto suspend timer to one-shot timer and expect suspend event within the set interval
 *   - Resume the root port and expect resume event
 *   - Set auto suspend timer to periodic timer and expect suspend events periodically
 *   - Stop periodical timer
 *   - Close the interface
 */
TEST_CASE("Automatic suspend timer", "[uac_host][power_management]")
{
    uint8_t mic_iface_num = 0;
    uint8_t spk_iface_num = 0;
    uint8_t if_rx = false;
    test_handle_dev_connection(&mic_iface_num, &if_rx);
    if (!if_rx) {
        spk_iface_num = mic_iface_num;
        test_handle_dev_connection(&mic_iface_num, &if_rx);
        TEST_ASSERT_EQUAL(if_rx, true);
    } else {
        test_handle_dev_connection(&spk_iface_num, &if_rx);
        TEST_ASSERT_EQUAL(if_rx, false);
    }

    // Open device's interface
    uac_host_device_handle_t mic_device_handle = NULL;
    test_open_mic_device(mic_iface_num, audio_buf_default.size, audio_buf_default.threshold, &mic_device_handle);

    // Get mic alt interface 1 params
    uac_host_dev_alt_param_t mic_alt_params;
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_get_device_alt_param(mic_device_handle, 1, &mic_alt_params));

    // Start the interface
    uac_host_stream_config_t stream_config = {
        .channels = mic_alt_params.channels,
        .bit_resolution = mic_alt_params.bit_resolution,
        .sample_freq = mic_alt_params.sample_freq[0],
        .flags = FLAG_STREAM_PAUSE_AFTER_START,
    };
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_start(mic_device_handle, &stream_config));

    // Set one-shot suspend timer and expect suspend event within the suspend timer interval
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_auto_suspend(USB_HOST_LIB_AUTO_SUSPEND_ONE_SHOT, TEST_UVC_SUSPEND_TIMER_INTERVAL_MS));
    expect_client_event(&expect_suspend_evt, pdMS_TO_TICKS(TEST_UVC_SUSPEND_TIMER_INTERVAL_MS + TEST_UVC_SUSPEND_TIMER_MARGIN_MS));

    // Manually resume the root port and wait for the resume event
    printf("Issue resume\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_resume());
    expect_client_event(&expect_resume_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));

    // Make sure no other event is delivered,
    // since the timer is a One-Shot timer and it shall not automatically suspend the root port again
    expect_client_event(NULL, pdMS_TO_TICKS(TEST_UVC_SUSPEND_TIMER_INTERVAL_MS * 2));

    // Set Periodic auto suspend timer
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_auto_suspend(USB_HOST_LIB_AUTO_SUSPEND_PERIODIC, TEST_UVC_SUSPEND_TIMER_INTERVAL_MS));

    // Expect auto-suspend timer event -> Resume the root port and expect the resume event -> Verify data transmit
    for (int i = 0; i < 3; i++) {
        // Expect suspend event from auto suspend timer
        expect_client_event(&expect_suspend_evt, pdMS_TO_TICKS(TEST_UVC_SUSPEND_TIMER_INTERVAL_MS + TEST_UVC_SUSPEND_TIMER_MARGIN_MS));

        // Manually resume the root port and wait for the resume event
        printf("Issue resume\n");
        TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_resume());
        expect_client_event(&expect_resume_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));

        // Verify data transmit on resumed device
        uint8_t volume;
        TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_get_volume(mic_device_handle, &volume));
    }

    // Disable the Periodic auto suspend timer
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_set_auto_suspend(USB_HOST_LIB_AUTO_SUSPEND_PERIODIC, 0));

    // Make sure no other event is delivered
    expect_client_event(NULL, pdMS_TO_TICKS(TEST_UVC_SUSPEND_TIMER_INTERVAL_MS * 2));

    // Close the interface
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_close(mic_device_handle));
}

/**
 * @brief: Test suspend/resume with sudden disconnect
 *
 * Purpose:
 *   - Test client suspend/resume event delivery when the device is disconnected from the root port in suspended state
 *
 * Procedure:
 *   - Connect and open 2 interfaces
 *   - Suspend the root port and expect 2 suspend events
 *   - Disconnect the root port and expect 2 disconnect events
 */
TEST_CASE("Test suspend/resume sudden disconnect", "[uac_host][power_management]")
{
    uint8_t mic_iface_num = 0;
    uint8_t spk_iface_num = 0;
    uint8_t if_rx = false;
    test_handle_dev_connection(&mic_iface_num, &if_rx);
    if (!if_rx) {
        spk_iface_num = mic_iface_num;
        test_handle_dev_connection(&mic_iface_num, &if_rx);
        TEST_ASSERT_EQUAL(if_rx, true);
    } else {
        test_handle_dev_connection(&spk_iface_num, &if_rx);
        TEST_ASSERT_EQUAL(if_rx, false);
    }

    // Open device
    uac_host_device_handle_t mic_device_handle = NULL, spk_device_handle = NULL;
    test_open_mic_device(mic_iface_num, audio_buf_default.size, audio_buf_default.threshold, &mic_device_handle);
    test_open_spk_device(spk_iface_num, audio_buf_default.size, audio_buf_default.threshold, &spk_device_handle);

    // Suspend the root port
    printf("Issue suspend\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());

    // Expect 2 Suspend events -> each for each interface
    expect_client_event(&expect_suspend_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));
    expect_client_event(&expect_suspend_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));

    // Sudden disconnect
    force_conn_state(false, 0);

    // Expect 2 Disconnected events -> each for each interface
    expect_client_event(&expect_disconn_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));
    expect_client_event(&expect_disconn_evt, pdMS_TO_TICKS(TEST_EVENT_CLIENT_WAIT_MS));
}

#endif // UAC_HOST_SUSPEND_RESUME_API_SUPPORTED
