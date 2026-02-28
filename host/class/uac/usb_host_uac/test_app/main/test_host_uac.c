/*
 * SPDX-FileCopyrightText: 2024-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "unity.h"
#include "usb/usb_host.h"
#include "usb/uac_host.h"
#include "test_host_uac_common.h"

const static char *TAG = "UAC_TEST";

// ----------------------- Public -------------------------

// Known microphone device parameters
#define UAC_DEV_PID 0x25
#define UAC_DEV_VID 0x1395

#define UAC_DEV_MIC_IFACE_NUM 3
#define UAC_DEV_MIC_IFACE_ALT_NUM 1

#define UAC_DEV_MIC_IFACE_ALT_1_CHANNELS 1
#define UAC_DEV_MIC_IFACE_ALT_1_BIT_RESOLUTION 16
#define UAC_DEV_MIC_IFACE_ALT_1_SAMPLE_FREQ_TYPE 1
#define UAC_DEV_MIC_IFACE_ALT_1_SAMPLE_FREQ_1 8000

static const uint8_t UAC_DEV_MIC_IFACE_CHANNELS_ALT[UAC_DEV_MIC_IFACE_ALT_NUM] = {UAC_DEV_MIC_IFACE_ALT_1_CHANNELS};
static const uint8_t UAC_DEV_MIC_IFACE_BIT_RESOLUTION_ALT[UAC_DEV_MIC_IFACE_ALT_NUM] = {UAC_DEV_MIC_IFACE_ALT_1_BIT_RESOLUTION};
static const uint8_t UAC_DEV_MIC_IFACE_SAMPLE_FREQ_TPYE_ALT[UAC_DEV_MIC_IFACE_ALT_NUM] = {UAC_DEV_MIC_IFACE_ALT_1_SAMPLE_FREQ_TYPE};
static const uint32_t UAC_DEV_MIC_IFACE_SAMPLE_FREQ_ALT[UAC_DEV_MIC_IFACE_ALT_NUM][UAC_DEV_MIC_IFACE_ALT_1_SAMPLE_FREQ_TYPE] = {{UAC_DEV_MIC_IFACE_ALT_1_SAMPLE_FREQ_1}};


// Known speaker device parameters
#define UAC_DEV_SPK_IFACE_NUM 4
#define UAC_DEV_SPK_IFACE_ALT_NUM 1

#define UAC_DEV_SPK_IFACE_ALT_1_CHANNELS 1
#define UAC_DEV_SPK_IFACE_ALT_1_BIT_RESOLUTION 16
#define UAC_DEV_SPK_IFACE_ALT_1_SAMPLE_FREQ_TYPE 1
#define UAC_DEV_SPK_IFACE_ALT_1_SAMPLE_FREQ_1 8000

static const uint8_t UAC_DEV_SPK_IFACE_CHANNELS_ALT[UAC_DEV_SPK_IFACE_ALT_NUM] = {UAC_DEV_SPK_IFACE_ALT_1_CHANNELS};
static const uint8_t UAC_DEV_SPK_IFACE_BIT_RESOLUTION_ALT[UAC_DEV_SPK_IFACE_ALT_NUM] = {UAC_DEV_SPK_IFACE_ALT_1_BIT_RESOLUTION};
static const uint8_t UAC_DEV_SPK_IFACE_SAMPLE_FREQ_TPYE_ALT[UAC_DEV_SPK_IFACE_ALT_NUM] = {UAC_DEV_SPK_IFACE_ALT_1_SAMPLE_FREQ_TYPE};
static const uint32_t UAC_DEV_SPK_IFACE_SAMPLE_FREQ_ALT[UAC_DEV_SPK_IFACE_ALT_NUM][UAC_DEV_SPK_IFACE_ALT_1_SAMPLE_FREQ_TYPE] = {{UAC_DEV_SPK_IFACE_ALT_1_SAMPLE_FREQ_1}};


/**
 * @brief Test with known UAC device, check if the device's parameters are parsed correctly
 * @note please modify the known device parameters if the device is changed
 */
