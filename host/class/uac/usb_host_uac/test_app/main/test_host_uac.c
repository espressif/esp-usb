/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
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
#include "esp_private/usb_phy.h"
#include "usb/usb_host.h"
#include "usb/uac_host.h"

// USB PHY for device disconnection emulation
static usb_phy_handle_t phy_hdl = NULL;
const static char *TAG = "UAC_TEST";

// ----------------------- Public -------------------------
static EventGroupHandle_t s_evt_handle;
static QueueHandle_t s_event_queue = NULL;
static EventGroupHandle_t s_evt_handle = NULL;

#define BIT0_USB_HOST_DRIVER_REMOVED      (0x01 << 0)

// Known microphone device parameters
#define UAC_DEV_PID 0x3307
#define UAC_DEV_VID 0x349C
#define UAC_DEV_MIC_IFACE_NUM 3
#define UAC_DEV_MIC_IFACE_ALT_NUM 1
#define UAC_DEV_MIC_IFACE_ALT_1_CHANNELS 1
#define UAC_DEV_MIC_IFACE_ALT_1_BIT_RESOLUTION 16
#define UAC_DEV_MIC_IFACE_ALT_1_SAMPLE_FREQ_TYPE 1
#define UAC_DEV_MIC_IFACE_ALT_1_SAMPLE_FREQ_1 8000
static const uint32_t UAC_DEV_MIC_IFACE_ALT_SAMPLE_FREQ[UAC_DEV_MIC_IFACE_ALT_NUM][UAC_DEV_MIC_IFACE_ALT_1_SAMPLE_FREQ_TYPE] = {{UAC_DEV_MIC_IFACE_ALT_1_SAMPLE_FREQ_1}};
// Known speaker device parameters
#define UAC_DEV_SPK_IFACE_NUM 4
#define UAC_DEV_SPK_IFACE_ALT_NUM 1
#define UAC_DEV_SPK_IFACE_ALT_1_CHANNELS 1
#define UAC_DEV_SPK_IFACE_ALT_1_BIT_RESOLUTION 16
#define UAC_DEV_SPK_IFACE_ALT_1_SAMPLE_FREQ_TYPE 1
#define UAC_DEV_SPK_IFACE_ALT_1_SAMPLE_FREQ_1 8000
static const uint32_t UAC_DEV_SPK_IFACE_ALT_SAMPLE_FREQ[UAC_DEV_SPK_IFACE_ALT_NUM][UAC_DEV_SPK_IFACE_ALT_1_SAMPLE_FREQ_TYPE] = {{UAC_DEV_SPK_IFACE_ALT_1_SAMPLE_FREQ_1}};

static void force_conn_state(bool connected, TickType_t delay_ticks)
{
    TEST_ASSERT_NOT_NULL(phy_hdl);
    if (delay_ticks > 0) {
        //Delay of 0 ticks causes a yield. So skip if delay_ticks is 0.
        vTaskDelay(delay_ticks);
    }
    ESP_ERROR_CHECK(usb_phy_action(phy_hdl, (connected) ? USB_PHY_ACTION_HOST_ALLOW_CONN : USB_PHY_ACTION_HOST_FORCE_DISCONN));
}

typedef enum {
    APP_EVENT = 0,
    UAC_DRIVER_EVENT,
    UAC_DEVICE_EVENT,
} event_group_t;

typedef enum {
    DRIVER_REMOVE = 1,
} user_event_t;

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
} event_queue_t;

