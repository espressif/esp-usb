/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <inttypes.h>
#include <stdbool.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/uac_host.h"

static const char *TAG = "usb_audio_player";

#define USB_HOST_TASK_PRIORITY  5
#define UAC_TASK_PRIORITY       5
#define USER_TASK_PRIORITY      2

static QueueHandle_t s_event_queue = NULL;
static uac_host_device_handle_t s_spk_dev_handle = NULL;
static uac_host_device_handle_t s_mic_dev_handle = NULL;

// playback resources
static TaskHandle_t s_play_task_handle = NULL;

// MIC recording resources
static uint8_t *s_mic_record_buf = NULL;
static size_t s_mic_record_buf_size = 0;     // total capacity
static size_t s_mic_record_wr = 0;           // ring write index
static bool is_playing_back = false;
static bool s_mic_recording = false;         // recording state
static volatile bool s_stop_play_request = false; // request playback stop
#ifndef CONFIG_EXAMPLE_MIC_PLAYBACK
static uint8_t s_buffer[48000];

static void generate_siren_tone(uint32_t rate, uint8_t channels, uint8_t bits,
                                uint8_t *buf, int buf_size)
{
    float volume = 0.01f;
    float high_freq = 1200.0f;
    float low_freq  = 700.0f;
    float period   = 0.3f;
    size_t frames = buf_size / (channels * (bits / 8));

    for (size_t i = 0; i < frames; i++) {
        // current time in seconds
        float t = (float)i / rate;
        // determine current frequency based on time
        float freq = fmodf(t, period * 2) < period ? high_freq : low_freq;

        if (bits == 16) {
            int16_t sample = (int16_t)(32767 * volume * sinf(2 * M_PI * freq * t));
            for (uint8_t ch = 0; ch < channels; ch++) {
                ((int16_t *)buf)[i * channels + ch] = sample;
            }
        } else if (bits == 24) {
            int32_t sample = (int32_t)(8388607 * volume * sinf(2 * M_PI * freq * t));
            for (uint8_t ch = 0; ch < channels; ch++) {
                size_t idx = (i * channels + ch) * 3;
                buf[idx]   = sample & 0xFF;
                buf[idx + 1] = (sample >> 8) & 0xFF;
                buf[idx + 2] = (sample >> 16) & 0xFF;
            }
        }
    }
}
#endif

typedef struct {
    const uint8_t *pcm_ptr;
    size_t pcm_size;
    bool is_loop;
    void (*complete_cb)(void);
} player_config_t;

static void pcm_play_task(void *arg)
{
    player_config_t *config = (player_config_t *)arg;
    size_t byte_offset = 0;
    const size_t chunk_bytes = 2048;
    ESP_LOGI(TAG, "PCM play task started");
    while (true) {
        if (s_spk_dev_handle == NULL || s_stop_play_request) {
            break;
        }
        size_t remaining = config->pcm_size - byte_offset;
        size_t bytes_to_write = remaining > chunk_bytes ? chunk_bytes : remaining;
        const uint8_t *buf = config->pcm_ptr + byte_offset;
        ESP_LOGI(TAG, "Writing %d bytes", bytes_to_write);
        esp_err_t ret = uac_host_device_write(s_spk_dev_handle, (void *)buf, bytes_to_write, 1000);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "uac_host_device_write failed: %s", esp_err_to_name(ret));
            break;
        }
        byte_offset += bytes_to_write;
        if (byte_offset >= config->pcm_size) {
            if (!config->is_loop) {
                ESP_LOGI(TAG, "PCM playback completed");
                break;
            }
            byte_offset = 0; // loop
        }
    }
    if (config->complete_cb) {
        config->complete_cb();
    }
    s_play_task_handle = NULL;
    is_playing_back = false;
    vTaskDelete(NULL);
}

