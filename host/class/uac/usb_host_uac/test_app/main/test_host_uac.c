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
#include "esp_idf_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "unity.h"
#include "esp_private/usb_phy.h"
#include "usb/usb_host.h"
#include "usb/uac_host.h"

const static char *TAG = "UAC_TEST";

// ----------------------- Public -------------------------
static EventGroupHandle_t s_evt_handle;
static QueueHandle_t s_transfer_event_queue = NULL;
static QueueHandle_t s_client_event_queue = NULL;
static EventGroupHandle_t s_evt_handle = NULL;

#define BIT0_USB_HOST_DRIVER_REMOVED      (0x01 << 0)

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
            uac_host_driver_event_t event;
            uint8_t addr;
            uint8_t iface_num;
            void *arg;
        } driver_evt;
        struct {
            uac_host_device_event_t event;
            uac_host_device_handle_t handle;
            void *arg;
        } device_evt;
    };
} event_queue_t;

// usb_host_lib_set_root_port_power is used to force toggle connection, primary developed for esp32p4
// esp32p4 is supported from IDF 5.3
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 3, 0)
static usb_phy_handle_t phy_hdl = NULL;

// Force connection/disconnection using PHY
static void force_conn_state(bool connected, TickType_t delay_ticks)
{
    TEST_ASSERT_NOT_EQUAL(NULL, phy_hdl);
    if (delay_ticks > 0) {
        // Delay of 0 ticks causes a yield. So skip if delay_ticks is 0.
        vTaskDelay(delay_ticks);
    }
    ESP_ERROR_CHECK(usb_phy_action(phy_hdl, (connected) ? USB_PHY_ACTION_HOST_ALLOW_CONN : USB_PHY_ACTION_HOST_FORCE_DISCONN));
}

// Initialize the internal USB PHY to connect to the USB OTG peripheral. We manually install the USB PHY for testing
static bool install_phy(void)
{
    usb_phy_config_t phy_config = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_HOST,
        .otg_speed = USB_PHY_SPEED_UNDEFINED,   // In Host mode, the speed is determined by the connected device
    };
    TEST_ASSERT_EQUAL(ESP_OK, usb_new_phy(&phy_config, &phy_hdl));
    // Return true, to skip_phy_setup during the usb_host_install()
    return true;
}

static void delete_phy(void)
{
    TEST_ASSERT_EQUAL(ESP_OK, usb_del_phy(phy_hdl)); // Tear down USB PHY
    phy_hdl = NULL;
}
#else // ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 3, 0)

// Force connection/disconnection using root port power
static void force_conn_state(bool connected, TickType_t delay_ticks)
{
    if (delay_ticks > 0) {
        // Delay of 0 ticks causes a yield. So skip if delay_ticks is 0.
        vTaskDelay(delay_ticks);
    }
    ESP_ERROR_CHECK(usb_host_lib_set_root_port_power(connected));
}

static bool install_phy(void)
{
    // Return false, NOT to skip_phy_setup during the usb_host_install()
    return false;
}

static void delete_phy(void) {}
#endif // ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 3, 0)


static void uac_device_callback(uac_host_device_handle_t uac_device_handle, const uac_host_device_event_t event, void *arg)
{
    // Send uac device event to the event queue
    event_queue_t evt_queue = {
        .event_group = UAC_DEVICE_EVENT,
        .device_evt.handle = uac_device_handle,
        .device_evt.event = event,
        .device_evt.arg = arg
    };

    bool transfer_evt_queue = false;
    bool client_evt_queue = false;

    switch (event) {
    case UAC_HOST_DRIVER_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "UAC Device disconnected");
        TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_close(uac_device_handle));
        client_evt_queue = true;
        break;
#ifdef UAC_HOST_SUSPEND_RESUME_API_SUPPORTED
    case UAC_HOST_DEVICE_EVENT_SUSPENDED:
        ESP_LOGI(TAG, "Device suspended");
        client_evt_queue = true;
        break;
    case UAC_HOST_DEVICE_EVENT_RESUMED:
        ESP_LOGI(TAG, "Device resumed");
        client_evt_queue = true;
        break;