TEST_CASE("test uac device handling", "[uac_host][known_device]")
{
    // handle device connection
    uint8_t mic_iface_num = 0;
    uint8_t spk_iface_num = 0;
    test_handle_dev_connection(&mic_iface_num, NULL);
    TEST_ASSERT_EQUAL(UAC_DEV_MIC_IFACE_NUM, mic_iface_num);
    test_handle_dev_connection(&spk_iface_num, NULL);
    TEST_ASSERT_EQUAL(UAC_DEV_SPK_IFACE_NUM, spk_iface_num);
    uint8_t test_counter = 0;

    while (++test_counter < 5) {
        // check if mic device params as expected
        uac_host_device_handle_t mic_device_handle = NULL;
        uac_host_dev_info_t dev_info;
        test_open_mic_device(mic_iface_num, 16000, 4000, &mic_device_handle);
        TEST_ASSERT_EQUAL(ESP_OK, uac_host_get_device_info(mic_device_handle, &dev_info));
        TEST_ASSERT_EQUAL(UAC_STREAM_RX, dev_info.type);
        ESP_LOGI(TAG, "UAC Device opened: MIC");
        TEST_ASSERT_EQUAL(ESP_OK, uac_host_printf_device_param(mic_device_handle));
        TEST_ASSERT_EQUAL(UAC_DEV_PID, dev_info.PID);
        TEST_ASSERT_EQUAL(UAC_DEV_VID, dev_info.VID);
        TEST_ASSERT_EQUAL(UAC_DEV_MIC_IFACE_NUM, dev_info.iface_num);
        TEST_ASSERT_EQUAL(UAC_DEV_MIC_IFACE_ALT_NUM, dev_info.iface_alt_num);
        printf("iManufacturer: %ls\n", dev_info.iManufacturer);
        printf("iProduct: %ls\n", dev_info.iProduct);
        printf("iSerialNumber: %ls\n", dev_info.iSerialNumber);
        uac_host_dev_alt_param_t iface_alt_params;
        for (int i = 0; i < dev_info.iface_alt_num; i++) {
            TEST_ASSERT_EQUAL(ESP_OK, uac_host_get_device_alt_param(mic_device_handle, i + 1, &iface_alt_params));
            TEST_ASSERT_EQUAL(UAC_DEV_MIC_IFACE_CHANNELS_ALT[i], iface_alt_params.channels);
            TEST_ASSERT_EQUAL(UAC_DEV_MIC_IFACE_BIT_RESOLUTION_ALT[i], iface_alt_params.bit_resolution);
            TEST_ASSERT_EQUAL(UAC_DEV_MIC_IFACE_SAMPLE_FREQ_TPYE_ALT[i], iface_alt_params.sample_freq_type);
            // check frequency one by one
            for (size_t j = 0; j < iface_alt_params.sample_freq_type; j++) {
                TEST_ASSERT_EQUAL(UAC_DEV_MIC_IFACE_SAMPLE_FREQ_ALT[i][j], iface_alt_params.sample_freq[j]);
            }
        }

        // check if spk device params as expected
        uac_host_device_handle_t spk_device_handle = NULL;
        test_open_spk_device(spk_iface_num, 16000, 4000, &spk_device_handle);
        TEST_ASSERT_EQUAL(ESP_OK, uac_host_get_device_info(spk_device_handle, &dev_info));
        TEST_ASSERT_EQUAL(UAC_STREAM_TX, dev_info.type);
        ESP_LOGI(TAG, "UAC Device opened: SPK");
        TEST_ASSERT_EQUAL(ESP_OK, uac_host_printf_device_param(spk_device_handle));
        TEST_ASSERT_EQUAL(UAC_DEV_PID, dev_info.PID);
        TEST_ASSERT_EQUAL(UAC_DEV_VID, dev_info.VID);
        TEST_ASSERT_EQUAL(UAC_DEV_SPK_IFACE_NUM, dev_info.iface_num);
        TEST_ASSERT_EQUAL(UAC_DEV_SPK_IFACE_ALT_NUM, dev_info.iface_alt_num);
        printf("iManufacturer: %ls\n", dev_info.iManufacturer);
        printf("iProduct: %ls\n", dev_info.iProduct);
        printf("iSerialNumber: %ls\n", dev_info.iSerialNumber);

        for (int i = 0; i < dev_info.iface_alt_num; i++) {
            TEST_ASSERT_EQUAL(ESP_OK, uac_host_get_device_alt_param(spk_device_handle, i + 1, &iface_alt_params));
            TEST_ASSERT_EQUAL(UAC_DEV_SPK_IFACE_CHANNELS_ALT[i], iface_alt_params.channels);
            TEST_ASSERT_EQUAL(UAC_DEV_SPK_IFACE_BIT_RESOLUTION_ALT[i], iface_alt_params.bit_resolution);
            TEST_ASSERT_EQUAL(UAC_DEV_SPK_IFACE_SAMPLE_FREQ_TPYE_ALT[i], iface_alt_params.sample_freq_type);
            for (size_t j = 0; j < iface_alt_params.sample_freq_type; j++) {
                TEST_ASSERT_EQUAL(UAC_DEV_SPK_IFACE_SAMPLE_FREQ_ALT[i][j], iface_alt_params.sample_freq[j]);
            }
        }

        // close the device
        test_close_device(mic_device_handle);
        test_close_device(spk_device_handle);
        // reset the queue
        test_uac_queue_reset();
    }
}