static esp_err_t start_pcm_playback(player_config_t *config)
{
    ESP_RETURN_ON_FALSE(config->pcm_ptr != NULL && config->pcm_size > 0, ESP_ERR_INVALID_ARG, TAG, "Invalid PCM config");
    static player_config_t s_player_config;
    if (s_play_task_handle == NULL) {
        s_player_config = *config;
        BaseType_t ret = xTaskCreatePinnedToCore(pcm_play_task, "pcm_play", 4096, (void *)&s_player_config, USER_TASK_PRIORITY, &s_play_task_handle, 0);
        if (ret != pdTRUE) {
            ESP_LOGE(TAG, "Failed to create PCM play task");
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

/**
 * @brief event group
 *
 * UAC_DRIVER_EVENT     - UAC Host Driver event, such as device connection
 * UAC_DEVICE_EVENT     - UAC Host Device event, such as rx/tx completion, device disconnection
 */
typedef enum {
    UAC_DRIVER_EVENT,
    UAC_DEVICE_EVENT,
} event_group_t;

/**
 * @brief event queue
 *
 * This event is used for delivering the UAC Host event from callback to the uac_lib_task
 */
typedef struct {
    event_group_t event_group;
    union {
        struct {
            uint8_t addr;
            uint8_t iface_num;
            uac_host_driver_event_t event;
            void *arg;
        } driver_evt;
        struct {
            uac_host_device_handle_t handle;
            uac_host_device_event_t event;
            void *arg;
        } device_evt;
    };
} s_event_queue_t;

// removed audio_player dependent code

static void uac_device_callback(uac_host_device_handle_t uac_device_handle, const uac_host_device_event_t event, void *arg)
{
    // Send uac device event to the event queue
    s_event_queue_t evt_queue = {
        .event_group = UAC_DEVICE_EVENT,
        .device_evt.handle = uac_device_handle,
        .device_evt.event = event,
        .device_evt.arg = arg
    };
    // should not block here
    xQueueSend(s_event_queue, &evt_queue, 0);
}

static void uac_host_lib_callback(uint8_t addr, uint8_t iface_num, const uac_host_driver_event_t event, void *arg)
{
    // Send uac driver event to the event queue
    s_event_queue_t evt_queue = {
        .event_group = UAC_DRIVER_EVENT,
        .driver_evt.addr = addr,
        .driver_evt.iface_num = iface_num,
        .driver_evt.event = event,
        .driver_evt.arg = arg
    };
    xQueueSend(s_event_queue, &evt_queue, 0);
}

static void mic_palyback_done_cb(void)
{
    vTaskDelay(pdMS_TO_TICKS(1000)); // wait for a while before resuming recording
    if (s_mic_dev_handle != NULL) {
        if (uac_host_device_unpause(s_mic_dev_handle) == ESP_OK) {
            s_mic_recording = true;
            s_mic_record_wr = 0;
        } else {
            ESP_LOGE(TAG, "Failed to unpause MIC device");
        }
    }
}

/**
 * @brief Start USB Host install and handle common USB host library events while app pin not low
 *
 * @param[in] arg  Not used
 */
static void usb_lib_task(void *arg)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };

    ESP_ERROR_CHECK(usb_host_install(&host_config));
    ESP_LOGI(TAG, "USB Host installed");
    xTaskNotifyGive((TaskHandle_t)arg);

    while (true) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        // In this example, there is only one client registered
        // So, once we deregister the client, this call must succeed with ESP_OK
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_ERROR_CHECK(usb_host_device_free_all());
            break;
        }
    }

    ESP_LOGI(TAG, "USB Host shutdown");
    // Clean up USB Host
    vTaskDelay(10); // Short delay to allow clients clean-up
    ESP_ERROR_CHECK(usb_host_uninstall());
    vTaskDelete(NULL);
}