static void uac_device_callback(uac_host_device_handle_t uac_device_handle, const uac_host_device_event_t event, void *arg)
{
    if (event == UAC_HOST_DRIVER_EVENT_DISCONNECTED) {
        ESP_LOGI(TAG, "UAC Device disconnected");
        ESP_ERROR_CHECK(uac_host_device_close(uac_device_handle));
        return;
    }
    // Send uac device event to the event queue
    event_queue_t evt_queue = {
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
    event_queue_t evt_queue = {
        .event_group = UAC_DRIVER_EVENT,
        .driver_evt.addr = addr,
        .driver_evt.iface_num = iface_num,
        .driver_evt.event = event,
        .driver_evt.arg = arg
    };
    xQueueSend(s_event_queue, &evt_queue, 0);
}

/**
 * @brief Start USB Host install and handle common USB host library events while app pin not low
 *
 * @param[in] arg  Not used
 */
static void usb_lib_task(void *arg)
{
    // Initialize the internal USB PHY to connect to the USB OTG peripheral.
    // We manually install the USB PHY for testing
    usb_phy_config_t phy_config = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_HOST,
        .otg_speed = USB_PHY_SPEED_UNDEFINED,   //In Host mode, the speed is determined by the connected device
    };
    TEST_ASSERT_EQUAL(ESP_OK, usb_new_phy(&phy_config, &phy_hdl));

    const usb_host_config_t host_config = {
        .skip_phy_setup = true,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };

    ESP_ERROR_CHECK(usb_host_install(&host_config));
    ESP_LOGI(TAG, "USB Host installed");
    xTaskNotifyGive(arg);

    bool all_clients_gone = false;
    bool all_dev_free = false;
    while (!all_clients_gone || !all_dev_free) {
        // Start handling system events
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            printf("No more clients\n");
            usb_host_device_free_all();
            all_clients_gone = true;
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            printf("All devices freed\n");
            all_dev_free = true;
        }
    }

    ESP_LOGI(TAG, "USB Host shutdown");
    // Clean up USB Host
    vTaskDelay(10); // Short delay to allow clients clean-up
    ESP_ERROR_CHECK(usb_host_uninstall());
    ESP_ERROR_CHECK(usb_del_phy(phy_hdl)); //Tear down USB PHY
    phy_hdl = NULL;
    // set bit BIT0_USB_HOST_DRIVER_REMOVED to notify driver removed
    xEventGroupSetBits(s_evt_handle, BIT0_USB_HOST_DRIVER_REMOVED);
    vTaskDelete(NULL);
}

/**
 * @brief Setups UAC testing
 *
 * - Create USB lib task
 * - Install UAC Host driver
 */
void test_uac_setup()
{
    // create a queue to handle events
    s_event_queue = xQueueCreate(16, sizeof(event_queue_t));
    TEST_ASSERT_NOT_NULL(s_event_queue);
    s_evt_handle = xEventGroupCreate();
    TEST_ASSERT_NOT_NULL(s_evt_handle);
    static TaskHandle_t uac_task_handle = NULL;
    // create USB lib task, pass the current task handle to notify when the task is created
    TEST_ASSERT_EQUAL(pdTRUE, xTaskCreatePinnedToCore(usb_lib_task,
                      "usb_events",
                      4096,
                      xTaskGetCurrentTaskHandle(),
                      5, &uac_task_handle, 0));

    // install uac host driver
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    uac_host_driver_config_t uac_config = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = uac_host_lib_callback,
        .callback_arg = NULL
    };

    ESP_ERROR_CHECK(uac_host_install(&uac_config));
    ESP_LOGI(TAG, "UAC Class Driver installed");
}

void test_uac_queue_reset()
{
    xQueueReset(s_event_queue);
}

void test_uac_teardown(bool force)
{
    if (force) {
        force_conn_state(false, pdMS_TO_TICKS(1000));
    }
    vTaskDelay(500);
    // uninstall uac host driver
    ESP_LOGI(TAG, "UAC Driver uninstall");
    ESP_ERROR_CHECK(uac_host_uninstall());
    // Wait for USB lib task to finish
    xEventGroupWaitBits(s_evt_handle, BIT0_USB_HOST_DRIVER_REMOVED, pdTRUE, pdTRUE, portMAX_DELAY);
    // delete event queue and event group
    vQueueDelete(s_event_queue);
    vEventGroupDelete(s_evt_handle);
    // delay to allow task to delete
    vTaskDelay(100);
}