/**
 * @brief Test with known UAC device, check if the device's parameters are parsed correctly
 * @note please modify the known device parameters if the device is changed
 */
TEST_CASE("test uac device handling with known pid vid", "[uac_host][known_device]")
{
    uint8_t mic_iface_num = UAC_DEV_MIC_IFACE_NUM;
    uint8_t spk_iface_num = UAC_DEV_SPK_IFACE_NUM;
    uint8_t test_counter = 0;

    while (++test_counter < 5) {
        // check if mic device params as expected
        uac_host_device_handle_t mic_device_handle = NULL;
        uac_host_dev_info_t dev_info;
        // check if device params as expected
        uac_host_device_config_t dev_config = {
            .iface_num = mic_iface_num,
            .buffer_size = 16000,
            .buffer_threshold = 4000,
            .callback = uac_device_callback,
            .callback_arg = NULL,
        };

        do {
            esp_err_t ret = uac_host_device_open_with_vid_pid(UAC_DEV_VID, UAC_DEV_PID, &dev_config, &mic_device_handle);
            if (ret == ESP_ERR_NOT_FOUND) {
                ESP_LOGI(TAG, "Device not found, please connect the device");
                vTaskDelay(1000);
            }
        } while (mic_device_handle == NULL);

        TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_open_with_vid_pid(UAC_DEV_VID, UAC_DEV_PID, &dev_config, &mic_device_handle));
        TEST_ASSERT_EQUAL(ESP_OK, uac_host_get_device_info(mic_device_handle, &dev_info));
        TEST_ASSERT_EQUAL(UAC_STREAM_RX, dev_info.type);
        ESP_LOGI(TAG, "UAC Device opened: MIC");
        TEST_ASSERT_EQUAL(ESP_OK, uac_host_printf_device_param(mic_device_handle));
        TEST_ASSERT_EQUAL(UAC_DEV_PID, dev_info.PID);
        TEST_ASSERT_EQUAL(UAC_DEV_VID, dev_info.VID);
        TEST_ASSERT_EQUAL(UAC_DEV_MIC_IFACE_NUM, dev_info.iface_num);
        TEST_ASSERT_EQUAL(UAC_DEV_MIC_IFACE_ALT_NUM, dev_info.iface_alt_num);
        printf("iManufacturer: %ls\n", dev_info.iManufacturer);
        printf("iProduct: %ls\n", dev_info.iProduct);
        printf("iSerialNumber: %ls\n", dev_info.iSerialNumber);
        uac_host_dev_alt_param_t iface_alt_params;
        for (int i = 0; i < dev_info.iface_alt_num; i++) {
            TEST_ASSERT_EQUAL(ESP_OK, uac_host_get_device_alt_param(mic_device_handle, i + 1, &iface_alt_params));
            TEST_ASSERT_EQUAL(UAC_DEV_MIC_IFACE_CHANNELS_ALT[i], iface_alt_params.channels);
            TEST_ASSERT_EQUAL(UAC_DEV_MIC_IFACE_BIT_RESOLUTION_ALT[i], iface_alt_params.bit_resolution);
            TEST_ASSERT_EQUAL(UAC_DEV_MIC_IFACE_SAMPLE_FREQ_TPYE_ALT[i], iface_alt_params.sample_freq_type);
            // check frequency one by one
            for (size_t j = 0; j < iface_alt_params.sample_freq_type; j++) {
                TEST_ASSERT_EQUAL(UAC_DEV_MIC_IFACE_SAMPLE_FREQ_ALT[i][j], iface_alt_params.sample_freq[j]);
            }
        }

        // check if spk device params as expected
        uac_host_device_handle_t spk_device_handle = NULL;
        dev_config.iface_num = spk_iface_num;
        TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_open_with_vid_pid(UAC_DEV_VID, UAC_DEV_PID, &dev_config, &spk_device_handle));
        TEST_ASSERT_EQUAL(ESP_OK, uac_host_get_device_info(spk_device_handle, &dev_info));
        TEST_ASSERT_EQUAL(UAC_STREAM_TX, dev_info.type);
        ESP_LOGI(TAG, "UAC Device opened: SPK");
        TEST_ASSERT_EQUAL(ESP_OK, uac_host_printf_device_param(spk_device_handle));
        TEST_ASSERT_EQUAL(UAC_DEV_PID, dev_info.PID);
        TEST_ASSERT_EQUAL(UAC_DEV_VID, dev_info.VID);
        TEST_ASSERT_EQUAL(UAC_DEV_SPK_IFACE_NUM, dev_info.iface_num);
        TEST_ASSERT_EQUAL(UAC_DEV_SPK_IFACE_ALT_NUM, dev_info.iface_alt_num);
        printf("iManufacturer: %ls\n", dev_info.iManufacturer);
        printf("iProduct: %ls\n", dev_info.iProduct);
        printf("iSerialNumber: %ls\n", dev_info.iSerialNumber);

        for (int i = 0; i < dev_info.iface_alt_num; i++) {
            TEST_ASSERT_EQUAL(ESP_OK, uac_host_get_device_alt_param(spk_device_handle, i + 1, &iface_alt_params));
            TEST_ASSERT_EQUAL(UAC_DEV_SPK_IFACE_CHANNELS_ALT[i], iface_alt_params.channels);
            TEST_ASSERT_EQUAL(UAC_DEV_SPK_IFACE_BIT_RESOLUTION_ALT[i], iface_alt_params.bit_resolution);
            TEST_ASSERT_EQUAL(UAC_DEV_SPK_IFACE_SAMPLE_FREQ_TPYE_ALT[i], iface_alt_params.sample_freq_type);
            for (size_t j = 0; j < iface_alt_params.sample_freq_type; j++) {
                TEST_ASSERT_EQUAL(UAC_DEV_SPK_IFACE_SAMPLE_FREQ_ALT[i][j], iface_alt_params.sample_freq[j]);
            }
        }

        // close the device
        test_close_device(mic_device_handle);
        test_close_device(spk_device_handle);
        // reset the queue
        test_uac_queue_reset();
    }
}