#endif // UAC_HOST_SUSPEND_RESUME_API_SUPPORTED
    case UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR:
    case UAC_HOST_DEVICE_EVENT_RX_DONE:
    case UAC_HOST_DEVICE_EVENT_TX_DONE:
        transfer_evt_queue = true;
        break;
    default:
        TEST_FAIL_MESSAGE("Unrecognized device event");
        break;
    }

    // Sanity check, logical XOR
    assert(transfer_evt_queue != client_evt_queue);

    if (transfer_evt_queue) {
        xQueueSend(s_transfer_event_queue, &evt_queue, 0);
    } else {
        xQueueSend(s_client_event_queue, &evt_queue, 0);
    }
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

    switch (event) {
    case UAC_HOST_DRIVER_EVENT_RX_CONNECTED:
        ESP_LOGI(TAG, "RX Device connected");
        break;
    case UAC_HOST_DRIVER_EVENT_TX_CONNECTED:
        ESP_LOGI(TAG, "TX Device connected");
        break;
    default:
        TEST_FAIL_MESSAGE("Unrecognized driver event");
    }

    xQueueSend(s_client_event_queue, &evt_queue, 0);
}

#ifdef UAC_HOST_SUSPEND_RESUME_API_SUPPORTED
/**
 * @brief Expect USB Host client event
 *
 * @param[in] expected_client_event  Expected client event (NULL to not expect any event)
 * @param[in] ticks  Ticks to wait
 */
static void expect_client_event(const event_queue_t *expected_client_event, TickType_t ticks)
{
    event_queue_t client_event;

    // Check, that no event is delivered
    if (expected_client_event == NULL) {
        if (pdFALSE == xQueueReceive(s_client_event_queue, &client_event, ticks)) {
            return;
        } else {
            TEST_FAIL_MESSAGE("Expecting NO client event, but an event delivered");
        }
    }

    // Check that an event is delivered
    if (pdTRUE == xQueueReceive(s_client_event_queue, &client_event, ticks)) {

        // Check event group
        TEST_ASSERT_EQUAL_MESSAGE(expected_client_event->event_group, client_event.event_group, "Unexpected event group");

        // Check event type according to event group
        if (expected_client_event->event_group == UAC_DRIVER_EVENT) {
            TEST_ASSERT_EQUAL_MESSAGE(expected_client_event->driver_evt.event, client_event.driver_evt.event, "Unexpected driver event");
        } else if (expected_client_event->event_group == UAC_DEVICE_EVENT) {
            TEST_ASSERT_EQUAL_MESSAGE(expected_client_event->device_evt.event, client_event.device_evt.event, "Unexpected device event");
        } else {
            // Event group not set, fail the test
            TEST_FAIL_MESSAGE("Client event group not set");
        }
    } else {
        TEST_FAIL_MESSAGE("Client event not generated on time");
    }
}

/**
 * @brief Expect USB Host transfer event
 *
 * @param[in] expected_transfer_event  Expected transfer event (data RX/TX) (NULL to not expect any event)
 * @param[in] ticks  Ticks to wait
 * @return Count of delivered events
 */
static size_t expect_transfer_event(const event_queue_t *expected_transfer_event, TickType_t ticks)
{
    event_queue_t transfer_event;
    size_t events_delivered = 0;

    // Check, that no event is delivered
    if (expected_transfer_event == NULL) {
        if (pdFALSE == xQueueReceive(s_transfer_event_queue, &transfer_event, ticks)) {
            return 0;
        } else {
            TEST_FAIL_MESSAGE("Expecting NO transfer event, but an event delivered");
        }
    }

    TEST_ASSERT_EQUAL_MESSAGE(expected_transfer_event->event_group, UAC_DEVICE_EVENT, "Only device events are allowed");
    TimeOut_t queue_timeout;
    TickType_t remaining_ticks = ticks;
    vTaskSetTimeOutState(&queue_timeout);

    do {
        // Check that an event is delivered
        if (pdTRUE == xQueueReceive(s_transfer_event_queue, &transfer_event, remaining_ticks)) {
            TEST_ASSERT_EQUAL_MESSAGE(expected_transfer_event->device_evt.event, transfer_event.device_evt.event, "Unexpected transfer event");
            events_delivered++;
        } else {
            // Only fail if no event was delivered
            TEST_ASSERT_MESSAGE(events_delivered, "Transfer event not generated on time");
        }

    } while (xTaskCheckForTimeOut(&queue_timeout, &remaining_ticks) == pdFALSE);

    return events_delivered;
}
#endif // UAC_HOST_SUSPEND_RESUME_API_SUPPORTED

/**
 * @brief Start USB Host install and handle common USB host library events while app pin not low
 *
 * @param[in] arg  Not used
 */
static void usb_lib_task(void *arg)
{
    const bool skip_phy_setup = install_phy();
    const usb_host_config_t host_config = {
        .skip_phy_setup = skip_phy_setup,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };

    TEST_ASSERT_EQUAL(ESP_OK, usb_host_install(&host_config));
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
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_uninstall());
    delete_phy(); //Tear down USB PHY
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
void test_uac_setup(void)
{
    // create a queue to handle events
    s_client_event_queue = xQueueCreate(8, sizeof(event_queue_t));
    TEST_ASSERT_NOT_NULL(s_client_event_queue);
    s_transfer_event_queue = xQueueCreate(16, sizeof(event_queue_t));
    TEST_ASSERT_NOT_NULL(s_transfer_event_queue);
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

    TEST_ASSERT_EQUAL(ESP_OK, uac_host_install(&uac_config));
    ESP_LOGI(TAG, "UAC Class Driver installed");
}