void test_open_mic_device(uac_host_device_handle_t *uac_device_handle, uint32_t buffer_size, uint32_t buffer_threshod)
{
    uac_host_dev_info_t dev_info;
    // check if device params as expected
    const uac_host_device_config_t dev_config = {
        .addr = 1,
        .iface_num = UAC_DEV_MIC_IFACE_NUM,
        .buffer_size = buffer_size,
        .buffer_threshold = buffer_threshod,
        .callback = uac_device_callback,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(uac_host_device_open(&dev_config, uac_device_handle));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_get_device_info(*uac_device_handle, &dev_info));
    TEST_ASSERT_EQUAL(UAC_STREAM_RX, dev_info.type);
    ESP_LOGI(TAG, "UAC Device opened: MIC");
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_printf_device_param(*uac_device_handle));
    TEST_ASSERT_EQUAL(UAC_DEV_MIC_IFACE_NUM, dev_info.iface_num);
    TEST_ASSERT_EQUAL(UAC_DEV_MIC_IFACE_ALT_NUM, dev_info.iface_alt_num);
    TEST_ASSERT_EQUAL(UAC_DEV_PID, dev_info.PID);
    TEST_ASSERT_EQUAL(UAC_DEV_VID, dev_info.VID);
    printf("iManufacturer: %ls\n", dev_info.iManufacturer);
    printf("iProduct: %ls\n", dev_info.iProduct);
    printf("iSerialNumber: %ls\n", dev_info.iSerialNumber);

    uac_host_dev_alt_param_t iface_alt_params;
    for (int i = 1; i <= dev_info.iface_alt_num; i++) {
        TEST_ASSERT_EQUAL(ESP_OK, uac_host_get_device_alt_param(*uac_device_handle, i, &iface_alt_params));
        TEST_ASSERT_EQUAL(UAC_DEV_MIC_IFACE_ALT_1_CHANNELS, iface_alt_params.channels);
        TEST_ASSERT_EQUAL(UAC_DEV_MIC_IFACE_ALT_1_BIT_RESOLUTION, iface_alt_params.bit_resolution);
        TEST_ASSERT_EQUAL(UAC_DEV_MIC_IFACE_ALT_1_SAMPLE_FREQ_TYPE, iface_alt_params.sample_freq_type);
        // check frequency one by one
        for (size_t j = 0; j < iface_alt_params.sample_freq_type; j++) {
            TEST_ASSERT_EQUAL(UAC_DEV_MIC_IFACE_ALT_SAMPLE_FREQ[i][j], iface_alt_params.sample_freq[j]);
        }
    }
}

void test_open_spk_device(uac_host_device_handle_t *uac_device_handle, uint32_t buffer_size, uint32_t buffer_threshod)
{
    uac_host_dev_info_t dev_info;
    // check if device params as expected
    const uac_host_device_config_t dev_config = {
        .addr = 1,
        .iface_num = UAC_DEV_SPK_IFACE_NUM,
        .buffer_size = buffer_size,
        .buffer_threshold = buffer_threshod,
        .callback = uac_device_callback,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(uac_host_device_open(&dev_config, uac_device_handle));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_get_device_info(*uac_device_handle, &dev_info));
    TEST_ASSERT_EQUAL(UAC_STREAM_TX, dev_info.type);
    ESP_LOGI(TAG, "UAC Device opened: SPK");
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_printf_device_param(*uac_device_handle));
    TEST_ASSERT_EQUAL(UAC_DEV_SPK_IFACE_NUM, dev_info.iface_num);
    TEST_ASSERT_EQUAL(UAC_DEV_SPK_IFACE_ALT_NUM, dev_info.iface_alt_num);
    TEST_ASSERT_EQUAL(UAC_DEV_PID, dev_info.PID);
    TEST_ASSERT_EQUAL(UAC_DEV_VID, dev_info.VID);
    printf("iManufacturer: %ls\n", dev_info.iManufacturer);
    printf("iProduct: %ls\n", dev_info.iProduct);
    printf("iSerialNumber: %ls\n", dev_info.iSerialNumber);

    uac_host_dev_alt_param_t iface_alt_params;
    for (int i = 1; i <= dev_info.iface_alt_num; i++) {
        TEST_ASSERT_EQUAL(ESP_OK, uac_host_get_device_alt_param(*uac_device_handle, i, &iface_alt_params));
        TEST_ASSERT_EQUAL(UAC_DEV_SPK_IFACE_ALT_1_CHANNELS, iface_alt_params.channels);
        TEST_ASSERT_EQUAL(UAC_DEV_SPK_IFACE_ALT_1_BIT_RESOLUTION, iface_alt_params.bit_resolution);
        TEST_ASSERT_EQUAL(UAC_DEV_SPK_IFACE_ALT_1_SAMPLE_FREQ_TYPE, iface_alt_params.sample_freq_type);
        for (size_t j = 0; j < iface_alt_params.sample_freq_type; j++) {
            TEST_ASSERT_EQUAL(UAC_DEV_SPK_IFACE_ALT_SAMPLE_FREQ[i][j], iface_alt_params.sample_freq[j]);
        }
    }
}

