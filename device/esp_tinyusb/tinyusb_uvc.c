/*
 * SPDX-FileCopyrightText: 2022-2026 Espressif Systems (Shanghai) CO LTD
 *                         2026 Daniel Kampert (DanielKampert@Kampis-Elektroecke.de)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <esp_check.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_task_wdt.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <stdint.h>
#include <string.h>

#include <tusb.h>
#include <tinyusb_uvc.h>
#include <uvc.h>
#include <sdkconfig.h>

static const char *TAG = "tusb_uvc";

#define UVC_EVENT_EXIT         (1<<0)
#define UVC_EVENT_EXIT_DONE    (1<<1)

static esp_tusb_uvc_t s_uvc_obj[TINYUSB_UVC_ITF_MAX];


#if (CFG_TUD_VIDEO)
void tud_video_frame_xfer_complete_cb(uint_fast8_t ctl_idx, uint_fast8_t stm_idx)
{
    esp_tusb_uvc_t *uvc = tinyusb_uvc_get_intf(ctl_idx);

    if ((uvc == NULL) || (uvc->initialized == false)) {
        return;
    }

    ESP_LOGD(TAG, "Frame transfer complete callback, buffer=%p", uvc->current_frame_buffer);

    // Frame transfer completed - return buffer to user
    if ((uvc->current_frame_buffer != NULL) && uvc->fb_return_cb) {
        uvc->fb_return_cb(ctl_idx, uvc->current_frame_buffer);
        uvc->current_frame_buffer = NULL;
    }

    if (uvc->callback_streaming_start) {
        uvc_event_t event = {
            .type = UVC_EVENT_FRAME_END,
        };

        uvc->callback_streaming_start(ctl_idx, &event);
    }
}

int tud_video_commit_cb(uint_fast8_t ctl_idx, uint_fast8_t stm_idx, 
                        video_probe_and_commit_control_t const *parameters)
{
    esp_tusb_uvc_t *uvc = tinyusb_uvc_get_intf(ctl_idx);

    if ((uvc == NULL) || (uvc->initialized == false)) {
        return VIDEO_ERROR_UNKNOWN;
    }

    ESP_LOGD(TAG, "UVC streaming committed on interface %d", ctl_idx);
    
    if (uvc->callback_streaming_start) {
        uvc_event_t event = {
            .type = UVC_EVENT_STREAMING_START,
        };
        uvc->callback_streaming_start(ctl_idx, &event);
    }
    
    uvc->streaming = true;

    return VIDEO_ERROR_NONE;
}
#endif /* CFG_TUD_VIDEO */

esp_tusb_uvc_t *tinyusb_uvc_get_intf(tinyusb_uvc_itf_t itf)
{
    if (itf >= TINYUSB_UVC_ITF_MAX) {
        return NULL;
    }
    return &s_uvc_obj[itf];
}