/**
 * @brief record the rx stream data from microphone
 */
TEST_CASE("test uac rx reading", "[uac_host][rx]")
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

    const uint32_t buffer_threshold = 4800;
    const uint32_t buffer_size = 19200;

    uac_host_device_handle_t uac_device_handle = NULL;
    test_open_mic_device(mic_iface_num, buffer_size, buffer_threshold, &uac_device_handle);

    // start the device with first alt interface params
    uac_host_dev_alt_param_t iface_alt_params;
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_get_device_alt_param(uac_device_handle, 1, &iface_alt_params));
    const uac_host_stream_config_t stream_config = {
        .channels = iface_alt_params.channels,
        .bit_resolution = iface_alt_params.bit_resolution,
        .sample_freq = iface_alt_params.sample_freq[0],
    };
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_start(uac_device_handle, &stream_config));
    // Most device support mute and volume control. if not, comment out the following two lines
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_set_mute(uac_device_handle, 0));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_set_volume(uac_device_handle, 80));

    uint8_t *rx_buffer = (uint8_t *)calloc(1, buffer_threshold);
    TEST_ASSERT_NOT_NULL(rx_buffer);
    uint32_t rx_size = 0;
    // got 5s data, then stop the stream
    const uint32_t timeout = 5000;
    uint32_t time_counter = 0;
    event_queue_t evt_queue = {0};
    ESP_LOGI(TAG, "Start reading data from MIC");
    while (1) {
        if (xQueueReceive(client_event_queue, &evt_queue, portMAX_DELAY)) {
            TEST_ASSERT_EQUAL(UAC_DEVICE_EVENT, evt_queue.event_group);
            uac_host_device_handle_t uac_device_handle = evt_queue.device_evt.handle;
            uac_host_device_event_t event = evt_queue.device_evt.event;
            switch (event) {
            case UAC_HOST_DEVICE_EVENT_RX_DONE:
                uac_host_device_read(uac_device_handle, rx_buffer, buffer_threshold, &rx_size, 0);
                TEST_ASSERT_EQUAL(buffer_threshold, rx_size);
                time_counter += rx_size / (iface_alt_params.channels * iface_alt_params.bit_resolution / 8 * iface_alt_params.sample_freq[0] / 1000);
                if (time_counter >= timeout) {
                    goto exit_rx;
                }
                break;
            default:
                TEST_ASSERT(0);
                break;
            }
        }
    }
exit_rx:
    ESP_LOGI(TAG, "Stop reading data from MIC");
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_set_mute(uac_device_handle, 1));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_close(uac_device_handle));

    free(rx_buffer);
}

/**
 * @brief playback the wav sound to speaker, the wav will be down-sampled
 * if the device's sample frequency is not matched
 */