void test_uac_queue_reset(void)
{
    xQueueReset(s_client_event_queue);
    xQueueReset(s_transfer_event_queue);
}

void test_uac_teardown(bool force)
{
    if (force) {
        force_conn_state(false, pdMS_TO_TICKS(1000));
    }
    vTaskDelay(500);
    // uninstall uac host driver
    ESP_LOGI(TAG, "UAC Driver uninstall");
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_uninstall());
    // Wait for USB lib task to finish
    xEventGroupWaitBits(s_evt_handle, BIT0_USB_HOST_DRIVER_REMOVED, pdTRUE, pdTRUE, portMAX_DELAY);
    // delete event queue and event group
    vQueueDelete(s_client_event_queue);
    vQueueDelete(s_transfer_event_queue);
    vEventGroupDelete(s_evt_handle);
    // delay to allow task to delete
    vTaskDelay(100);
}

void test_open_mic_device(uint8_t iface_num, uint32_t buffer_size, uint32_t buffer_threshold, uac_host_device_handle_t *uac_device_handle)
{
    // check if device params as expected
    const uac_host_device_config_t dev_config = {
        .addr = 1,
        .iface_num = iface_num,
        .buffer_size = buffer_size,
        .buffer_threshold = buffer_threshold,
        .callback = uac_device_callback,
        .callback_arg = NULL,
    };
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_open(&dev_config, uac_device_handle));
}

void test_open_spk_device(uint8_t iface_num, uint32_t buffer_size, uint32_t buffer_threshold, uac_host_device_handle_t *uac_device_handle)
{
    // check if device params as expected
    const uac_host_device_config_t dev_config = {
        .addr = 1,
        .iface_num = iface_num,
        .buffer_size = buffer_size,
        .buffer_threshold = buffer_threshold,
        .callback = uac_device_callback,
        .callback_arg = NULL,
    };
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_open(&dev_config, uac_device_handle));
}

void test_close_device(uac_host_device_handle_t uac_device_handle)
{
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_close(uac_device_handle));
}