void test_close_device(uac_host_device_handle_t uac_device_handle)
{
    ESP_ERROR_CHECK(uac_host_device_close(uac_device_handle));
}

void test_handle_mic_connection()
{
    event_queue_t evt_queue = {0};
    // ignore the first connected event
    TEST_ASSERT_EQUAL(pdTRUE, xQueueReceive(s_event_queue, &evt_queue, portMAX_DELAY));
    TEST_ASSERT_EQUAL(UAC_DRIVER_EVENT, evt_queue.event_group);
    TEST_ASSERT_EQUAL(UAC_HOST_DRIVER_EVENT_CONNECTED, evt_queue.driver_evt.event);

    TEST_ASSERT_EQUAL(1, evt_queue.driver_evt.addr);
    TEST_ASSERT_EQUAL(UAC_DEV_MIC_IFACE_NUM, evt_queue.driver_evt.iface_num);

}

void test_handle_spk_connection()
{
    event_queue_t evt_queue = {0};
    // ignore the first connected event
    TEST_ASSERT_EQUAL(pdTRUE, xQueueReceive(s_event_queue, &evt_queue, portMAX_DELAY));
    TEST_ASSERT_EQUAL(UAC_DRIVER_EVENT, evt_queue.event_group);
    TEST_ASSERT_EQUAL(UAC_HOST_DRIVER_EVENT_CONNECTED, evt_queue.driver_evt.event);
    TEST_ASSERT_EQUAL(1, evt_queue.driver_evt.addr);
    TEST_ASSERT_EQUAL(UAC_DEV_SPK_IFACE_NUM, evt_queue.driver_evt.iface_num);
}

TEST_CASE("test uac device handling", "[uac_host]")
{
    test_uac_setup();
    // handle device connection
    test_handle_mic_connection();
    test_handle_spk_connection();
    uint8_t test_counter = 0;

    while (++test_counter < 5) {
        uac_host_device_handle_t mic_device_handle = NULL;
        test_open_mic_device(&mic_device_handle, 16000, 4000);
        uac_host_device_handle_t spk_device_handle = NULL;
        test_open_spk_device(&spk_device_handle, 16000, 4000);
        test_close_device(mic_device_handle);
        test_close_device(spk_device_handle);
        // reset the queue
        test_uac_queue_reset();
    }

    // Tear down test
    test_uac_teardown(true);
    // Verify the memory leackage during test environment tearDown()
}

TEST_CASE("test uac rx reading", "[uac_host][rx]")
{
    // write test for uac rx
    test_uac_setup();
    test_handle_mic_connection();
    test_handle_spk_connection();
    const uint32_t buffer_threshold = 4000;
    const uint32_t buffer_size = 16000;

    uac_host_device_handle_t uac_device_handle = NULL;
    test_open_mic_device(&uac_device_handle, buffer_size, buffer_threshold);

    const uac_host_stream_config_t stream_config = {
        .channels = UAC_DEV_MIC_IFACE_ALT_1_CHANNELS,
        .bit_resolution = UAC_DEV_MIC_IFACE_ALT_1_BIT_RESOLUTION,
        .sample_freq = UAC_DEV_MIC_IFACE_ALT_SAMPLE_FREQ[0][0],
    };
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_start(uac_device_handle, &stream_config));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_control(uac_device_handle, UAC_CTRL_UAC_MUTE, (void *)0));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_control(uac_device_handle, UAC_CTRL_UAC_VOLUME, (void *)80));

    uint8_t *rx_buffer = (uint8_t *)calloc(1, buffer_threshold);
    TEST_ASSERT_NOT_NULL(rx_buffer);
    uint32_t rx_size = 0;
    // got 5s data, then stop the stream
    const uint32_t timeout = 5000;
    uint32_t time_counter = 0;
    event_queue_t evt_queue = {0};

    while (1) {
        if (xQueueReceive(s_event_queue, &evt_queue, portMAX_DELAY)) {
            TEST_ASSERT_EQUAL(UAC_DEVICE_EVENT, evt_queue.event_group);
            uac_host_device_handle_t uac_device_handle = evt_queue.device_evt.handle;
            uac_host_device_event_t event = evt_queue.device_evt.event;
            switch (event) {
            case UAC_HOST_DEVICE_EVENT_RX_DONE:
                uac_host_device_read(uac_device_handle, rx_buffer, buffer_threshold, &rx_size, 0);
                TEST_ASSERT_EQUAL(buffer_threshold, rx_size);
                time_counter += rx_size / (UAC_DEV_MIC_IFACE_ALT_1_CHANNELS * UAC_DEV_MIC_IFACE_ALT_1_BIT_RESOLUTION / 8 * UAC_DEV_MIC_IFACE_ALT_SAMPLE_FREQ[0][0] / 1000);
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
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_control(uac_device_handle, UAC_CTRL_UAC_MUTE, (void *)1));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_stop(uac_device_handle));

    free(rx_buffer);
    test_uac_teardown(true);
}