static void uac_lib_task(void *arg)
{
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uac_host_driver_config_t uac_config = {
        .create_background_task = true,
        .task_priority = UAC_TASK_PRIORITY,
        .stack_size = 4096,
        .core_id = 0,
        .callback = uac_host_lib_callback,
        .callback_arg = NULL
    };

    ESP_ERROR_CHECK(uac_host_install(&uac_config));
    ESP_LOGI(TAG, "UAC Class Driver installed");
    s_event_queue_t evt_queue = {0};
    while (1) {
        if (xQueueReceive(s_event_queue, &evt_queue, portMAX_DELAY)) {
            if (UAC_DRIVER_EVENT ==  evt_queue.event_group) {
                uac_host_driver_event_t event = evt_queue.driver_evt.event;
                uint8_t addr = evt_queue.driver_evt.addr;
                uint8_t iface_num = evt_queue.driver_evt.iface_num;
                switch (event) {
                case UAC_HOST_DRIVER_EVENT_TX_CONNECTED: {
                    uac_host_dev_info_t dev_info;
                    uac_host_device_handle_t uac_device_handle = NULL;
                    const uac_host_device_config_t dev_config = {
                        .addr = addr,
                        .iface_num = iface_num,
                        .buffer_size = 16000,
                        .buffer_threshold = 4000,
                        .callback = uac_device_callback,
                        .callback_arg = NULL,
                    };
                    ESP_ERROR_CHECK(uac_host_device_open(&dev_config, &uac_device_handle));
                    ESP_ERROR_CHECK(uac_host_get_device_info(uac_device_handle, &dev_info));
                    ESP_LOGI(TAG, "UAC Device connected: SPK");
                    uac_host_printf_device_param(uac_device_handle);
                    uac_host_dev_alt_param_t iface_alt_params;
                    uac_host_get_device_alt_param(uac_device_handle, 1, &iface_alt_params);
                    uac_host_stream_config_t stm_config = {
                        .channels = iface_alt_params.channels,
                        .bit_resolution = iface_alt_params.bit_resolution,
                        .sample_freq = iface_alt_params.sample_freq[0],
                    };
                    ESP_LOGI(TAG, "Start UAC speaker with %"PRIu32" Hz, %u bits, %s ",
                             stm_config.sample_freq, stm_config.bit_resolution, stm_config.channels == 1 ? "Mono" : "Stereo");
                    if (ESP_OK != uac_host_device_start(uac_device_handle, &stm_config)) {
                        ESP_LOGE(TAG, "Failed to start UAC device");
                        ESP_ERROR_CHECK(uac_host_device_close(uac_device_handle));
                        break;
                    }
                    s_spk_dev_handle = uac_device_handle;
                    uac_host_device_set_volume(uac_device_handle, 50); // set volume
                    uac_host_device_set_mute(uac_device_handle, false); // set mute off
#ifndef CONFIG_EXAMPLE_MIC_PLAYBACK
                    player_config_t player_config = {0};
                    generate_siren_tone(stm_config.sample_freq, stm_config.channels, stm_config.bit_resolution,
                                        s_buffer, sizeof(s_buffer));
                    player_config.pcm_ptr = s_buffer;
                    player_config.pcm_size = sizeof(s_buffer);
                    player_config.is_loop = true;
                    start_pcm_playback(&player_config); // start playback in loop
#endif
                    break;
                }
                case UAC_HOST_DRIVER_EVENT_RX_CONNECTED: {
                    ESP_LOGI(TAG, "UAC Device connected: MIC");
                    // Open MIC device and start recording into a ring buffer
#ifdef CONFIG_EXAMPLE_MIC_PLAYBACK
                    uac_host_device_handle_t uac_device_handle = NULL;
                    const uint32_t rx_buffer_size = 19200;        // internal driver buffer
                    const uac_host_device_config_t dev_config = {
                        .addr = addr,
                        .iface_num = iface_num,
                        .buffer_size = rx_buffer_size,
                        .callback = uac_device_callback,
                        .callback_arg = NULL,
                    };
                    ESP_ERROR_CHECK(uac_host_device_open(&dev_config, &uac_device_handle));
                    uac_host_dev_alt_param_t mic_alt_params;
                    ESP_ERROR_CHECK(uac_host_get_device_alt_param(uac_device_handle, 1, &mic_alt_params));
                    const uac_host_stream_config_t mic_stream_config = {
                        .channels = mic_alt_params.channels,
                        .bit_resolution = mic_alt_params.bit_resolution,
                        .sample_freq = mic_alt_params.sample_freq[0],
                    };
                    ESP_LOGI(TAG, "Start UAC microphone with %"PRIu32" Hz, %u bits, channels=%u",
                             mic_stream_config.sample_freq,
                             mic_stream_config.bit_resolution,
                             mic_stream_config.channels);
                    if (ESP_OK != uac_host_device_start(uac_device_handle, &mic_stream_config)) {
                        ESP_LOGE(TAG, "Failed to start UAC microphone");
                        ESP_ERROR_CHECK(uac_host_device_close(uac_device_handle));
                        break;
                    }
                    s_mic_dev_handle = uac_device_handle;
                    if (s_mic_record_buf == NULL) {
                        // Allocate a linear buffer to store ~5 seconds of PCM; round down to threshold multiple
                        uint32_t bytes_per_sec = mic_stream_config.sample_freq * mic_stream_config.channels * (mic_stream_config.bit_resolution / 8);
                        s_mic_record_buf_size = bytes_per_sec * 15 / 10; // 1.5 seconds
                        s_mic_record_buf = (uint8_t *)calloc(1, s_mic_record_buf_size);
                        if (s_mic_record_buf == NULL) {
                            ESP_LOGE(TAG, "Failed to allocate MIC record buffer (%u bytes)", (unsigned)s_mic_record_buf_size);
                            ESP_ERROR_CHECK(uac_host_device_close(uac_device_handle));
                            break;
                        }
                    }
                    // Initialize recording state
                    s_mic_record_wr = 0;
                    s_mic_recording = true;
#endif
                    break;
                }
                default:
                    break;
                }
            } else if (UAC_DEVICE_EVENT == evt_queue.event_group) {
                uac_host_device_event_t event = evt_queue.device_evt.event;
                switch (event) {
                case UAC_HOST_DRIVER_EVENT_DISCONNECTED: {
                    ESP_LOGI(TAG, "UAC Device disconnected");
                    uac_host_device_handle_t handle = evt_queue.device_evt.handle;
                    if (handle == s_spk_dev_handle) {
                        s_spk_dev_handle = NULL;
                    } else if (handle == s_mic_dev_handle) {
                        s_mic_dev_handle = NULL;
                        s_mic_recording = false;
                        // Stop playback if it's running
                        if (s_play_task_handle) {
                            s_stop_play_request = true;
                            while (s_play_task_handle) {
                                vTaskDelay(pdMS_TO_TICKS(10));
                            }
                            s_stop_play_request = false;
                        }
                        if (s_mic_record_buf) {
                            free(s_mic_record_buf);
                            s_mic_record_buf = NULL;
                        }
                        s_mic_record_buf_size = 0;
                        s_mic_record_wr = 0;
                    }
                    ESP_ERROR_CHECK(uac_host_device_close(handle));
                    break;
                }
                case UAC_HOST_DEVICE_EVENT_RX_DONE:
                    if (s_mic_dev_handle && s_mic_record_buf && s_mic_recording) {
                        uint32_t rx_size = 0;
                        uac_host_device_read(s_mic_dev_handle, s_mic_record_buf + s_mic_record_wr, s_mic_record_buf_size - s_mic_record_wr, &rx_size, 0);
                        ESP_LOGI(TAG, "Reading MIC %"PRIu32" bytes", rx_size);
                        s_mic_record_wr += rx_size;
                        if (s_mic_record_wr >= s_mic_record_buf_size) {
                            if (s_spk_dev_handle != NULL) {
                                s_mic_recording = false;
                                // Pause microphone streaming before playback
                                uac_host_device_pause(s_mic_dev_handle);
                                // Prepare playback of the recorded PCM
                                player_config_t player_config = {
                                    .pcm_ptr = s_mic_record_buf,
                                    .pcm_size = s_mic_record_buf_size,
                                    .complete_cb = mic_palyback_done_cb,
                                };
                                if (start_pcm_playback(&player_config) != ESP_OK) {
                                    uac_host_device_unpause(s_mic_dev_handle);
                                    s_mic_recording = true;
                                    s_mic_record_wr = 0;
                                }
                            } else {
                                s_mic_record_wr = 0;
                            }
                        }
                    }
                    break;
                case UAC_HOST_DEVICE_EVENT_TX_DONE:
                    break;
                case UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR:
                    break;
                default:
                    break;
                }
            }
        }
    }
}

void app_main(void)
{
    s_event_queue = xQueueCreate(10, sizeof(s_event_queue_t));
    assert(s_event_queue != NULL);

    static TaskHandle_t uac_task_handle = NULL;
    BaseType_t ret = xTaskCreatePinnedToCore(uac_lib_task, "uac_events", 4096, NULL,
                                             USER_TASK_PRIORITY, &uac_task_handle, 0);
    assert(ret == pdTRUE);
    ret = xTaskCreatePinnedToCore(usb_lib_task, "usb_events", 4096, (void *)uac_task_handle,
                                  USB_HOST_TASK_PRIORITY, NULL, 0);
    assert(ret == pdTRUE);
}