void test_handle_dev_connection(uint8_t *iface_num, uint8_t *if_rx)
{
    event_queue_t evt_queue = {0};
    // ignore the first connected event
    TEST_ASSERT_EQUAL(pdTRUE, xQueueReceive(s_client_event_queue, &evt_queue, portMAX_DELAY));
    TEST_ASSERT_EQUAL(UAC_DRIVER_EVENT, evt_queue.event_group);
    TEST_ASSERT_EQUAL(1, evt_queue.driver_evt.addr);
    if (iface_num) {
        *iface_num = evt_queue.driver_evt.iface_num;
    }
    if (if_rx) {
        *if_rx = evt_queue.driver_evt.event == UAC_HOST_DRIVER_EVENT_RX_CONNECTED ? 1 : 0;
    }
}

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
        if (xQueueReceive(s_client_event_queue, &evt_queue, portMAX_DELAY)) {
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
        if (xQueueReceive(s_client_event_queue, &evt_queue, portMAX_DELAY)) {
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
            if (xQueueReceive(s_client_event_queue, &evt_queue, portMAX_DELAY)) {
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
            if (xQueueReceive(s_client_event_queue, &evt_queue, portMAX_DELAY)) {
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

#ifdef UAC_HOST_SUSPEND_RESUME_API_SUPPORTED

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

//// Expect TX done event
//static const event_queue_t expect_tx_done_evt = {
//    .event_group = UAC_DEVICE_EVENT,
//    .device_evt.event = UAC_HOST_DEVICE_EVENT_TX_DONE,
//};

typedef struct {
    const uint32_t threshold;
    const uint32_t size;
} uac_dev_buf_params_t;

static uac_dev_buf_params_t test_buf_params = {
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
 *   - Connect and open 2 interfaces
 *   - Suspend the root port and expect 2 suspend events
 *   - Resume the root port and expect 2 resume events
 *   - Close both devices
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

    // Open device
    uac_host_device_handle_t mic_device_handle = NULL, spk_device_handle = NULL;
    test_open_mic_device(mic_iface_num, test_buf_params.size, test_buf_params.threshold, &mic_device_handle);
    test_open_spk_device(spk_iface_num, test_buf_params.size, test_buf_params.threshold, &spk_device_handle);

    // Suspend the root port
    printf("Issue suspend\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());

    expect_client_event(&expect_suspend_evt, 100);    // Mic device is opened by client -> expect event
    expect_client_event(&expect_suspend_evt, 100);    // Spk device is opened by client -> expect event

    // Stay suspended for a while
    vTaskDelay(100);

    // Resume the root port
    printf("Issue resume\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_resume());

    // Expect 2 Resume events -> each for each interface
    expect_client_event(&expect_resume_evt, 100);    // Mic device is opened by client -> expect event
    expect_client_event(&expect_resume_evt, 100);    // Spk device is opened by client -> expect event

    // Close both devices
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
 *   - Connect and open one interface
 *   - Suspend the root port and expect 1 suspend event
 *   - Resume the root port and expect 1 resume event
 *   - Close device
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

    // Open device
    uac_host_device_handle_t mic_device_handle = NULL;
    test_open_mic_device(mic_iface_num, test_buf_params.size, test_buf_params.threshold, &mic_device_handle);

    // Suspend the root port
    printf("Issue suspend\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());

    // Expect 1 Suspend event -> only one iface is opened
    expect_client_event(&expect_suspend_evt, 100);  // Mic device is opened by client -> expect event
    expect_client_event(NULL, 100);                 // Spk device not opened by client -> expect NO event

    // Stay suspended for a while
    vTaskDelay(100);

    // Resume the root port
    printf("Issue resume\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_resume());

    // Expect 1 Resume event -> only one iface is opened
    expect_client_event(&expect_resume_evt, 100);    // Mic device is opened by client -> expect event
    expect_client_event(NULL, 100);                 // Spk device not opened by client -> expect NO event

    // Close the mic device
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_close(mic_device_handle));
}

/**
 * @brief: Test suspend/resume signaling no device opened
 *
 * Purpose:
 *   - Test client suspend/resume event delivery with no device opened
 *
 * Procedure:
 *   - Connect 2 interfaces, don't open them
 *   - Suspend the root port and expect no events
 *   - Resume the root port and expect no events
 */
TEST_CASE("Test suspend/resume no device opened", "[uac_host][power_management]")
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
    expect_client_event(NULL, 100);    // No interface opened by client -> expect NO event

    // Resume the root port
    printf("Issue resume\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_resume());

    // Expect NO Resume event -> No interface was opened by the client
    expect_client_event(NULL, 100);    // No interface opened by client -> expect NO event
}

/**
 * @brief: Test close device which was previously opened by the client while in suspended state
 *
 * Purpose:
 *   - Test client reaction to closing device while in suspended state
 *
 * Procedure:
 *   - Connect and open one interface
 *   - Suspend the root port and expect suspend event
 *   - Close the device
 */
TEST_CASE("Test close opened suspended device", "[uac_host][power_management]")
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
    uac_host_device_handle_t mic_device_handle = NULL;
    test_open_mic_device(mic_iface_num, test_buf_params.size, test_buf_params.threshold, &mic_device_handle);

    // Suspend the root port
    printf("Issue suspend\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());

    // Expect 1 Suspend event -> only one iface is opened
    expect_client_event(&expect_suspend_evt, 100);   // Mic device is opened by client -> expect event

    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_close(mic_device_handle));
}

/**
 * @brief: Test close device which was previously opened and started by the client, while in suspended state
 *
 * Purpose:
 *   - Test client reaction to closing device while in suspended state
 *
 * Procedure:
 *   - Connect, open and start one interface
 *   - Suspend the root port and expect suspend event
 *   - Close the device
 */
TEST_CASE("Test close started suspended device", "[uac_host][power_management]")
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
    uac_host_device_handle_t mic_device_handle = NULL;
    test_open_mic_device(mic_iface_num, test_buf_params.size, test_buf_params.threshold, &mic_device_handle);

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

    // Suspend the root port
    printf("Issue suspend\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());

    // Expect 1 Suspend event -> only one iface is opened
    expect_client_event(&expect_suspend_evt, 100);   // Mic device is opened by client -> expect event

    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_close(mic_device_handle));
}

/**
 * @brief: Test suspending/resuming device after starting the device
 *
 * Purpose:
 *   - Reaction of the client to suspend/resume event while device is started
 *
 * Procedure:
 *   - Connect, open and start interface
 *   - Suspend the root port and expect suspend event
 *   - Resume the root port and expect resume event
 *   - Stop and close the device
 */
TEST_CASE("Test suspend/resume device while device started", "[uac_host][power_management]")
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
    uac_host_device_handle_t mic_device_handle = NULL;
    test_open_mic_device(mic_iface_num, test_buf_params.size, test_buf_params.threshold, &mic_device_handle);

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

    // Keep the device in started state for some time
    vTaskDelay(10);

    // Suspend the root port
    printf("Issue suspend\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());

    // Expect 1 Suspend event -> only one iface is opened
    expect_client_event(&expect_suspend_evt, 100);   // Mic device is opened by client -> expect event
    expect_client_event(NULL, 100);                  // Spk device not opened by client -> expect NO event

    // Stay suspended for a while
    vTaskDelay(100);

    // Resume the root port
    printf("Issue resume\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_resume());

    // Expect 1 Resume event -> only one iface is opened
    expect_client_event(&expect_resume_evt, 100);    // Mic device is opened by client -> expect event
    expect_client_event(NULL, 100);                  // Spk device not opened by client -> expect NO event

    // Expect the device to be started (to be in UAC_INTERFACE_STATE_READY), stop the device
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_stop(mic_device_handle));
    // Close the mic device
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_close(mic_device_handle));
}

/**
 * @brief: Test start device while the root port is suspended
 *
 * Purpose:
 *   - Reaction of the client to starting the device in suspended mode
 *
 * Procedure:
 *   - Connect and open interface
 *   - Suspend the root port and expect suspend event
 *   - Fail to start the device while in suspended state
 *   - Resume the root port and expect resume event
 *   - Start the device normally
 *   - Suspend the root port and expect suspend event
 *   - Start previously suspended device while in suspended state
 *   - Expect resume event
 *   - Stop and close the device
 */
TEST_CASE("Test start (pause after start) suspended device", "[uac_host][power_management]")
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
    uac_host_device_handle_t mic_device_handle = NULL;
    test_open_mic_device(mic_iface_num, test_buf_params.size, test_buf_params.threshold, &mic_device_handle);

    // get mic alt interface 1 params, set same params to spk device
    uac_host_dev_alt_param_t mic_alt_params;
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_get_device_alt_param(mic_device_handle, 1, &mic_alt_params));

    // Suspend the root port
    printf("Issue suspend\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());

    // Expect 1 Suspend event -> only one iface is opened
    expect_client_event(&expect_suspend_evt, 100);    // Mic device is opened by client -> expect event

    // Stay suspended for a while
    vTaskDelay(100);

    uac_host_stream_config_t stream_config = {
        .channels = mic_alt_params.channels,
        .bit_resolution = mic_alt_params.bit_resolution,
        .sample_freq = mic_alt_params.sample_freq[0],
        .flags = FLAG_STREAM_PAUSE_AFTER_START,
    };

    // Try to start device from suspended state, when the previous state was IDLE
    // Expect to fail, since the usb_host_lib must claim device's EPs, for which the device must not be in suspended state
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, uac_host_device_start(mic_device_handle, &stream_config));

    // Resume the root port
    printf("Issue resume\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_resume());

    // Expect 1 Resume event -> only one iface is opened
    expect_client_event(&expect_resume_evt, 100);    // Mic device is opened by client -> expect event

    // Some time for the usb host lib to process the auto resume by transfer
    vTaskDelay(100);

    // Start the device with the device in resumed state
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_start(mic_device_handle, &stream_config));

    // Suspend the root port
    printf("Issue suspend\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());

    // Expect 1 Suspend event -> only one iface is opened
    expect_client_event(&expect_suspend_evt, 100);    // Mic device is opened by client -> expect event

    // Start the device with the device in suspended state (after it has been already successfully started)
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_start(mic_device_handle, &stream_config));

    // Expect 1 Resume event -> only one iface is opened
    expect_client_event(&expect_resume_evt, 100);    // Mic device is opened by client -> expect event

    vTaskDelay(100);
    // Expect the device to be started (to be in UAC_INTERFACE_STATE_READY), stop the device
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_stop(mic_device_handle));
    // Close the mic device
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_close(mic_device_handle));
}

