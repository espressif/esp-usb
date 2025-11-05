/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/uac_host.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "usb_audio_player";

#define USB_HOST_TASK_PRIORITY  5
#define UAC_TASK_PRIORITY       5
#define USER_TASK_PRIORITY      2

static QueueHandle_t s_event_queue = NULL;
static uac_host_device_handle_t s_spk_dev_handle = NULL;
static uac_host_device_handle_t s_mic_dev_handle = NULL;
static void uac_device_callback(uac_host_device_handle_t uac_device_handle, const uac_host_device_event_t event, void *arg);

// WAV playback resources
static TaskHandle_t s_play_task_handle = NULL;
extern const uint8_t wav_8kfile_start[] asm("_binary_output_8k_wav_start");
extern const uint8_t wav_8kfile_end[] asm("_binary_output_8k_wav_end");
extern const uint8_t wav_16kfile_start[] asm("_binary_output_16k_wav_start");
extern const uint8_t wav_16kfile_end[] asm("_binary_output_16k_wav_end");

// MIC recording resources
static uint8_t *s_mic_record_buf = NULL;
static size_t s_mic_record_buf_size = 0;     // total capacity
static size_t s_mic_record_wr = 0;           // ring write index
static bool is_playing_back = false;
static bool s_mic_recording = false;         // recording state


static bool parse_wav_header(const uint8_t *buf, size_t len, uint32_t *sample_rate,
                             uint8_t *num_channels, uint8_t *bits_per_sample,
                             const uint8_t **data_ptr, size_t *data_size)
{
    if (len < 44) {
        return false;
    }
    if (memcmp(buf + 0, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
        return false;
    }
    const uint8_t *p = buf + 12;
    const uint8_t *end = buf + len;
    uint16_t audio_format = 0;
    bool have_fmt = false;
    bool have_data = false;
    while (p + 8 <= end) {
        const uint8_t *chunk_id_ptr = p; p += 4;
        uint32_t chunk_size = *(const uint32_t *)p; p += 4;
        if (p + chunk_size > end) {
            return false;
        }
        if (memcmp(chunk_id_ptr, "fmt ", 4) == 0) {
            if (chunk_size < 16) {
                return false;
            }
            audio_format = *(const uint16_t *)(p + 0);
            *num_channels = *(const uint16_t *)(p + 2);
            *sample_rate = *(const uint32_t *)(p + 4);
            *bits_per_sample = *(const uint16_t *)(p + 14);
            have_fmt = true;
        } else if (memcmp(chunk_id_ptr, "data", 4) == 0) {
            *data_ptr = p;
            *data_size = chunk_size;
            have_data = true;
        }
        p += chunk_size + (chunk_size & 1); // chunks are word aligned
        if (have_fmt && have_data) {
            break;
        }
    }
    if (!have_fmt || !have_data) {
        return false;
    }
    if (audio_format != 1) { // PCM only
        return false;
    }
    return true;
}

static esp_err_t get_wav_data(int req_sample_freq, const uint8_t **pcm_ptr, size_t *pcm_size, uint32_t *rate, uint8_t *ch, uint8_t *bits)
{
    const uint8_t *s_wav_data = NULL;
    const uint8_t *s_wav_data_end = NULL;
    if (req_sample_freq == 8000) {
        s_wav_data = wav_8kfile_start;
        s_wav_data_end = wav_8kfile_end;
    } else if (req_sample_freq == 16000) {
        s_wav_data = wav_16kfile_start;
        s_wav_data_end = wav_16kfile_end;
    } else {
        ESP_LOGE(TAG, "Unsupported sample rate: %u", req_sample_freq);
        return ESP_FAIL;
    }
    const uint8_t *wav_start = s_wav_data;
    const size_t wav_size = (size_t)(s_wav_data_end - s_wav_data);
    if (!parse_wav_header(wav_start, wav_size, rate, ch, bits, pcm_ptr, pcm_size)) {
        ESP_LOGE(TAG, "Failed to parse embedded WAV file");
        return ESP_FAIL;
    }
    return ESP_OK;
}

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
        if (s_spk_dev_handle == NULL) {
            break;
        }
        if (config->pcm_ptr == NULL || config->pcm_size == 0) {
            vTaskDelay(10);
            continue;
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
                if (config->complete_cb) {
                    config->complete_cb();
                }
                break;
            }
            byte_offset = 0; // loop
        }
    }
    s_play_task_handle = NULL;
    is_playing_back = false;
    vTaskDelete(NULL);
}

static void start_pcm_playback(player_config_t *config)
{
    static player_config_t s_player_config;
    s_player_config = *config;
    if (s_play_task_handle == NULL) {
        BaseType_t ret = xTaskCreatePinnedToCore(pcm_play_task, "pcm_play", 4096, (void *)&s_player_config, USER_TASK_PRIORITY, &s_play_task_handle, 0);
        if (ret != pdTRUE) {
            ESP_LOGE(TAG, "Failed to create PCM play task");
        }
    }
}