TEST_CASE("test uac tx writing", "[uac_host][tx]")
{
    // handle device connection
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

    // source wav file format
    const uint32_t sample_freq = 48000;
    const uint8_t channels = 2;
    const uint8_t bit_resolution = 16;
    uint32_t tx_buffer_size = 19200;
    uint32_t tx_buffer_threshold = 4800;

    uac_host_device_handle_t uac_device_handle = NULL;
    test_open_spk_device(spk_iface_num, tx_buffer_size, tx_buffer_threshold, &uac_device_handle);
    uac_host_dev_alt_param_t spk_alt_params;
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_get_device_alt_param(uac_device_handle, 1, &spk_alt_params));

    // open the device with the wav file's format
    const uac_host_stream_config_t stream_config = {
        .channels = spk_alt_params.channels,
        .bit_resolution = spk_alt_params.bit_resolution,
        .sample_freq = spk_alt_params.sample_freq[0],
        .flags = FLAG_STREAM_PAUSE_AFTER_START,
    };
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_start(uac_device_handle, &stream_config));

    extern const uint8_t wav_file_start[] asm("_binary_new_epic_wav_start");
    extern const uint8_t wav_file_end[] asm("_binary_new_epic_wav_end");

    const uint16_t *s_buffer = (uint16_t *)(wav_file_start + 44);
    uint16_t *tx_buffer = calloc(1, tx_buffer_threshold);
    TEST_ASSERT_NOT_NULL(tx_buffer);
    uint32_t tx_size = tx_buffer_threshold;

    int freq_offsite_step = sample_freq / spk_alt_params.sample_freq[0];
    int downsampling_bits = bit_resolution - spk_alt_params.bit_resolution;
    int offset_size = tx_size / (spk_alt_params.bit_resolution / 8);
    // we can only support adjust from 2 channels to 1 channel
    bool channels_adjust = channels != spk_alt_params.channels ? true : false;

    // fill the tx buffer with wav file data
    for (size_t i = 0; i < offset_size; i++) {
        tx_buffer[i] = s_buffer[i * freq_offsite_step] >> downsampling_bits;
    }

    if (downsampling_bits == 8) {
        // move buffer to the correct position
        for (size_t i = 0; i < offset_size; i++) {
            *((uint8_t *)tx_buffer + i) = *((uint8_t *)tx_buffer + i * 2);
        }
        tx_size = tx_size / 2;
    } else if (downsampling_bits) {
        ESP_LOGE(TAG, "Unsupported downsampling bits %d", downsampling_bits);
    }

    if (channels_adjust) {
        // convert stereo to mono
        if (downsampling_bits == 8) {
            tx_size = tx_size / 2;
            for (size_t i = 0; i < tx_size; i++) {
                *((uint8_t *)tx_buffer + i) = *((uint8_t *)tx_buffer + i * 2);
            }
        } else {
            tx_size = tx_size / 2;
            for (size_t i = 0; i < tx_size; i++) {
                tx_buffer[i] = tx_buffer[2 * i];
            }
        }
    }
    s_buffer += offset_size * freq_offsite_step;

    uint8_t volume = 0;
    int16_t volume_db = 0;
    uint8_t actual_volume = 0;
    bool mute = false;
    bool actual_mute = false;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, uac_host_device_write(uac_device_handle, (uint8_t *)tx_buffer, tx_size, 0));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_set_mute(uac_device_handle, mute));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_get_mute(uac_device_handle, &actual_mute));
    TEST_ASSERT_EQUAL(mute, actual_mute);
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_get_volume_db(uac_device_handle, &volume_db));
    printf("Initial Volume db: %.3f \n", (float)volume_db / 256.0);
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_set_volume_db(uac_device_handle, 0));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_get_volume_db(uac_device_handle, &volume_db));
    TEST_ASSERT_EQUAL(0, volume_db);

    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_get_volume(uac_device_handle, &actual_volume));
    volume = actual_volume;
    printf("Volume: %d \n", volume);
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_unpause(uac_device_handle));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_write(uac_device_handle, (uint8_t *)tx_buffer, tx_size, 0));

    uint8_t test_counter = 0;
    const uint8_t test_counter_max = 15;
    event_queue_t evt_queue = {0};
    while (1) {
        if (xQueueReceive(client_event_queue, &evt_queue, portMAX_DELAY)) {
            TEST_ASSERT_EQUAL(UAC_DEVICE_EVENT, evt_queue.event_group);
            uac_host_device_handle_t uac_device_handle = evt_queue.device_evt.handle;
            uac_host_device_event_t event = evt_queue.device_evt.event;
            switch (event) {
            case UAC_HOST_DEVICE_EVENT_TX_DONE:
                if ((uint32_t)(s_buffer + offset_size) > (uint32_t)wav_file_end) {
                    s_buffer = (uint16_t *)(wav_file_start + 44);
                    volume += 10;
                    if (volume > 100) {
                        volume = 10;
                    }
                    test_counter++;
                    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_set_volume(uac_device_handle, volume));
                    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_get_volume(uac_device_handle, &actual_volume));
                    TEST_ASSERT_EQUAL(volume, actual_volume);
                    printf("Volume: %d \n", volume);
                } else {
                    // fill the tx buffer with wav file data
                    tx_size = tx_buffer_threshold;
                    for (size_t i = 0; i < offset_size; i++) {
                        tx_buffer[i] = s_buffer[i * freq_offsite_step] >> downsampling_bits;
                    }
                    if (downsampling_bits == 8) {
                        // move buffer to the correct position
                        for (size_t i = 0; i < offset_size; i++) {
                            *((uint8_t *)tx_buffer + i) = *((uint8_t *)tx_buffer + i * 2);
                        }
                        tx_size = tx_size / 2;
                    } else if (downsampling_bits) {
                        ESP_LOGE(TAG, "Unsupported downsampling bits %d", downsampling_bits);
                    }
                    if (channels_adjust) {
                        // convert stereo to mono
                        if (downsampling_bits == 8) {
                            tx_size = tx_size / 2;
                            for (size_t i = 0; i < tx_size; i++) {
                                *((uint8_t *)tx_buffer + i) = *((uint8_t *)tx_buffer + i * 2);
                            }
                        } else {
                            tx_size = tx_size / 2;
                            for (size_t i = 0; i < tx_size; i++) {
                                tx_buffer[i] = tx_buffer[2 * i];
                            }
                        }
                    }
                    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_write(uac_device_handle, (uint8_t *)tx_buffer, tx_size, 1));
                    s_buffer += offset_size * freq_offsite_step;
                }
                if (test_counter > test_counter_max) {
                    goto exit_tx;
                }
                break;
            default:
                TEST_ASSERT(0);
                break;
            }
        }
    }