/**
 * @brief: Test start device while the root port is suspended
 *
 * Purpose:
 *   - Reaction of the client to starting the device in suspended mode
 *
 * Procedure:
 *   - Connect and open interface
 *   - Suspend the root port and expect suspend event
 *   - Fail to start the device while in suspended state
 *   - Resume the root port and expect resume event
 *   - Start the device normally
 *   - Suspend the root port and expect suspend event
 *   - Start previously suspended device while in suspended state
 *   - Expect resume event
 *   - Stop and close the device
 */
TEST_CASE("Test start (unpause after start) suspended device", "[uac_host][power_management]")
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
    uac_host_device_handle_t mic_device_handle = NULL;
    test_open_mic_device(mic_iface_num, test_buf_params.size, test_buf_params.threshold, &mic_device_handle);

    // get mic alt interface 1 params, set same params to spk device
    uac_host_dev_alt_param_t mic_alt_params;
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_get_device_alt_param(mic_device_handle, 1, &mic_alt_params));

    // Suspend the root port
    printf("Issue suspend\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());

    // Expect 1 Suspend event -> only one iface is opened
    expect_client_event(&expect_suspend_evt, 100);    // Mic device is opened by client -> expect event

    // Stay suspended for a while
    vTaskDelay(100);

    uac_host_stream_config_t stream_config = {
        .channels = mic_alt_params.channels,
        .bit_resolution = mic_alt_params.bit_resolution,
        .sample_freq = mic_alt_params.sample_freq[0],
    };

    // Try to start device from suspended state, when the previous state was IDLE
    // Expect to fail, since the usb_host_lib must claim device's EPs, for which the device must not be in suspended state
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, uac_host_device_start(mic_device_handle, &stream_config));

    // Resume the root port
    printf("Issue resume\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_resume());

    // Expect 1 Resume event -> only one iface is opened
    expect_client_event(&expect_resume_evt, 100);    // Mic device is opened by client -> expect event

    // Some time for the usb host lib to process the auto resume by transfer
    vTaskDelay(100);

    // Start the device with the device in resumed state
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_start(mic_device_handle, &stream_config));

    // Device has been started without FLAG_STREAM_PAUSE_AFTER_START flag, expect transfer events after starting the device
    TEST_ASSERT(expect_transfer_event(&expect_rx_done_evt, pdMS_TO_TICKS(1000)));

    // Suspend the root port
    printf("Issue suspend\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());

    // Expect 1 Suspend event -> only one iface is opened
    expect_client_event(&expect_suspend_evt, 100);    // Mic device is opened by client -> expect event

    // Expect no transfer events
    TEST_ASSERT_FALSE(expect_transfer_event(NULL, pdMS_TO_TICKS(1000)));

    // Start the device with the device in suspended state (after it has been already successfully started)
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_start(mic_device_handle, &stream_config));

    // Expect 1 Resume event -> only one iface is opened
    expect_client_event(&expect_resume_evt, 100);    // Mic device is opened by client -> expect event
    TEST_ASSERT(expect_transfer_event(&expect_rx_done_evt, pdMS_TO_TICKS(1000)));

    vTaskDelay(100);
    // Expect the device to be started (to be in UAC_INTERFACE_STATE_READY), stop the device
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_stop(mic_device_handle));
    // Close the mic device
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_close(mic_device_handle));
}