/**
 * @brief event group
 *
 * APP_EVENT            - General control event
 * UAC_DRIVER_EVENT     - UAC Host Driver event, such as device connection
 * UAC_DEVICE_EVENT     - UAC Host Device event, such as rx/tx completion, device disconnection
 */
typedef enum {
    APP_EVENT = 0,
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
            uac_host_driver_event_t event;
            void *arg;
        } device_evt;
    };
} s_event_queue_t;

// removed audio_player dependent code

static void uac_device_callback(uac_host_device_handle_t uac_device_handle, const uac_host_device_event_t event, void *arg)
{
    if (event == UAC_HOST_DRIVER_EVENT_DISCONNECTED) {
        if (uac_device_handle == s_spk_dev_handle) {
            s_spk_dev_handle = NULL;
        }
        if (uac_device_handle == s_mic_dev_handle) {
            s_mic_dev_handle = NULL;
            if (s_mic_record_buf) {
                free(s_mic_record_buf);
                s_mic_record_buf = NULL;
            }
            s_mic_record_buf_size = 0;
            s_mic_record_wr = 0;
        }
        ESP_LOGI(TAG, "UAC Device disconnected");
        ESP_ERROR_CHECK(uac_host_device_close(uac_device_handle));
        return;
    }
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
    uac_host_device_resume(s_mic_dev_handle);
    s_mic_recording = true;
    s_mic_record_wr = 0;
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
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };

    ESP_ERROR_CHECK(usb_host_install(&host_config));
    ESP_LOGI(TAG, "USB Host installed");
    xTaskNotifyGive(arg);

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
                    // Parse embedded WAV data
                    player_config_t player_config = {0};
                    uac_host_stream_config_t stm_config = {0};
                    get_wav_data(iface_alt_params.sample_freq[0], &player_config.pcm_ptr, &player_config.pcm_size,
                                 &stm_config.sample_freq, &stm_config.channels, &stm_config.bit_resolution);

                    ESP_LOGI(TAG, "Start UAC speaker with %"PRIu32" Hz, %u bits, %s (PCM bytes=%u)",
                             stm_config.sample_freq, stm_config.bit_resolution, stm_config.channels == 1 ? "Mono" : "Stereo", player_config.pcm_size);
                    ESP_ERROR_CHECK(uac_host_device_start(uac_device_handle, &stm_config));
                    s_spk_dev_handle = uac_device_handle;
                    uac_host_device_set_volume(uac_device_handle, 50); // set volume
                    uac_host_device_set_mute(uac_device_handle, false); // set mute off
#ifndef CONFIG_EXAMPLE_MIC_PLAYBACK
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
                    ESP_ERROR_CHECK(uac_host_device_start(uac_device_handle, &mic_stream_config));
                    s_mic_dev_handle = uac_device_handle;
                    if (s_mic_record_buf == NULL) {
                        // Allocate a linear buffer to store ~5 seconds of PCM; round down to threshold multiple
                        uint32_t bytes_per_sec = mic_stream_config.sample_freq * mic_stream_config.channels * (mic_stream_config.bit_resolution / 8);
                        s_mic_record_buf_size = bytes_per_sec * 5;
                        s_mic_record_buf = (uint8_t *)calloc(1, s_mic_record_buf_size);
                        if (s_mic_record_buf == NULL) {
                            ESP_LOGE(TAG, "Failed to allocate MIC record buffer (%u bytes)", (unsigned)s_mic_record_buf_size);
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
                case UAC_HOST_DRIVER_EVENT_DISCONNECTED:
                    ESP_LOGI(TAG, "UAC Device disconnected");
                    if (s_play_task_handle) {
                        // let the task exit by noticing NULL handle
                        s_spk_dev_handle = NULL;
                    }
                    break;
                case UAC_HOST_DEVICE_EVENT_RX_DONE:
                    if (s_mic_dev_handle && s_mic_record_buf && s_mic_recording) {
                        uint32_t rx_size = 0;
                        esp_err_t ret = ESP_OK;
                        uac_host_device_read(s_mic_dev_handle, s_mic_record_buf + s_mic_record_wr, s_mic_record_buf_size - s_mic_record_wr, &rx_size, 0);
                        s_mic_record_wr += rx_size;
                        if (s_mic_record_wr >= s_mic_record_buf_size) {
                            s_mic_recording = false;
                            // Stop/suspend microphone streaming before playback
                            uac_host_device_suspend(s_mic_dev_handle);
                            // Prepare playback of the recorded PCM
                            player_config_t player_config = {
                                .pcm_ptr = s_mic_record_buf,
                                .pcm_size = s_mic_record_buf_size,
                                .complete_cb = mic_palyback_done_cb,
                            };
                            start_pcm_playback(&player_config);
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
            } else if (APP_EVENT == evt_queue.event_group) {
                break;
            }
        }
    }

    ESP_LOGI(TAG, "UAC Driver uninstall");
    ESP_ERROR_CHECK(uac_host_uninstall());
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
