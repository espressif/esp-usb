/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "soc/soc_caps.h"

// #if SOC_USB_OTG_SUPPORTED

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "unity.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "device_common.h"

#define SPIN_TASK_PRIO      2
#define STATS_TASK_PRIO     3
#define STATS_TICKS         pdMS_TO_TICKS(1000)
#define ARRAY_SIZE_OFFSET   5   //Increase this if print_real_time_stats returns ESP_ERR_INVALID_SIZE

static SemaphoreHandle_t sync_stats_task;
static SemaphoreHandle_t _data_cmp_sem;


#define TEST_QUEUE_LEN      5 // Length of the queue for RX events

typedef struct {
    int itf;
} cdc_rx_event_t;

static QueueHandle_t _test_cdc_rx_event_queue = NULL;

void test_storage_event_queue_setup(void)
{
    _test_cdc_rx_event_queue = xQueueCreate(TEST_QUEUE_LEN, sizeof(cdc_rx_event_t));
    TEST_ASSERT_NOT_NULL(_test_cdc_rx_event_queue);
}

void test_storage_event_queue_teardown(void)
{
    if (_test_cdc_rx_event_queue) {
        vQueueDelete(_test_cdc_rx_event_queue);
        _test_cdc_rx_event_queue = NULL;
    }
}

/**
 * @brief   Function to print the CPU usage of tasks over a given duration.
 *
 * This function will measure and print the CPU usage of tasks over a specified
 * number of ticks (i.e. real time stats). This is implemented by simply calling
 * uxTaskGetSystemState() twice separated by a delay, then calculating the
 * differences of task run times before and after the delay.
 *
 * @note    If any tasks are added or removed during the delay, the stats of
 *          those tasks will not be printed.
 * @note    This function should be called from a high priority task to minimize
 *          inaccuracies with delays.
 * @note    When running in dual core mode, each core will correspond to 50% of
 *          the run time.
 *
 * @param   xTicksToWait    Period of stats measurement
 *
 * @return
 *  - ESP_OK                Success
 *  - ESP_ERR_NO_MEM        Insufficient memory to allocated internal arrays
 *  - ESP_ERR_INVALID_SIZE  Insufficient array size for uxTaskGetSystemState. Trying increasing ARRAY_SIZE_OFFSET
 *  - ESP_ERR_INVALID_STATE Delay duration too short
 */
static esp_err_t print_real_time_stats(TickType_t xTicksToWait)
{
    TaskStatus_t *start_array = NULL, *end_array = NULL;
    UBaseType_t start_array_size, end_array_size;
    configRUN_TIME_COUNTER_TYPE start_run_time, end_run_time;
    esp_err_t ret;

    // Allocate array to store current task states
    start_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
    start_array = malloc(sizeof(TaskStatus_t) * start_array_size);
    if (start_array == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto exit;
    }
    // Get current task states
    start_array_size = uxTaskGetSystemState(start_array, start_array_size, &start_run_time);
    if (start_array_size == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        goto exit;
    }

    vTaskDelay(xTicksToWait);

    // Allocate array to store tasks states post delay
    end_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
    end_array = malloc(sizeof(TaskStatus_t) * end_array_size);
    if (end_array == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto exit;
    }
    // Get post delay task states
    end_array_size = uxTaskGetSystemState(end_array, end_array_size, &end_run_time);
    if (end_array_size == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        goto exit;
    }

    // Calculate total_elapsed_time in units of run time stats clock period.
    uint32_t total_elapsed_time = (end_run_time - start_run_time);
    if (total_elapsed_time == 0) {
        ret = ESP_ERR_INVALID_STATE;
        goto exit;
    }

    printf("| Task | Run Time | Percentage\n");
    // Match each task in start_array to those in the end_array
    for (int i = 0; i < start_array_size; i++) {
        int k = -1;
        for (int j = 0; j < end_array_size; j++) {
            if (start_array[i].xHandle == end_array[j].xHandle) {
                k = j;
                // Mark that task have been matched by overwriting their handles
                start_array[i].xHandle = NULL;
                end_array[j].xHandle = NULL;
                break;
            }
        }
        // Check if matching task found
        if (k >= 0) {
            uint32_t task_elapsed_time = end_array[k].ulRunTimeCounter - start_array[i].ulRunTimeCounter;
            uint32_t percentage_time = (task_elapsed_time * 100UL) / (total_elapsed_time * CONFIG_FREERTOS_NUMBER_OF_CORES);
            printf("| %s | %"PRIu32" | %"PRIu32"%%\n", start_array[i].pcTaskName, task_elapsed_time, percentage_time);
        }
    }

    // Print unmatched tasks
    for (int i = 0; i < start_array_size; i++) {
        if (start_array[i].xHandle != NULL) {
            printf("| %s | Deleted\n", start_array[i].pcTaskName);
        }
    }
    for (int i = 0; i < end_array_size; i++) {
        if (end_array[i].xHandle != NULL) {
            printf("| %s | Created\n", end_array[i].pcTaskName);
        }
    }
    ret = ESP_OK;

exit:    // Common return path
    free(start_array);
    free(end_array);
    return ret;
}