/**
 * @brief: Test resume the root port by ctrl transfer
 *
 * Purpose:
 *   - Reaction of the client to auto-resume by transfer
 *
 * Procedure:
 *   - Connect, open and start interface
 *   - Suspend the root port and expect suspend event
 *   - Send a ctrl transfer to a suspended device, expect resume event
 *   - Stop and close the device
 */
TEST_CASE("Test resume by ctrl transfer", "[uac_host][power_management]")
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
    uac_host_device_handle_t mic_device_handle = NULL;
    test_open_mic_device(mic_iface_num, test_buf_params.size, test_buf_params.threshold, &mic_device_handle);

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

    // Keep the device in started state for some time
    vTaskDelay(10);

    // Suspend the root port
    printf("Issue suspend\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());
    expect_client_event(&expect_suspend_evt, 100);    // Mic device is opened by client -> expect event

    // Stay suspended for a while
    vTaskDelay(100);

    // Send ctrl transfer to auto resume the device
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_set_mute(mic_device_handle, 0));

    // Expect 1 Resume event -> only one iface is opened
    expect_client_event(&expect_resume_evt, 100);    // Mic device is opened by client -> expect event

    // Some time for the usb host lib to process the auto resume by transfer
    vTaskDelay(100);

    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_unpause(mic_device_handle));

    // Expect the device to be started (to be in UAC_INTERFACE_STATE_READY), stop the device
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_stop(mic_device_handle));
    // Close the mic device
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_close(mic_device_handle));
}

/**
 * @brief: Test stopping device from suspended state
 *
 * Purpose:
 *   - Reaction of the client to stopping paused device, while the device in suspended state
 *   - Reaction of the client to stopping unpaused device, while the device in suspended state
 *
 * Procedure:
 *   - Connect, open and start (pause after start) interface
 *   - Suspend the root port and expect suspend event
 *   - Stop the device
 *   - Resume the root port and expect resume event
 *   - Start, unpause the device and make sure the data are being transferred
 *   - Suspend the root port and expect suspend event
 *   - Stop and close the device
 */