exit_tx:
    free(tx_buffer);
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_set_mute(uac_device_handle, 1));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_close(uac_device_handle));
}

/**
 * @brief loopback the microphone received data to speaker
 * please use a headset with microphone and speaker to test this function
 */
TEST_CASE("test uac tx rx loopback", "[uac_host][tx][rx]")
{
    // handle device connection
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

    const uint32_t rx_buffer_size = 19200;
    const uint32_t rx_buffer_threshold = 4800;

    uac_host_device_handle_t mic_device_handle = NULL;
    test_open_mic_device(mic_iface_num, rx_buffer_size, rx_buffer_threshold, &mic_device_handle);
    uac_host_device_handle_t spk_device_handle = NULL;
    // set same params to spk device
    test_open_spk_device(spk_iface_num, rx_buffer_size, rx_buffer_threshold, &spk_device_handle);

    // get mic alt interface 1 params, set same params to spk device
    uac_host_dev_alt_param_t mic_alt_params;
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_get_device_alt_param(mic_device_handle, 1, &mic_alt_params));

    uac_host_stream_config_t stream_config = {
        .channels = mic_alt_params.channels,
        .bit_resolution = mic_alt_params.bit_resolution,
        .sample_freq = mic_alt_params.sample_freq[0],
        .flags = FLAG_STREAM_PAUSE_AFTER_START,
    };

    uint8_t actual_volume = 0;
    bool actual_mute = 0;
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_start(mic_device_handle, &stream_config));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_set_mute(mic_device_handle, 0));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_get_mute(mic_device_handle, &actual_mute));
    TEST_ASSERT_EQUAL(0, actual_mute);
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_set_volume(mic_device_handle, 80));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_get_volume(mic_device_handle, &actual_volume));
    TEST_ASSERT_EQUAL(80, actual_volume);

    uac_host_dev_alt_param_t spk_alt_params;
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_get_device_alt_param(spk_device_handle, 1, &spk_alt_params));

    // some usb headset may have one channel for mic and two channels for speaker
    bool channel_mismatch = false;
    if (spk_alt_params.channels != mic_alt_params.channels) {
        if (mic_alt_params.channels == 1 && spk_alt_params.channels == 2) {
            ESP_LOGW(TAG, "Speaker channels %u and microphone channels %u are not the same", spk_alt_params.channels, mic_alt_params.channels);
            stream_config.channels = 2;
            channel_mismatch = true;
        } else {
            ESP_LOGE(TAG, "Speaker channels %u and microphone channels %u are not supported", spk_alt_params.channels, mic_alt_params.channels);
            TEST_ASSERT(0);
        }
    }

    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_start(spk_device_handle, &stream_config));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_set_mute(spk_device_handle, 0));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_get_mute(spk_device_handle, &actual_mute));
    TEST_ASSERT_EQUAL(0, actual_mute);
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_set_volume(spk_device_handle, 80));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_get_volume(spk_device_handle, &actual_volume));
    TEST_ASSERT_EQUAL(80, actual_volume);

    uint8_t *rx_buffer = (uint8_t *)calloc(1, rx_buffer_threshold);
    uint8_t *rx_buffer_stereo = NULL;
    if (channel_mismatch) {
        rx_buffer_stereo = (uint8_t *)calloc(1, rx_buffer_threshold * 2);
        TEST_ASSERT_NOT_NULL(rx_buffer_stereo);
    }
    TEST_ASSERT_NOT_NULL(rx_buffer);
    uint32_t rx_size = 0;
    // got 5s data, then stop the stream
    const uint32_t timeout = 5000;
    const uint32_t test_times = 3;
    uint32_t time_counter = 0;
    uint32_t test_counter = 0;
    event_queue_t evt_queue = {0};
    while (1) {
        TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_unpause(mic_device_handle));
        TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_unpause(spk_device_handle));
        while (1) {
            if (xQueueReceive(client_event_queue, &evt_queue, portMAX_DELAY)) {
                TEST_ASSERT_EQUAL(UAC_DEVICE_EVENT, evt_queue.event_group);
                uac_host_device_event_t event = evt_queue.device_evt.event;
                esp_err_t ret = ESP_FAIL;
                switch (event) {
                case UAC_HOST_DEVICE_EVENT_RX_DONE:
                    // read as much as possible
                    do {
                        ret = uac_host_device_read(mic_device_handle, rx_buffer, rx_buffer_threshold, &rx_size, 0);
                        if (ret == ESP_OK) {
                            if (channel_mismatch) {
                                // convert mono to stereo
                                if (mic_alt_params.bit_resolution == 16) {
                                    for (size_t i = 0; i < rx_size; i += 2) {
                                        rx_buffer_stereo[i * 2] = rx_buffer[i];
                                        rx_buffer_stereo[i * 2 + 1] = rx_buffer[i + 1];
                                        rx_buffer_stereo[i * 2 + 2] = rx_buffer[i];
                                        rx_buffer_stereo[i * 2 + 3] = rx_buffer[i + 1];
                                    }
                                    ret = uac_host_device_write(spk_device_handle, rx_buffer_stereo, rx_size * 2, 0);
                                } else {
                                    for (size_t i = 0; i < rx_size; i++) {
                                        rx_buffer_stereo[i * 2] = rx_buffer[i];
                                        rx_buffer_stereo[i * 2 + 1] = rx_buffer[i];
                                    }
                                    ret = uac_host_device_write(spk_device_handle, rx_buffer_stereo, rx_size * 2, 0);
                                }
                            } else {
                                ret = uac_host_device_write(spk_device_handle, rx_buffer, rx_size, 0);
                            }
                            time_counter += rx_size / (mic_alt_params.channels * mic_alt_params.bit_resolution / 8 * mic_alt_params.sample_freq[0] / 1000);
                        }
                    } while (ret == ESP_OK);

                    if (time_counter >= timeout) {
                        goto restart_rx;
                    }
                    break;
                case UAC_HOST_DEVICE_EVENT_TX_DONE:
                    // we do nothing here, just wait for the rx done event
                    break;
                default:
                    TEST_ASSERT(0);
                    break;
                }
            }
        }