TEST_CASE("test uac tx writing", "[uac_host][tx]")
{
    // write test for uac tx
    test_uac_setup();

    // handle device connection
    test_handle_mic_connection();
    test_handle_spk_connection();

    uint32_t tx_buffer_size = 16000;
    uint32_t tx_buffer_threshold = 4000;

    uac_host_device_handle_t uac_device_handle = NULL;
    test_open_spk_device(&uac_device_handle, tx_buffer_size, tx_buffer_threshold);

    // open the device with saved alt interface params
    const uac_host_stream_config_t stream_config = {
        .channels = UAC_DEV_SPK_IFACE_ALT_1_CHANNELS,
        .bit_resolution = UAC_DEV_SPK_IFACE_ALT_1_BIT_RESOLUTION,
        .sample_freq = UAC_DEV_SPK_IFACE_ALT_SAMPLE_FREQ[0][0],
        .flags = FLAG_STREAM_SUSPEND_AFTER_START,
    };
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_start(uac_device_handle, &stream_config));

    extern const uint8_t wave_array_32000_16_1[];
    extern const uint32_t s_buffer_size;
    const uint32_t sample_freq = UAC_DEV_SPK_IFACE_ALT_SAMPLE_FREQ[0][0];
    const uint8_t channels = UAC_DEV_SPK_IFACE_ALT_1_CHANNELS;
    const uint8_t bit_resolution = UAC_DEV_SPK_IFACE_ALT_1_BIT_RESOLUTION;
    uint16_t *s_buffer = (uint16_t *)wave_array_32000_16_1;
    int freq_offsite_step = 32000 / sample_freq;
    int downsampling_bits = 16 - bit_resolution;
    // write half buffer size data to usb speaker
    const uint32_t buffer_ms = 500;
    uint32_t buffer_size = buffer_ms * (bit_resolution / 8) * (sample_freq / 1000) * channels;
    // if 8bit spk, declare uint8_t *d_buffer
    size_t offset_size = buffer_size / (bit_resolution / 8);

    uint16_t *tx_buffer = (uint16_t *)calloc(1, buffer_size);
    TEST_ASSERT_NOT_NULL(tx_buffer);

    // fill to usb buffer
    for (size_t i = 0; i < offset_size; i++) {
        tx_buffer[i] = *(s_buffer + i * freq_offsite_step) >> downsampling_bits;
    }
    // invalid operate
    uint32_t volume = 10;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, uac_host_device_write(uac_device_handle, (uint8_t *)tx_buffer, buffer_size, 0));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_control(uac_device_handle, UAC_CTRL_UAC_MUTE, (void *)0));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_control(uac_device_handle, UAC_CTRL_UAC_VOLUME, (void *)volume));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_resume(uac_device_handle));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_write(uac_device_handle, (uint8_t *)tx_buffer, buffer_size, 0));
    s_buffer += offset_size * freq_offsite_step;

    // playback from volume 10 to 100
    const uint8_t volume_max = 100;
    uint8_t volume_step = 0;
    event_queue_t evt_queue = {0};
    while (1) {
        if (xQueueReceive(s_event_queue, &evt_queue, portMAX_DELAY)) {
            TEST_ASSERT_EQUAL(UAC_DEVICE_EVENT, evt_queue.event_group);
            uac_host_device_handle_t uac_device_handle = evt_queue.device_evt.handle;
            uac_host_device_event_t event = evt_queue.device_evt.event;
            switch (event) {
            case UAC_HOST_DEVICE_EVENT_TX_DONE:
                if ((uint32_t)(s_buffer + offset_size) >= (uint32_t)(wave_array_32000_16_1 + s_buffer_size)) {
                    s_buffer = (uint16_t *)wave_array_32000_16_1;
                    if (++volume_step >= 5) {
                        volume += 10;
                        volume_step = 0;
                        TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_control(uac_device_handle, UAC_CTRL_UAC_VOLUME, (void *)volume));
                    }
                } else {
                    // fill to usb buffer
                    for (size_t i = 0; i < offset_size; i++) {
                        tx_buffer[i] = *(s_buffer + i * freq_offsite_step) >> downsampling_bits;
                    }
                    // write to usb speaker
                    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_write(uac_device_handle, (uint8_t *)tx_buffer, buffer_size, 0));
                    s_buffer += offset_size * freq_offsite_step;
                }
                if (volume > volume_max) {
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
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_control(uac_device_handle, UAC_CTRL_UAC_MUTE, (void *)1));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_stop(uac_device_handle));

    free(tx_buffer);
    test_uac_teardown(true);
}