TEST_CASE("Test device stop from suspended state", "[uac_host][power_management][rx]")
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
    uac_host_device_handle_t mic_device_handle = NULL;
    test_open_mic_device(mic_iface_num, test_buf_params.size, test_buf_params.threshold, &mic_device_handle);

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

    // Suspend the root port
    printf("Issue suspend\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());
    expect_client_event(&expect_suspend_evt, 100);    // Mic device is opened by client -> expect suspend event

    vTaskDelay(100);
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_stop(mic_device_handle));

    // Resume the root port
    printf("Issue resume\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_resume());
    expect_client_event(&expect_resume_evt, 100);    // Mic device is opened by client -> expect resume event

    vTaskDelay(100);
    // Start and unpause the device
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_start(mic_device_handle, &stream_config));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_unpause(mic_device_handle));

    // Check that data events are being sent
    TEST_ASSERT(expect_transfer_event(&expect_rx_done_evt, pdMS_TO_TICKS(1000)));

    // Suspend the root port
    printf("Issue suspend\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());
    expect_client_event(&expect_suspend_evt, 100);    // Mic device is opened by client -> expect suspend event

    vTaskDelay(100);
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_stop(mic_device_handle));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_close(mic_device_handle));
}

/**
 * @brief: Test unpause device in suspended state, which was previously in ready state
 *
 * Purpose:
 *   - Reaction of the client to unpausing ready device, while the device in suspended state
 *
 * Procedure:
 *   - Connect, open and start (pause after start) interface
 *   - Suspend the root port and expect suspend event
 *   - Unpause the device
 *   - Expect resume event, caused by ctrl transfer submit, as the uac_host_device_unpause issues a ctrl transfer
 *   - Expect data transfer events
 *   - Pause, stop and close the device
 */
TEST_CASE("Test unpause ready device from suspended state", "[uac_host][power_management][rx]")
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
    uac_host_device_handle_t mic_device_handle = NULL;
    test_open_mic_device(mic_iface_num, test_buf_params.size, test_buf_params.threshold, &mic_device_handle);

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

    // Suspend the root port
    printf("Issue suspend\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());
    expect_client_event(&expect_suspend_evt, 100);    // Mic device is opened by client -> expect suspend event

    vTaskDelay(100);
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_unpause(mic_device_handle));

    // auto resume by transfer submit
    expect_client_event(&expect_resume_evt, 100);    // Mic device is opened by client -> expect resume event

    // Check that data events are being sent
    TEST_ASSERT(expect_transfer_event(&expect_rx_done_evt, pdMS_TO_TICKS(1000)));

    // Cleanup
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_pause(mic_device_handle));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_stop(mic_device_handle));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_close(mic_device_handle));
}

/**
 * @brief: Test unpause device in suspended state, which was previously in active state
 *
 * Purpose:
 *   - Reaction of the client to unpausing active device, while the device in suspended state
 *
 * Procedure:
 *   - Connect, open and start (pause after start) interface
 *   - Expect data transfer events
 *   - Suspend the root port and expect suspend event
 *   - Expect no data transfer events
 *   - Unpause the device
 *   - Expect resume event, caused by ctrl transfer submit, as the uac_host_device_unpause issues a ctrl transfer
 *   - Expect data transfer events
 *   - Pause, stop and close the device
 */
TEST_CASE("Test unpause active device from suspended state", "[uac_host][power_management][rx]")
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
    uac_host_device_handle_t mic_device_handle = NULL;
    test_open_mic_device(mic_iface_num, test_buf_params.size, test_buf_params.threshold, &mic_device_handle);

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
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_unpause(mic_device_handle));

    // Check that data events are being sent
    TEST_ASSERT(expect_transfer_event(&expect_rx_done_evt, pdMS_TO_TICKS(1000)));

    // Suspend the root port
    printf("Issue suspend\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());
    expect_client_event(&expect_suspend_evt, 100);    // Mic device is opened by client -> expect suspend event

    // Check that data events are NOT being sent anymore
    TEST_ASSERT_FALSE(expect_transfer_event(NULL, pdMS_TO_TICKS(1000)));

    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_unpause(mic_device_handle));
    // auto resume by transfer submit
    expect_client_event(&expect_resume_evt, 100);    // Mic device is opened by client -> expect resume event

    // Check that data events are being sent
    TEST_ASSERT(expect_transfer_event(&expect_rx_done_evt, pdMS_TO_TICKS(1000)));

    // Cleanup
    printf("Cleanup\n");
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_pause(mic_device_handle));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_stop(mic_device_handle));
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_close(mic_device_handle));
}