restart_rx:
        if (++test_counter >= test_times) {
            goto exit_rx;
        }
        TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_pause(mic_device_handle));
        TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_pause(spk_device_handle));
        time_counter = 0;
        vTaskDelay(100);
    }

exit_rx:
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_set_mute(spk_device_handle, 1));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_close(spk_device_handle));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_set_mute(mic_device_handle, 1));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_close(mic_device_handle));
    free(rx_buffer);
    if (rx_buffer_stereo) {
        free(rx_buffer_stereo);
    }
}

/**
 * @brief: Test disconnect the device when the stream is running
 */
TEST_CASE("test uac tx rx loopback with disconnect", "[uac_host][tx][rx][hot-plug]")
{
    // handle device connection
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

    const uint32_t rx_buffer_size = 19200;
    const uint32_t rx_buffer_threshold = 4800;

    uac_host_device_handle_t mic_device_handle = NULL;
    test_open_mic_device(mic_iface_num, rx_buffer_size, rx_buffer_threshold, &mic_device_handle);
    uac_host_device_handle_t spk_device_handle = NULL;
    // set same params to spk device
    test_open_spk_device(spk_iface_num, rx_buffer_size, rx_buffer_threshold, &spk_device_handle);

    // get mic alt interface 1 params, set same params to spk device
    uac_host_dev_alt_param_t mic_alt_params;
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_get_device_alt_param(mic_device_handle, 1, &mic_alt_params));

    uac_host_stream_config_t stream_config = {
        .channels = mic_alt_params.channels,
        .bit_resolution = mic_alt_params.bit_resolution,
        .sample_freq = mic_alt_params.sample_freq[0],
        .flags = FLAG_STREAM_PAUSE_AFTER_START,
    };
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_start(mic_device_handle, &stream_config));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_set_mute(mic_device_handle, 0));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_set_volume(mic_device_handle, 80));

    uac_host_dev_alt_param_t spk_alt_params;
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_get_device_alt_param(spk_device_handle, 1, &spk_alt_params));

    // some usb headset may have one channel for mic and two channels for speaker
    bool channel_mismatch = false;
    if (spk_alt_params.channels != mic_alt_params.channels) {
        if (mic_alt_params.channels == 1 && spk_alt_params.channels == 2) {
            ESP_LOGW(TAG, "Speaker channels %u and microphone channels %u are not the same", spk_alt_params.channels, mic_alt_params.channels);
            stream_config.channels = 2;
            channel_mismatch = true;
        } else {
            ESP_LOGE(TAG, "Speaker channels %u and microphone channels %u are not supported", spk_alt_params.channels, mic_alt_params.channels);
            TEST_ASSERT(0);
        }
    }

    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_start(spk_device_handle, &stream_config));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_set_mute(spk_device_handle, 0));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_set_volume(spk_device_handle, 80));

    uint8_t *rx_buffer = (uint8_t *)calloc(1, rx_buffer_threshold);
    uint8_t *rx_buffer_stereo = NULL;
    if (channel_mismatch) {
        rx_buffer_stereo = (uint8_t *)calloc(1, rx_buffer_threshold * 2);
        TEST_ASSERT_NOT_NULL(rx_buffer_stereo);
    }
    TEST_ASSERT_NOT_NULL(rx_buffer);
    uint32_t rx_size = 0;
    // got 5s data, then stop the stream
    const uint32_t timeout = 1000;
    const uint32_t test_times = 2;
    uint32_t time_counter = 0;
    uint32_t test_counter = 0;
    event_queue_t evt_queue = {0};
    while (1) {
        TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_unpause(mic_device_handle));
        TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_unpause(spk_device_handle));
        while (1) {
            if (xQueueReceive(client_event_queue, &evt_queue, portMAX_DELAY)) {
                TEST_ASSERT_EQUAL(UAC_DEVICE_EVENT, evt_queue.event_group);
                uac_host_device_event_t event = evt_queue.device_evt.event;
                esp_err_t ret = ESP_FAIL;
                switch (event) {
                case UAC_HOST_DEVICE_EVENT_RX_DONE:
                    // read as much as possible
                    do {
                        ret = uac_host_device_read(mic_device_handle, rx_buffer, rx_buffer_threshold, &rx_size, 0);
                        if (ret == ESP_OK) {
                            if (channel_mismatch) {
                                // convert mono to stereo
                                if (mic_alt_params.bit_resolution == 16) {
                                    for (size_t i = 0; i < rx_size; i += 2) {
                                        rx_buffer_stereo[i * 2] = rx_buffer[i];
                                        rx_buffer_stereo[i * 2 + 1] = rx_buffer[i + 1];
                                        rx_buffer_stereo[i * 2 + 2] = rx_buffer[i];
                                        rx_buffer_stereo[i * 2 + 3] = rx_buffer[i + 1];
                                    }
                                    ret = uac_host_device_write(spk_device_handle, rx_buffer_stereo, rx_size * 2, 0);
                                } else {
                                    for (size_t i = 0; i < rx_size; i++) {
                                        rx_buffer_stereo[i * 2] = rx_buffer[i];
                                        rx_buffer_stereo[i * 2 + 1] = rx_buffer[i];
                                    }
                                    ret = uac_host_device_write(spk_device_handle, rx_buffer_stereo, rx_size * 2, 0);
                                }
                            } else {
                                ret = uac_host_device_write(spk_device_handle, rx_buffer, rx_size, 0);
                            }
                            time_counter += rx_size / (mic_alt_params.channels * mic_alt_params.bit_resolution / 8 * mic_alt_params.sample_freq[0] / 1000);
                        }
                    } while (ret == ESP_OK);

                    if (time_counter >= timeout) {
                        goto restart_rx;
                    }
                    break;
                case UAC_HOST_DEVICE_EVENT_TX_DONE:
                    // we do nothing here, just wait for the rx done event
                    break;
                default:
                    TEST_ASSERT(0);
                    break;
                }
            }
        }
restart_rx:
        if (++test_counter >= test_times) {
            goto exit_rx;
        }
        TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_pause(mic_device_handle));
        TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_pause(spk_device_handle));
        time_counter = 0;
        vTaskDelay(100);
    }

exit_rx:
    force_conn_state(false, 0);
    // Wait device be closed by callback
    vTaskDelay(500);
    free(rx_buffer);
    if (rx_buffer_stereo) {
        free(rx_buffer_stereo);
    }
}
