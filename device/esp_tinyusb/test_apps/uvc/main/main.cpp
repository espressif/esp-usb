#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_uvc.h"

extern const uint8_t image1_start[]        asm("_binary_Image1_jpg_start");
extern const size_t image1_len             asm("Image1_jpg_length");
extern const uint8_t image2_start[]        asm("_binary_Image2_jpg_start");
extern const size_t image2_len             asm("Image2_jpg_length");

typedef struct {
    uint8_t *buffer_a;
    uint8_t *buffer_b;
    size_t size_a;
    size_t size_b;
    bool use_a;
    bool a_in_use;
    bool b_in_use;
    SemaphoreHandle_t mutex;
} ping_pong_t;

static ping_pong_t ping_pong;

static const char *TAG = "UVC";

bool fb_request_cb(int itf, uint8_t **buffer, size_t *buffer_size) {
    if (xSemaphoreTake(ping_pong.mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        if ((ping_pong.use_a == true) && (ping_pong.a_in_use == false) && (ping_pong.size_a > 0)) {
            *buffer = ping_pong.buffer_a;
            *buffer_size = ping_pong.size_a;
            ping_pong.a_in_use = true;

            ESP_LOGD(TAG, "Requesting Buffer A (size=%d), B_in_use=%d", ping_pong.size_a, ping_pong.b_in_use);

            xSemaphoreGive(ping_pong.mutex);

            return true;
        }
        else if ((ping_pong.use_a == false) && (ping_pong.b_in_use == false) && (ping_pong.size_b > 0)) {
            *buffer = ping_pong.buffer_b;
            *buffer_size = ping_pong.size_b;
            ping_pong.b_in_use = true;

            ESP_LOGD(TAG, "Requesting Buffer B (size=%d), A_in_use=%d", ping_pong.size_b, ping_pong.a_in_use);

            xSemaphoreGive(ping_pong.mutex);

            return true;
        }

        ESP_LOGD(TAG, "No buffer available! A_in_use=%d, B_in_use=%d, use_a=%d", 
                 ping_pong.a_in_use, ping_pong.b_in_use, ping_pong.use_a);
        xSemaphoreGive(ping_pong.mutex);
    }
    
    return false;
}

void fb_return_cb(int itf, uint8_t *buffer) {
    if (xSemaphoreTake(ping_pong.mutex, portMAX_DELAY) == pdTRUE) {
        if (buffer == ping_pong.buffer_a) {
            ESP_LOGD(TAG, "Returning Buffer A");
            ping_pong.a_in_use = false;
            // Wechsle zum anderen Buffer für den nächsten Request
            ping_pong.use_a = false;
        } else if (buffer == ping_pong.buffer_b) {
            ESP_LOGD(TAG, "Returning Buffer B");
            ping_pong.b_in_use = false;
            // Wechsle zum anderen Buffer für den nächsten Request
            ping_pong.use_a = true;
        } else {
            ESP_LOGE(TAG, "Unknown buffer returned: %p", buffer);
        }

        xSemaphoreGive(ping_pong.mutex);
    }
}

void streaming_start_cb(int itf, uvc_event_t *event) {
    ESP_LOGI(TAG, "Streaming started");
}

extern "C" void app_main(void) {
    esp_err_t ret;
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();

    ping_pong.buffer_a = (uint8_t*)image1_start;
    ping_pong.buffer_b = (uint8_t*)image2_start;
    ping_pong.size_a = image1_len;
    ping_pong.size_b = image2_len;
    ping_pong.use_a = true;
    ping_pong.a_in_use = false;
    ping_pong.b_in_use = false;
    ping_pong.mutex = xSemaphoreCreateMutex();

    ESP_LOGI(TAG, "Initializing USB...");

    ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB driver install failed: %d", ret);
        return;
    }

    ESP_LOGI(TAG, "TinyUSB driver installed successfully");

    tinyusb_config_uvc_t uvc_cfg = {
        .uvc_port = TINYUSB_UVC_ITF_0,
        .callback_streaming_start = streaming_start_cb,
        .callback_streaming_stop = NULL,
        .fb_request_cb = fb_request_cb,
        .fb_return_cb = fb_return_cb,
        .stop_cb = NULL,
        .uvc_buffer = (uint8_t*)image1_start,
        .uvc_buffer_size = image1_len,
    };
    
    ret = tinyusb_uvc_init(&uvc_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UVC init failed: %d", ret);
        return;
    }
    ESP_LOGI(TAG, "UVC initialized successfully");

    while(1)
    {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}