/**
 * @brief: Test pause device in suspended state, which was previously in active state
 *
 * Purpose:
 *   - Reaction of the client to pausing active device, while the device in suspended state
 *
 * Procedure:
 *   - Connect, open and start (pause after start) interface
 *   - Expect data transfer events
 *   - Suspend the root port and expect suspend event
 *   - Expect no data transfer events
 *   - Pause the device
 *   - Resume the root port and expect resume event
 *   - Unpause the device and expect data transfer events
 *   - Pause, stop and close the device
 */
TEST_CASE("Test pause active device from suspended state", "[uac_host][power_management][rx]")
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
    uac_host_device_handle_t mic_device_handle = NULL;
    test_open_mic_device(mic_iface_num, test_buf_params.size, test_buf_params.threshold, &mic_device_handle);

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
    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_unpause(mic_device_handle));

    // Check that data events are being sent
    TEST_ASSERT(expect_transfer_event(&expect_rx_done_evt, pdMS_TO_TICKS(1000)));

    // Suspend the root port
    printf("Issue suspend\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());
    expect_client_event(&expect_suspend_evt, 100);    // expect suspend event

    // Check that data events are NOT being sent anymore
    TEST_ASSERT_FALSE(expect_transfer_event(NULL, pdMS_TO_TICKS(1000)));

    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_pause(mic_device_handle));

    printf("Issue resume\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_resume());
    expect_client_event(&expect_resume_evt, 100);    // expect resume event

    TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_unpause(mic_device_handle));

    // Check that data events are being sent
    TEST_ASSERT(expect_transfer_event(&expect_rx_done_evt, pdMS_TO_TICKS(1000)));

    // Cleanup
    printf("Cleanup\n");
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

    // Open device
    uac_host_device_handle_t mic_device_handle = NULL;
    test_open_mic_device(mic_iface_num, test_buf_params.size, test_buf_params.threshold, &mic_device_handle);

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
    uint8_t *rx_buffer = (uint8_t *)calloc(1, test_buf_params.threshold);
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

            if (xQueueReceive(s_transfer_event_queue, &transfer_event, pdMS_TO_TICKS(1000)) == pdPASS) {

                // Validate data received from the transfer event queue
                TEST_ASSERT_EQUAL_MESSAGE(transfer_event.event_group, UAC_DEVICE_EVENT, "Incorrect event group");
                TEST_ASSERT_EQUAL_MESSAGE(transfer_event.device_evt.event, UAC_HOST_DEVICE_EVENT_RX_DONE, "Incorrect event type");

                // Read data from the device's ring buffer
                TEST_ASSERT_EQUAL(ESP_OK, uac_host_device_read(mic_device_handle, rx_buffer, test_buf_params.threshold, &rx_size, 0));
                TEST_ASSERT_EQUAL(test_buf_params.threshold, rx_size);
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
        expect_client_event(&expect_suspend_evt, 100);    // expect suspend event

        // Notify the data handling task, that the device is suspended, to stop expecting new data
        xTaskNotifyGiveIndexed(data_handling_task_hdl, TEST_NOTIFY_DEVICE_SUSPENDED);

        // Keep the device suspended for a while
        vTaskDelay(pdMS_TO_TICKS(TEST_SUPEND_RESUME_HOLD_MS));

        // Resume the root port and wait for the resume event
        TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_resume());
        expect_client_event(&expect_resume_evt, 100);    // expect resume event
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

/**
 * @brief: Test suspend/resume signaling with sudden disconnect
 *
 * Purpose:
 *   - Test client suspend/resume event delivery when disconnecting suspended device
 *
 * Procedure:
 *   - Connect and open 2 interfaces
 *   - Suspend the root port and expect 2 suspend events
 *   - Disconnect the root port and expect 2 disconnect events
 *   - Close both devices
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
    test_open_mic_device(mic_iface_num, test_buf_params.size, test_buf_params.threshold, &mic_device_handle);
    test_open_spk_device(spk_iface_num, test_buf_params.size, test_buf_params.threshold, &spk_device_handle);

    // Suspend the root port
    printf("Issue suspend\n");
    TEST_ASSERT_EQUAL(ESP_OK, usb_host_lib_root_port_suspend());

    // Expect 2 Suspend events -> each for each interface
    expect_client_event(&expect_suspend_evt, 100);    // Mic device is opened by client -> expect event
    expect_client_event(&expect_suspend_evt, 100);    // Spk device is opened by client -> expect event

    // Sudden disconnect
    force_conn_state(false, 0);

    // Expect 2 Disconnected events -> each for each interface
    expect_client_event(&expect_disconn_evt, 100);    // Mic device is opened by client -> expect event
    expect_client_event(&expect_disconn_evt, 100);    // Spk device is opened by client -> expect event
}

#endif // UAC_HOST_SUSPEND_RESUME_API_SUPPORTED