static void stats_task(void *arg)
{
    xSemaphoreTake(sync_stats_task, portMAX_DELAY);

    // Print real time stats every 1 second
    while (1) {
        printf("\n\nGetting real time stats over %"PRIu32" ticks\n", STATS_TICKS);
        if (print_real_time_stats(STATS_TICKS) == ESP_OK) {
            printf("Real time stats obtained\n");
        } else {
            printf("Error getting real time stats\n");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// CDC related
static uint8_t cdc_test_app_buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE + 1];

void test_cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    // size_t rx_size = 0;
    /* Read the data */
    // TEST_ASSERT_EQUAL_MESSAGE(ESP_OK,
    //         tinyusb_cdcacm_read(itf, cdc_test_app_buf, CONFIG_TINYUSB_CDC_RX_BUFSIZE, &rx_size),
    //         "CDC read failed");
    // /* Echoed back with data */
    // TEST_ASSERT_EQUAL_MESSAGE(rx_size,
    //         tinyusb_cdcacm_write_queue(itf, cdc_test_app_buf, rx_size),
    //         "CDC write queue failed");

    cdc_rx_event_t msg = {
        .itf = itf,
    };
    xQueueSend(_test_cdc_rx_event_queue, &msg, portMAX_DELAY);
}

static void test_cdc_rx_task(void *arg)
{
    SemaphoreHandle_t sync_task = (SemaphoreHandle_t)arg;
    // xSemaphoreGive(sync_task);

    while (1) {
        cdc_rx_event_t msg;
        BaseType_t ret = xQueueReceive(_test_cdc_rx_event_queue, &msg, pdMS_TO_TICKS(5000));
        if (ret == pdPASS) {
            size_t rx_size = 0;
            /* Read the data */
            TEST_ASSERT_EQUAL_MESSAGE(ESP_OK,
                                      tinyusb_cdcacm_read(msg.itf, cdc_test_app_buf, CONFIG_TINYUSB_CDC_RX_BUFSIZE, &rx_size),
                                      "CDC read failed");
            /* Echoed back with data */
            TEST_ASSERT_EQUAL_MESSAGE(rx_size,
                                      tinyusb_cdcacm_write_queue(msg.itf, cdc_test_app_buf, rx_size),
                                      "CDC write queue failed");
        } else {
            printf("No data received within 5 seconds\n");
        }
    }
}


TEST_CASE("Measure CDC class CPU load", "[ci][load]")
{
    test_storage_event_queue_setup();
#if (!CONFIG_FREERTOS_UNICORE)
    // Allow other core to finish initialization
    vTaskDelay(pdMS_TO_TICKS(100));
#endif // (!CONFIG_FREERTOS_UNICORE)

    sync_stats_task = xSemaphoreCreateBinary();
    _data_cmp_sem = xSemaphoreCreateBinary();

    TEST_ASSERT_NOT_NULL(sync_stats_task);
    TEST_ASSERT_NOT_NULL(_data_cmp_sem);

    // Create and start stats task
    // xTaskCreatePinnedToCore(stats_task, "stats", 4096, NULL, STATS_TASK_PRIO, NULL, tskNO_AFFINITY);
    // Run stats task
    // xSemaphoreGive(sync_stats_task);

    // Create task for RX events
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(test_cdc_rx_task,
                                          "cdc_routine",
                                          4096,
                                          (void *) xTaskGetCurrentTaskHandle(),
                                          4,
                                          NULL));

    // Wait the task to start
    // xSemaphoreTake(sync_task, portMAX_DELAY);

    // Install TinyUSB driver
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(test_device_event_handler);
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));

    // Install cdc device
    tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = &test_cdc_rx_callback,
    };
    // Init CDC 0
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_cdcacm_init(&acm_cfg));

    // Wait for the device to be mounted and enumerated by the Host
    test_device_wait();

    // TODO: Wait for all data semaphore for 2 minutes
    if (xSemaphoreTake(_data_cmp_sem, pdMS_TO_TICKS(120000)) != pdTRUE) {
        printf("No data received within 5 seconds\n");
    }

    // Cleanup
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_cdcacm_deinit(TINYUSB_CDC_ACM_0));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());

    vSemaphoreDelete(sync_stats_task);
    vSemaphoreDelete(_data_cmp_sem);

    test_storage_event_queue_teardown();
}
// #endif // SOC_USB_OTG_SUPPORTED