static void video_task(void *arg)
{
    tinyusb_uvc_itf_t itf = (tinyusb_uvc_itf_t)(uintptr_t)arg;
    esp_tusb_uvc_t *uvc = tinyusb_uvc_get_intf(itf);
    
    if (uvc == NULL) {
        vTaskDelete(NULL);
        return;
    }

    uint32_t start_ms = 0;
    uint32_t frame_num = 0;
    bool already_start = false;

    ESP_LOGD(TAG, "UVC task started for interface %d", itf);

    esp_err_t wdt_err = esp_task_wdt_add(NULL);
    if (wdt_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to add UVC task to watchdog: %d!", wdt_err);
    }

    while (1) {
        esp_task_wdt_reset();
        
        EventBits_t uxBits = xEventGroupGetBits(uvc->event_group);
        if (uxBits & UVC_EVENT_EXIT) {
            ESP_LOGD(TAG, "UVC task exit for interface %d", itf);
            break;
        }

        if (tud_video_n_streaming(itf, 0) == false) {
            already_start = false;
            frame_num = 0;

            if (uvc->streaming) {
                uvc->streaming = false;
                if (uvc->callback_streaming_stop) {
                    uvc_event_t event = {
                        .type = UVC_EVENT_STREAMING_STOP,
                    };
                    uvc->callback_streaming_stop(itf, &event);
                }
            }

            vTaskDelay(pdMS_TO_TICKS(10));

            continue;
        }

        if (already_start == false) {
            already_start = true;
            start_ms = (uint32_t)(esp_timer_get_time() / 1000);
            ESP_LOGD(TAG, "UVC streaming started on interface %d", itf);
        }

        uint32_t cur = (uint32_t)(esp_timer_get_time() / 1000);
        if ((cur - start_ms) < (uvc->interval_ms * frame_num)) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        if ((cur - start_ms) > ((uvc->interval_ms * frame_num) + 100)) {
            start_ms = cur;
            frame_num = 0;
        }

        uint8_t *frame_buffer = NULL;
        size_t frame_size = 0;

        // Request frame buffer from user
        if (uvc->fb_request_cb && uvc->fb_request_cb(itf, &frame_buffer, &frame_size)) {
            if (frame_buffer && (frame_size > 0)) {
                // Check if we can transmit
                if (tud_video_n_frame_xfer(itf, 0, (void*)frame_buffer, frame_size)) {
                    ESP_LOGD(TAG, "Frame transfer started, buffer=%p, size=%d", frame_buffer, frame_size);
                    // Store current buffer - it will be returned in the complete callback
                    uvc->current_frame_buffer = frame_buffer;
                    frame_num++;
                } else {
                    ESP_LOGW(TAG, "Failed to transmit frame, current_buffer=%p", uvc->current_frame_buffer);
                    // Return buffer immediately if transmission failed
                    if (uvc->fb_return_cb) {
                        uvc->fb_return_cb(itf, frame_buffer);
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    esp_task_wdt_delete(NULL);
    
    xEventGroupSetBits(uvc->event_group, UVC_EVENT_EXIT_DONE);
    vTaskDelete(NULL);
}

esp_err_t tinyusb_uvc_init_itf(tinyusb_uvc_itf_t itf)
{
    ESP_RETURN_ON_FALSE(itf < TINYUSB_UVC_ITF_MAX, ESP_ERR_INVALID_ARG, TAG, "Invalid interface");
    
    esp_tusb_uvc_t *uvc = tinyusb_uvc_get_intf(itf);
    ESP_RETURN_ON_FALSE(uvc->initialized == false, ESP_ERR_INVALID_STATE, TAG, "Already initialized");

    memset(uvc, 0, sizeof(esp_tusb_uvc_t));
    
    return ESP_OK;
}

esp_err_t tinyusb_uvc_init(const tinyusb_config_uvc_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg, ESP_ERR_INVALID_ARG, TAG, "Config is NULL");
    ESP_RETURN_ON_FALSE(cfg->uvc_port < TINYUSB_UVC_ITF_MAX, ESP_ERR_INVALID_ARG, TAG, "Invalid port");

    tinyusb_uvc_itf_t itf = cfg->uvc_port;
    esp_tusb_uvc_t *uvc = tinyusb_uvc_get_intf(itf);

    ESP_RETURN_ON_ERROR(tinyusb_uvc_init_itf(itf), TAG, "Init interface failed");

    uvc->callback_streaming_start = cfg->callback_streaming_start;
    uvc->callback_streaming_stop = cfg->callback_streaming_stop;
    uvc->fb_request_cb = cfg->fb_request_cb;
    uvc->fb_return_cb = cfg->fb_return_cb;
    uvc->stop_cb = cfg->stop_cb;
    uvc->uvc_buffer = (uint8_t*)cfg->uvc_buffer;
    uvc->uvc_buffer_size = cfg->uvc_buffer_size;

#if CONFIG_TINYUSB_UVC_SUPPORT_TWO_CAM
    if (itf == TINYUSB_UVC_ITF_0) {
        uvc->interval_ms = 1000 / CONFIG_TINYUSB_UVC_CAM1_FRAMERATE;
    } else {
        uvc->interval_ms = 1000 / CONFIG_TINYUSB_UVC_CAM2_FRAMERATE;
    }
#else
    uvc->interval_ms = 1000 / CONFIG_TINYUSB_UVC_CAM1_FRAMERATE;
#endif

    uvc->event_group = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(uvc->event_group, ESP_ERR_NO_MEM, TAG, "Failed to create event group");

#if CONFIG_TINYUSB_UVC_SUPPORT_TWO_CAM
    BaseType_t core_id = (itf == TINYUSB_UVC_ITF_0) 
        ? ((CONFIG_TINYUSB_UVC_CAM1_TASK_CORE < 0) ? tskNO_AFFINITY : CONFIG_TINYUSB_UVC_CAM1_TASK_CORE)
        : ((CONFIG_TINYUSB_UVC_CAM2_TASK_CORE < 0) ? tskNO_AFFINITY : CONFIG_TINYUSB_UVC_CAM2_TASK_CORE);
    int priority = (itf == TINYUSB_UVC_ITF_0) 
        ? CONFIG_TINYUSB_UVC_CAM1_TASK_PRIORITY 
        : CONFIG_TINYUSB_UVC_CAM2_TASK_PRIORITY;
#else
    BaseType_t core_id = (CONFIG_TINYUSB_UVC_CAM1_TASK_CORE < 0) ? tskNO_AFFINITY : CONFIG_TINYUSB_UVC_CAM1_TASK_CORE;
    int priority = CONFIG_TINYUSB_UVC_CAM1_TASK_PRIORITY;
#endif

    char task_name[16];
    snprintf(task_name, sizeof(task_name), "UVC%d", itf);

    BaseType_t result = xTaskCreatePinnedToCore(
        video_task, 
        task_name, 
        4096, 
        (void*)(uintptr_t)itf, 
        priority, 
        &uvc->uvc_task_hdl, 
        core_id
    );
    
    if (result != pdPASS) {
        vEventGroupDelete(uvc->event_group);
        uvc->event_group = NULL;
        return ESP_ERR_NO_MEM;
    }
    
    uvc->initialized = true;
    ESP_LOGD(TAG, "UVC interface %d initialized", itf);
    
    return ESP_OK;
}

esp_err_t tinyusb_uvc_deinit_itf(tinyusb_uvc_itf_t itf)
{
    ESP_RETURN_ON_FALSE(itf < TINYUSB_UVC_ITF_MAX, ESP_ERR_INVALID_ARG, TAG, "Invalid interface");
    
    esp_tusb_uvc_t *uvc = tinyusb_uvc_get_intf(itf);
    ESP_RETURN_ON_FALSE(uvc->initialized, ESP_ERR_INVALID_STATE, TAG, "Not initialized");
    
    if (uvc->event_group) {
        xEventGroupSetBits(uvc->event_group, UVC_EVENT_EXIT);

        EventBits_t bits = xEventGroupWaitBits(
            uvc->event_group,
            UVC_EVENT_EXIT_DONE,
            pdTRUE,
            pdFALSE,
            pdMS_TO_TICKS(1000)
        );
        
        if ((bits & UVC_EVENT_EXIT_DONE) == false) {
            ESP_LOGW(TAG, "UVC task did not exit gracefully");
        }
        
        vEventGroupDelete(uvc->event_group);
        uvc->event_group = NULL;
    }
    
    memset(uvc, 0, sizeof(esp_tusb_uvc_t));
    
    ESP_LOGD(TAG, "UVC interface %d deinitialized", itf);
    
    return ESP_OK;
}

esp_err_t tinyusb_uvc_deinit(tinyusb_uvc_itf_t itf)
{
    return tinyusb_uvc_deinit_itf(itf);
}

bool tinyusb_uvc_streaming_active(tinyusb_uvc_itf_t itf)
{
    if (itf >= TINYUSB_UVC_ITF_MAX) {
        return false;
    }
    
    esp_tusb_uvc_t *uvc = tinyusb_uvc_get_intf(itf);
    if ((uvc == NULL) || (uvc->initialized == false)) {
        return false;
    }
    
    return uvc->streaming && tud_video_n_streaming(itf, 0);
}

esp_err_t tinyusb_uvc_transmit_frame(tinyusb_uvc_itf_t itf, uint8_t *frame, size_t len)
{
    ESP_RETURN_ON_FALSE(itf < TINYUSB_UVC_ITF_MAX, ESP_ERR_INVALID_ARG, TAG, "Invalid interface");
    ESP_RETURN_ON_FALSE(frame, ESP_ERR_INVALID_ARG, TAG, "Frame is NULL");
    ESP_RETURN_ON_FALSE(len > 0, ESP_ERR_INVALID_ARG, TAG, "Invalid length");
    
    esp_tusb_uvc_t *uvc = tinyusb_uvc_get_intf(itf);
    ESP_RETURN_ON_FALSE(uvc->initialized, ESP_ERR_INVALID_STATE, TAG, "Not initialized");
    ESP_RETURN_ON_FALSE(uvc->streaming, ESP_ERR_INVALID_STATE, TAG, "Not streaming");
    
    if (tud_video_n_frame_xfer(itf, 0, (void*)frame, len) == false) {
        return ESP_FAIL;
    }
    
    return ESP_OK;
}