TEST_CASE("test uac tx rx loopback", "[uac_host][tx][rx]")
{
    test_uac_setup();

    // handle device connection
    test_handle_mic_connection();
    test_handle_spk_connection();

    const uint32_t rx_buffer_threshold = 1600;
    const uint32_t rx_buffer_size = 8000;

    uac_host_device_handle_t mic_device_handle = NULL;
    test_open_mic_device(&mic_device_handle, rx_buffer_size, rx_buffer_threshold);
    uac_host_device_handle_t spk_device_handle = NULL;
    test_open_spk_device(&spk_device_handle, rx_buffer_size, rx_buffer_threshold);

    const uint8_t channels = UAC_DEV_MIC_IFACE_ALT_1_CHANNELS;
    const uint8_t bit_resolution = UAC_DEV_MIC_IFACE_ALT_1_BIT_RESOLUTION;
    const uint32_t sample_freq = UAC_DEV_MIC_IFACE_ALT_SAMPLE_FREQ[0][0];

    const uac_host_stream_config_t stream_config = {
        .channels = channels,
        .bit_resolution = bit_resolution,
        .sample_freq = sample_freq,
        .flags = FLAG_STREAM_SUSPEND_AFTER_START,
    };

    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_start(mic_device_handle, &stream_config));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_control(mic_device_handle, UAC_CTRL_UAC_MUTE, (void *)0));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_control(mic_device_handle, UAC_CTRL_UAC_VOLUME, (void *)80));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_start(spk_device_handle, &stream_config));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_control(spk_device_handle, UAC_CTRL_UAC_MUTE, (void *)0));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_control(spk_device_handle, UAC_CTRL_UAC_VOLUME, (void *)80));

    uint8_t *rx_buffer = (uint8_t *)calloc(1, rx_buffer_threshold);
    TEST_ASSERT_NOT_NULL(rx_buffer);
    uint32_t rx_size = 0;
    // got 10s data, then stop the stream
    const uint32_t timeout = 5000;
    const uint32_t test_times = 5;
    uint32_t time_counter = 0;
    uint32_t test_counter = 0;
    event_queue_t evt_queue = {0};
    while (1) {
        TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_resume(mic_device_handle));
        TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_resume(spk_device_handle));
        while (1) {
            if (xQueueReceive(s_event_queue, &evt_queue, portMAX_DELAY)) {
                TEST_ASSERT_EQUAL(UAC_DEVICE_EVENT, evt_queue.event_group);
                uac_host_device_event_t event = evt_queue.device_evt.event;
                esp_err_t ret = ESP_FAIL;
                switch (event) {
                case UAC_HOST_DEVICE_EVENT_RX_DONE:
                    // read as much as possible
                    do {
                        ret = uac_host_device_read(mic_device_handle, rx_buffer, rx_buffer_threshold, &rx_size, 0);
                        if (ret == ESP_OK) {
                            ret = uac_host_device_write(spk_device_handle, rx_buffer, rx_size, 0);
                            time_counter += rx_size / (channels * bit_resolution / 8 * sample_freq / 1000);
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
        TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_suspend(mic_device_handle));
        TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_suspend(spk_device_handle));
        vTaskDelay(100);
    }

exit_rx:
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_control(spk_device_handle, UAC_CTRL_UAC_MUTE, (void *)1));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_stop(spk_device_handle));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_control(mic_device_handle, UAC_CTRL_UAC_MUTE, (void *)1));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_stop(mic_device_handle));
    free(rx_buffer);
    test_uac_teardown(true);
}
