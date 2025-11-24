
/*
 * SPDX-FileCopyrightText: 2015-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "soc/soc_caps.h"
#if SOC_USB_OTG_SUPPORTED

#include "unity.h"
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include "esp_idf_version.h"
#include "esp_private/msc_scsi_bot.h"
#include "esp_private/usb_phy.h"
#include "usb/usb_host.h"
#include "usb/msc_host_vfs.h"
#include "test_common.h"
#include "../private_include/msc_common.h"

static const char *TAG = "APP";

#define ESP_OK_ASSERT(exp) TEST_ASSERT_EQUAL(ESP_OK, exp)

static esp_vfs_fat_mount_config_t mount_config = {
    .format_if_mount_failed = true,
    .max_files = 3,
    .allocation_unit_size = 1024,
};

static QueueHandle_t app_queue;
static SemaphoreHandle_t ready_to_deinit_usb;
static msc_host_device_handle_t device;
static msc_host_vfs_handle_t vfs_handle;

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

static void msc_event_cb(const msc_host_event_t *event, void *arg)
{
    switch (event->event) {
    case MSC_DEVICE_CONNECTED:
        printf("MSC_DEVICE_CONNECTED\n");
        break;
    case MSC_DEVICE_DISCONNECTED:
        printf("MSC_DEVICE_DISCONNECTED\n");
        break;
#ifdef MSC_HOST_SUSPEND_RESUME_API_SUPPORTED
    case MSC_DEVICE_SUSPENDED:
        printf("MSC_DEVICE_SUSPENDED\n");
        TEST_ASSERT_EQUAL_PTR_MESSAGE(device, event->device.handle, "Device handles not matching");
        break;
    case MSC_DEVICE_RESUMED:
        printf("MSC_DEVICE_RESUMED\n");
        TEST_ASSERT_EQUAL_PTR_MESSAGE(device, event->device.handle, "Device handles not matching");
        break;
#endif // MSC_HOST_SUSPEND_RESUME_API_SUPPORTED
    default:
        printf("Unrecognized MSC Client event\n");
        return;
    }

    xQueueSend(app_queue, event, 10);
}

/**
 * @brief Wait for app event
 *
 * @param expected_app_event expected event (NULL for no event expected)
 * @param ticks ticks to wait for an event
 */
static void wait_for_app_event(msc_host_event_t *expected_app_event, TickType_t ticks)
{
    msc_host_event_t app_event;

    // Check, that no event is delivered
    if (expected_app_event == NULL) {
        if (pdFALSE == xQueueReceive(app_queue, &app_event, ticks)) {
            return;
        } else {
            TEST_FAIL_MESSAGE("Expecting NO event, but an event delivered");
        }
    }

    // Check that an event is delivered
    if (pdTRUE == xQueueReceive(app_queue, &app_event, ticks)) {
        TEST_ASSERT_EQUAL_MESSAGE(expected_app_event->event, app_event.event, "Unexpected app event");
    } else {
        TEST_ASSERT_MESSAGE(false, "App event not generated on time");
    }
}

static const char *TEST_STRING = "Hello World!";
static const char *FILE_NAME = "/usb/ESP32.txt";
static const char *BASE_PATH = "/usb";

static void write_read_file(const char *file_path)
{
    char line[64];

    ESP_LOGI(TAG, "Writing file");
    FILE *f = fopen(file_path, "w");
    TEST_ASSERT(f);
    fprintf(f, TEST_STRING);
    fclose(f);

    ESP_LOGI(TAG, "Reading file");
    TEST_ASSERT(fopen(file_path, "r"));
    fgets(line, sizeof(line), f);
    fclose(f);
    // strip newline
    char *pos = strchr(line, '\n');
    if (pos) {
        *pos = '\0';
    }
    TEST_ASSERT_EQUAL_STRING(line, TEST_STRING);
    ESP_LOGI(TAG, "Done");
}

static bool file_exists(const char *file_path)
{
    return ( access(file_path, F_OK) == 0 );
}

// Handles common USB host library events
static void handle_usb_events(void *args)
{
    uint32_t end_flags = 0;

    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        // Release devices once all clients has deregistered
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            printf("USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS\n");
            usb_host_device_free_all();
            end_flags |= 1;
        }
        // Give ready_to_deinit_usb semaphore to indicate that USB Host library
        // can be deinitialized, and terminate this task.
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            printf("USB_HOST_LIB_EVENT_FLAGS_ALL_FREE\n");
            end_flags |= 2;
        }

        if (end_flags == 3) {
            xSemaphoreGive(ready_to_deinit_usb);
            break;
        }

#ifdef MSC_HOST_SUSPEND_RESUME_API_SUPPORTED
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_AUTO_SUSPEND) {
            printf("USB_HOST_LIB_EVENT_FLAGS_AUTO_SUSPEND\n");
            ESP_OK_ASSERT(usb_host_lib_root_port_suspend());
        }
#endif // MSC_HOST_SUSPEND_RESUME_API_SUPPORTED

    }
    vTaskDelete(NULL);
}

/**
 * @brief MSC driver handling task
 *
 * This task is only used if the MSC driver was installed with no background task
 *
 * @param[in] args Not used
 */
static void msc_task(void *args)
{
    ESP_LOGI(TAG, "USB MSC handling start");
    while (msc_host_handle_events(portMAX_DELAY) == ESP_OK) {
    }
    ESP_LOGI(TAG, "USB MSC handling stop");
    vTaskDelete(NULL);
}

static void check_sudden_disconnect(void)
{
    uint8_t data[512];
    const size_t DATA_SIZE = sizeof(data);

    ESP_LOGI(TAG, "Creating test.txt");
    FILE *file = fopen("/usb/test.txt", "w");
    TEST_ASSERT(file);

    ESP_LOGI(TAG, "Write data");
    TEST_ASSERT_EQUAL(DATA_SIZE, fwrite(data, 1, DATA_SIZE, file));
    TEST_ASSERT_EQUAL(DATA_SIZE, fwrite(data, 1, DATA_SIZE, file));
    TEST_ASSERT_EQUAL(0, fflush(file));

    ESP_LOGI(TAG, "Trigger a disconnect");
    force_conn_state(false, 0);

    msc_host_event_t expected_event = {.event = MSC_DEVICE_DISCONNECTED};
    wait_for_app_event(&expected_event, 100);

    ESP_LOGI(TAG, "Write data after disconnect");
    TEST_ASSERT_NOT_EQUAL( DATA_SIZE, fwrite(data, 1, DATA_SIZE, file));

    fclose(file);
}

static void msc_test_init(void)
{
    BaseType_t task_created;

    ready_to_deinit_usb = xSemaphoreCreateBinary();

    TEST_ASSERT( app_queue = xQueueCreate(5, sizeof(msc_host_event_t)) );
    const bool skip_phy_setup = install_phy();
    const usb_host_config_t host_config = {
        .skip_phy_setup = skip_phy_setup,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    ESP_OK_ASSERT( usb_host_install(&host_config) );
    task_created = xTaskCreatePinnedToCore(handle_usb_events, "usb_events", 2 * 2048, NULL, 2, NULL, 0);
    TEST_ASSERT(task_created);
}

static void msc_test_wait_and_install_device(void)
{
    ESP_LOGI(TAG, "Waiting for USB stick to be connected");
    msc_host_event_t app_event;
    xQueueReceive(app_queue, &app_event, portMAX_DELAY);
    TEST_ASSERT_EQUAL(MSC_DEVICE_CONNECTED, app_event.event);
    uint8_t device_addr = app_event.device.address;

    ESP_OK_ASSERT( msc_host_install_device(device_addr, &device) );
    ESP_OK_ASSERT( msc_host_vfs_register(device, BASE_PATH, &mount_config, &vfs_handle) );
}

static void msc_setup(void)
{
    msc_test_init();
    const msc_host_driver_config_t msc_config = {
        .create_backround_task = true,
        .callback = msc_event_cb,
        .stack_size = 4096,
        .task_priority = 5,
    };
    ESP_OK_ASSERT( msc_host_install(&msc_config) );
    msc_test_wait_and_install_device();
}

static void msc_test_uninstall_device(void)
{
    ESP_OK_ASSERT( msc_host_vfs_unregister(vfs_handle) );
    ESP_OK_ASSERT( msc_host_uninstall_device(device) );
}

static void msc_test_deinit(void)
{
    ESP_OK_ASSERT( msc_host_uninstall() );

    xSemaphoreTake(ready_to_deinit_usb, portMAX_DELAY);
    vSemaphoreDelete(ready_to_deinit_usb);
    vTaskDelay(10); // Wait to finish any ongoing USB operations
    ESP_OK_ASSERT( usb_host_uninstall() );
    delete_phy();

    vQueueDelete(app_queue);
    vTaskDelay(10); // Wait for FreeRTOS to clean up deleted tasks
}

static void msc_teardown(void)
{
    msc_test_uninstall_device();
    msc_test_deinit();
}

static void write_read_sectors(void)
{
    uint8_t write_data[DISK_BLOCK_SIZE];
    uint8_t read_data[DISK_BLOCK_SIZE];

    memset(write_data, 0x55, DISK_BLOCK_SIZE);
    memset(read_data, 0, DISK_BLOCK_SIZE);

    ESP_OK_ASSERT( scsi_cmd_write10(device, write_data, 10, 1, DISK_BLOCK_SIZE));
    ESP_OK_ASSERT( scsi_cmd_read10(device, read_data, 10, 1, DISK_BLOCK_SIZE));

    TEST_ASSERT_EQUAL_MEMORY(write_data, read_data, DISK_BLOCK_SIZE);
}

static void erase_storage(void)
{
    uint8_t data[DISK_BLOCK_SIZE];
    memset(data, 0xFF, DISK_BLOCK_SIZE);

    for (int block = 0; block < DISK_BLOCK_NUM; block++) {
        scsi_cmd_write10(device, data, block, 1, DISK_BLOCK_SIZE);
    }
}

TEST_CASE("write_and_read_file", "[usb_msc]")
{
    msc_setup();
    write_read_file(FILE_NAME);
    msc_teardown();
}

TEST_CASE("sudden_disconnect", "[usb_msc]")
{
    msc_setup();
    check_sudden_disconnect();
    msc_teardown();
}

TEST_CASE("sectors_can_be_written_and_read", "[usb_msc]")
{
    msc_setup();
    write_read_sectors();
    msc_teardown();
}

esp_err_t bot_execute_command(msc_device_t *device, uint8_t *cbw, void *data, size_t size);
/**
 * @brief Error recovery testcase
 *
 * Various error cases:
 *  - Accessing non-existent memory
 *  - Invalid SCSI command
 *  - USB transfer STALL
 */
TEST_CASE("error_recovery_1", "[usb_msc][ignore]")
{
    msc_setup();
    uint8_t data[DISK_BLOCK_SIZE];
    esp_err_t err;

    // Some flash disks will respond with stall, some with error in CSW, some with timeout
    printf("invalid bot command\n");
    uint32_t dummy_cbw[8] = {0x5342555, 2, 0, 0x55555555};
    err = bot_execute_command(device, (uint8_t *)dummy_cbw, data, 31);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, err);

    err = msc_host_reset_recovery(device);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    // Make sure we can read/write after the error was cleared
    printf("read write after reset\n");
    write_read_file(FILE_NAME);

    msc_teardown();
}

TEST_CASE("error_recovery_2", "[usb_msc][ignore]")
{
    msc_setup();
    uint8_t data[DISK_BLOCK_SIZE];
    esp_err_t err;

    // Write to and read from invalid sector
    // Some flash disks will respond with stall, some with error in CSW, some with timeout
    printf("read 10\n");
    err = scsi_cmd_read10(device, data, UINT32_MAX, 1, DISK_BLOCK_SIZE);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, err);
    err = msc_host_reset_recovery(device);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    printf("read write after reset\n");
    write_read_file(FILE_NAME);

    msc_teardown();
}

#define MAX_BUFFER_SIZE (1024 + 1) // Maximum buffer size for this test
#define SETVBUF_TEST(_size) do { \
    printf("setvbuf %d\n", _size); \
    int err = setvbuf(file, NULL, _IOFBF, _size); \
    TEST_ASSERT_EQUAL_MESSAGE(0, err, "setvbuf failed"); \
    err = fseek(file, SEEK_SET, 0); \
    TEST_ASSERT_EQUAL_MESSAGE(0, err, "fseek failed"); \
    size_t write_cnt = fwrite(write_buf, 1, _size, file); \
    TEST_ASSERT_EQUAL(_size, write_cnt); \
    err = fseek(file, SEEK_SET, 0); \
    TEST_ASSERT_EQUAL_MESSAGE(0, err, "fseek failed"); \
    memset(read_buf, 0, MAX_BUFFER_SIZE + 1); \
    size_t read_cnt = fread(read_buf, 1, _size, file); \
    TEST_ASSERT_EQUAL_MESSAGE(_size, read_cnt, "Error in reading file"); \
    TEST_ASSERT_EQUAL_HEX8_ARRAY(write_buf, read_buf, _size); \
    TEST_ASSERT_EQUAL_MESSAGE(0, read_buf[_size], "Read buffer accessed outside of its boundaries"); \
} while(0); \

/**
 * @brief setvbuf testcase
 *
 * From v1.1.0 the MSC driver reuses buffer from VFS for USB transfers.
 * Check that this feature works correctly with various buffer lengths.
 */
TEST_CASE("setvbuf", "[usb_msc]")
{
    msc_setup();

    FILE *file = fopen("/usb/test", "w+");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, "Could not open file for writing");

    char *write_buf = malloc(MAX_BUFFER_SIZE);
    char *read_buf = malloc(MAX_BUFFER_SIZE + 1); // 1 canary byte
    TEST_ASSERT_NOT_NULL(write_buf);
    TEST_ASSERT_NOT_NULL(read_buf);

    // Initialize write buffer with some data
    for (int i = 0; i < MAX_BUFFER_SIZE; i++) {
        write_buf[i] = i & 0xFF;
    }

    SETVBUF_TEST(64);
    SETVBUF_TEST(128);
    SETVBUF_TEST(500);
    SETVBUF_TEST(1000);
    SETVBUF_TEST(1023);
    SETVBUF_TEST(1024);
    SETVBUF_TEST(MAX_BUFFER_SIZE);

    fclose(file);
    free(write_buf);
    free(read_buf);

    msc_teardown();
}

/**
 * @brief USB MSC format testcase
 * @attention This testcase deletes all content on the USB MSC device.
 *            The device must be reset in order to contain the FILE_NAME again.
 */
TEST_CASE("can_be_formated", "[usb_msc]")
{
    printf("Create file\n");
    msc_setup();
    write_read_file(FILE_NAME);
    msc_teardown();

    printf("File exists after mounting again\n");
    msc_setup();
    TEST_ASSERT(file_exists(FILE_NAME));
    printf("Erase storage device\n");
    erase_storage();
    msc_teardown();

    printf("Check file does not exist after formatting\n");
    mount_config.format_if_mount_failed = true;
    msc_setup();
    TEST_ASSERT_FALSE(file_exists(FILE_NAME));
    msc_teardown();
    mount_config.format_if_mount_failed = false;
}

/**
 * @brief USB MSC API format testcase
 * @attention This testcase deletes all content on the USB MSC device.
 *            The device must be reset in order to contain the FILE_NAME again.
 */
TEST_CASE("can_be_formated_by_api", "[usb_msc]")
{
    printf("Create file on MSC device\n");
    msc_setup();
    write_read_file(FILE_NAME);

    printf("Format storage device using msc_host_vfs_format\n");
    esp_err_t ret = msc_host_vfs_format(device, &mount_config, vfs_handle);
    TEST_ASSERT_EQUAL(ESP_OK, ret);

    printf("Verify file does not exist after formatting\n");
    TEST_ASSERT_FALSE(file_exists(FILE_NAME));
    msc_teardown();
}

static void print_device_info(msc_host_device_info_t *info)
{
    const size_t megabyte = 1024 * 1024;
    uint64_t capacity = ((uint64_t)info->sector_size * info->sector_count) / megabyte;

    printf("Device info:\n");
    printf("\t Capacity: %llu MB\n", capacity);
    printf("\t Sector size: %"PRIu32"\n", info->sector_size);
    printf("\t Sector count: %"PRIu32"\n", info->sector_count);
    printf("\t PID: 0x%4X \n", info->idProduct);
    printf("\t VID: 0x%4X \n", info->idVendor);
    wprintf(L"\t iProduct: %S \n", info->iProduct);
    wprintf(L"\t iManufacturer: %S \n", info->iManufacturer);
    wprintf(L"\t iSerialNumber: %S \n", info->iSerialNumber);
}

/**
 * @brief USB MSC device_info testcase
 *
 * Simple testcase that only runs msc_host_get_device_info()
 * To make sure that we correctly handle missing string descriptors
 */
TEST_CASE("device_info", "[usb_msc]")
{
    msc_setup();
    msc_host_device_info_t info;
    esp_err_t err = msc_host_get_device_info(device, &info);
    msc_teardown();
    TEST_ASSERT_EQUAL(ESP_OK, err);
    print_device_info(&info);
}

/**
 * @brief USB MSC driver with no background task
 *
 * Install the driver without background task
 * and make sure that everything works
 */
TEST_CASE("no_background_task", "[usb_msc]")
{
    msc_test_init();
    const msc_host_driver_config_t msc_config = {
        .create_backround_task = false,
        .callback = msc_event_cb,
        .stack_size = 4096,
        .task_priority = 5,
    };
    ESP_OK_ASSERT( msc_host_install(&msc_config) );
    BaseType_t task_created = xTaskCreatePinnedToCore(msc_task, "msc_events", 2 * 2048, NULL, 2, NULL, 0);
    TEST_ASSERT(task_created);
    msc_test_wait_and_install_device();
    write_read_sectors(); // Do some dummy operations
    msc_teardown();
}

/**
 * @brief USB MSC driver with no background task
 *
 * Install and uninstall the driver without background task
 * without ever calling usb_host_client_handle_events()
 */
TEST_CASE("no_background_task_2", "[usb_msc]")
{
    msc_test_init();
    const msc_host_driver_config_t msc_config = {
        .create_backround_task = false,
        .callback = msc_event_cb,
    };
    ESP_OK_ASSERT( msc_host_install(&msc_config) );

    vTaskDelay(100); // Give USB Host Library some time for device enumeration

    msc_test_deinit();
}

#ifdef MSC_HOST_SUSPEND_RESUME_API_SUPPORTED
/**
 * @brief USB MSC driver Suspend/Resume
 *
 * Purpose:
 *     - Test global Suspend/Resume of the root port with MSC Driver
 *
 * Procedure:
 *     - Install USB Host lib, Install MSC driver, open a device, register VFS
 *     - Issue global suspend and verify, the MSC_DEVICE_SUSPENDED event is delivered
 *     - Unregister VFS
 *     - Issue global resume and verify, the MSC_DEVICE_RESUMED event is delivered
 *     - Register VFS back, check that a file can be read/written after Suspend/Resume cycle
 *     - Teardown
 */
TEST_CASE("suspend_resume", "[usb_msc]")
{
    msc_setup();
    write_read_file(FILE_NAME);                         // Initial read write check
    vTaskDelay(10);                                     // Wait some time to finish read_write

    msc_host_event_t expected_event = {.event = MSC_DEVICE_SUSPENDED};
    ESP_OK_ASSERT(usb_host_lib_root_port_suspend());    // Suspend the root port
    wait_for_app_event(&expected_event, 20);            // Expect MSC device to be suspended

    // Check root por state using usb_host_lib_info
    usb_host_lib_info_t lib_info;
    ESP_OK_ASSERT(usb_host_lib_info(&lib_info));
    TEST_ASSERT_MESSAGE(lib_info.root_port_suspended, "Root port is not suspended, but it's expected to be");

    // MSC device suspended, unmount storage, verify that the file does not exist
    TEST_ASSERT_EQUAL(ESP_OK, msc_host_vfs_unregister(vfs_handle));
    TEST_ASSERT_FALSE(file_exists(FILE_NAME));

    // Resume the device
    ESP_OK_ASSERT(usb_host_lib_root_port_resume());     // Resume the root port
    expected_event.event = MSC_DEVICE_RESUMED;          // Expect MSC device to be resumed
    wait_for_app_event(&expected_event, 20);            // Expect app event

    ESP_OK_ASSERT(usb_host_lib_info(&lib_info));
    TEST_ASSERT_FALSE_MESSAGE(lib_info.root_port_suspended, "Root port is suspended, but it's expected not to be");

    // MSC Device is resumed, mount storage, verify that the file exist and can be read/written
    TEST_ASSERT_EQUAL(ESP_OK, msc_host_vfs_register(device, BASE_PATH, &mount_config, &vfs_handle));
    TEST_ASSERT(file_exists(FILE_NAME));
    write_read_file(FILE_NAME);                         // Verify read write after suspend/resume cycle
    msc_teardown();
}

#define MSC_TEST_SUSPEND_TIMER_INTERVAL_MS    1000
#define MSC_TEST_SUSPEND_TIMER_MARGIN_MS      50
/**
 * @brief USB MSC driver with the automatic suspend timer timer
 *
 * Purpose:
 *     - Test One-Shot and Periodic auto suspend timer for global Suspend of the root port with MSC Driver
 *
 * Procedure:
 *     - Install USB Host lib, Install MSC driver, open a device, register VFS
 *     - Set One-Shot auto suspend timer to 1000ms
 *     - Simulate some USB traffic by reading/writing a file multiple times and regularly check that no USB_HOST_LIB event is delivered
 *     - Once all the traffic is idle, verify the MSC_DEVICE_SUSPENDED event is delivered
 *     - Measure time from issuing automatic suspend until the suspend event delivery
       - Set Periodic auto suspend timer and periodically check for MSC_DEVICE_SUSPENDED event, manually resume the device and verify device functionality
 *     - Disable the Periodic auto suspend timer and make sure no other event is delivered
 *     - Teardown
 */
TEST_CASE("auto_suspend_timer", "[usb_msc]")
{
    msc_setup();

    // Set One-Shot auto suspend timer and start measuring ticks
    ESP_OK_ASSERT(usb_host_lib_set_auto_suspend(USB_HOST_LIB_AUTO_SUSPEND_ONE_SHOT, MSC_TEST_SUSPEND_TIMER_INTERVAL_MS));
    TickType_t auto_suspend_tick_start = xTaskGetTickCount();
    msc_host_event_t peek_event, expected_event;

    // Simulate some traffic, so the root port would not suspend after MSC_TEST_SUSPEND_TIMER_INTERVAL_MS from now,
    // but after MSC_TEST_SUSPEND_TIMER_INTERVAL_MS, once there is no traffic
    // Regularly check, that there is no item in queue (check that the timer did not expire)
    for (int i = 0; i < 5; i++) {
        write_read_file(FILE_NAME);
        TEST_ASSERT_EQUAL(pdFALSE, xQueuePeek(app_queue, &peek_event, 0));
    }

    // Expect MSC device to be suspended, expect app queue and block for MSC_TEST_SUSPEND_TIMER_INTERVAL_MS + some margin
    expected_event.event = MSC_DEVICE_SUSPENDED;
    wait_for_app_event(&expected_event, pdMS_TO_TICKS(MSC_TEST_SUSPEND_TIMER_INTERVAL_MS + MSC_TEST_SUSPEND_TIMER_MARGIN_MS));

    TickType_t auto_suspend_tick_end = xTaskGetTickCount();      // App event received, stop measuring ticks
    const uint32_t elapsed_ms = (auto_suspend_tick_end - auto_suspend_tick_start) * portTICK_PERIOD_MS;

    // Check that, the time elapsed between setting the auto suspend timer and MSC_DEVICE_SUSPENDED event
    // is more than MSC_TEST_SUSPEND_TIMER_INTERVAL_MS + some margin
    TEST_ASSERT_GREATER_THAN_UINT32_MESSAGE(
        MSC_TEST_SUSPEND_TIMER_INTERVAL_MS + MSC_TEST_SUSPEND_TIMER_MARGIN_MS, elapsed_ms,
        "Auto suspend timer did not expire on time"
    );

    // Make sure that no event is delivered, since we set the timer to be One-Shot
    wait_for_app_event(NULL, pdMS_TO_TICKS(MSC_TEST_SUSPEND_TIMER_INTERVAL_MS * 2));

    // Resume the root port manually and expect the resume event
    ESP_OK_ASSERT(usb_host_lib_root_port_resume());
    expected_event.event = MSC_DEVICE_RESUMED;
    wait_for_app_event(&expected_event, 20);

    // Set Periodic auto suspend timer
    ESP_OK_ASSERT(usb_host_lib_set_auto_suspend(USB_HOST_LIB_AUTO_SUSPEND_PERIODIC, MSC_TEST_SUSPEND_TIMER_INTERVAL_MS));

    for (int i = 0; i < 3; i++) {

        // Expect suspended event from auto suspend timer
        expected_event.event = MSC_DEVICE_SUSPENDED;
        wait_for_app_event(&expected_event, pdMS_TO_TICKS(MSC_TEST_SUSPEND_TIMER_INTERVAL_MS + MSC_TEST_SUSPEND_TIMER_MARGIN_MS));

        // Resume the root port manually and expect the resume event
        ESP_OK_ASSERT(usb_host_lib_root_port_resume());
        expected_event.event = MSC_DEVICE_RESUMED;
        wait_for_app_event(&expected_event, 20);

        // MSC device resumed, verify read write after suspend/resume cycle
        write_read_file(FILE_NAME);
    }

    // Disable Periodic auto suspend timer
    ESP_OK_ASSERT(usb_host_lib_set_auto_suspend(USB_HOST_LIB_AUTO_SUSPEND_PERIODIC, 0));
    // Test that the Periodic auto suspend timer can be disabled and no other event is delivered
    TEST_ASSERT_EQUAL(pdFALSE, xQueuePeek(app_queue, &peek_event, pdMS_TO_TICKS(MSC_TEST_SUSPEND_TIMER_INTERVAL_MS * 2)));
    msc_teardown();
}

/**
 * @brief USB MSC automatic resume by transfer submit
 *
 * Purpose:
 *     - Test automatic root port resume by submitting a transfer
 *
 * Procedure:
 *     - Install USB Host lib, Install MSC driver, open a device, register VFS
 *     - Open a file and write some data
 *     - Issue global suspend and verify, the MSC_DEVICE_SUSPENDED event is delivered
 *     - Write some data and expect the root port to be automatically resumed, the MSC_DEVICE_RESUMED event is delivered
 *     - Teardown
 */
TEST_CASE("resume_by_transfer_submit", "[usb_msc]")
{
    msc_setup();

    write_read_file(FILE_NAME);                         // Initial read write check
    vTaskDelay(10);                                     // Wait some time to finish read_write

    msc_host_event_t expected_event = {.event = MSC_DEVICE_SUSPENDED,};
    ESP_OK_ASSERT(usb_host_lib_root_port_suspend());    // Suspend the root port
    wait_for_app_event(&expected_event, 10);            // Expect MSC device to be suspended

    write_read_file(FILE_NAME);                         // Automatically resume the root port by submitting a transfer

    expected_event.event = MSC_DEVICE_RESUMED;          // Expect MSC device to be automatically resumed after transfer submit
    wait_for_app_event(&expected_event, 10);            // Expect app event
    msc_teardown();
}

/**
 * @brief Open and close suspended device
 *
 * Purpose:
 *     - Test opening and closing of a device in suspended state by the MSC client
 *
 * Procedure:
 *     - Install USB Host lib, Install MSC driver
 *     - Suspend the root prot -> make sure no event is delivered
 *     - Fail to open a suspended device by the MSC driver
 *     - Resume the root port -> make sure no event is delivered
 *     - Open the device, register VFS
 *     - Suspend the root port -> make sure suspend event is delivered
 *     - Teardown
 */
TEST_CASE("suspended_device_open_close", "[usb_msc]")
{
    msc_test_init();
    const msc_host_driver_config_t msc_config = {
        .create_backround_task = true,
        .callback = msc_event_cb,
        .stack_size = 4096,
        .task_priority = 5,
    };
    TEST_ASSERT_EQUAL(ESP_OK, msc_host_install(&msc_config) );

    msc_host_event_t app_event;
    xQueueReceive(app_queue, &app_event, portMAX_DELAY);
    TEST_ASSERT_EQUAL(MSC_DEVICE_CONNECTED, app_event.event);
    uint8_t device_addr = app_event.device.address;

    // Suspend the root port and don't expect any event (device not opened by MSC client)
    ESP_OK_ASSERT(usb_host_lib_root_port_suspend());
    wait_for_app_event(NULL, 10);

    // Try to open suspended device
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, msc_host_install_device(device_addr, &device));

    // Resume the root port and don't expect any event (device not opened by MSC client)
    ESP_OK_ASSERT(usb_host_lib_root_port_resume());
    wait_for_app_event(NULL, 10);

    // Open the resumed device
    TEST_ASSERT_EQUAL(ESP_OK, msc_host_install_device(device_addr, &device));
    TEST_ASSERT_EQUAL(ESP_OK, msc_host_vfs_register(device, BASE_PATH, &mount_config, &vfs_handle));

    // Suspend the device and expect supend event
    ESP_OK_ASSERT(usb_host_lib_root_port_suspend());
    msc_host_event_t expected_event = {.event = MSC_DEVICE_SUSPENDED};
    wait_for_app_event(&expected_event, 10);

    msc_teardown();
}

/**
 * @brief Suspend device sudden disconnect
 *
 * Purpose:
 *     - Test disconnection upon a suspended device
 *
 * Procedure:
 *     - Install USB Host lib, Install MSC driver, open device, register VFS
 *     - Suspend the root prot and disconnect the device -> make sure app events are delivered
 *     - Teardown
 */
TEST_CASE("suspended_device_sudden_disconnect", "[usb_msc]")
{
    msc_setup();

    msc_host_event_t expected_event = {.event = MSC_DEVICE_SUSPENDED,};
    ESP_OK_ASSERT(usb_host_lib_root_port_suspend());
    wait_for_app_event(&expected_event, 10);

    force_conn_state(false, 0);

    expected_event.event = MSC_DEVICE_DISCONNECTED;
    wait_for_app_event(&expected_event, 10);
    msc_teardown();
}

/**
 * @brief USB MSC sudden Suspend
 *
 * Purpose:
 *     - Test suspending the root port during file write operation
 *
 * Procedure:
 *     - Install USB Host lib, Install MSC driver, open a device, register VFS
 *     - Open a file and write some data
 *     - Issue global suspend and verify, the MSC_DEVICE_SUSPENDED event is delivered
 *     - Write a data again after the port was suspended, expect data to be written and resume event delivered
 *     - Teardown
 */
static void check_sudden_suspend(void)
{
    uint8_t data[512];
    const size_t DATA_SIZE = sizeof(data);

    ESP_LOGI(TAG, "Creating test.txt");
    FILE *file = fopen("/usb/test.txt", "w");
    TEST_ASSERT(file);

    ESP_LOGI(TAG, "Write data");
    TEST_ASSERT_EQUAL(DATA_SIZE, fwrite(data, 1, DATA_SIZE, file));
    TEST_ASSERT_EQUAL(DATA_SIZE, fwrite(data, 1, DATA_SIZE, file));
    TEST_ASSERT_EQUAL(0, fflush(file));

    ESP_LOGI(TAG, "Trigger a suspend");
    ESP_OK_ASSERT(usb_host_lib_root_port_suspend());

    msc_host_event_t expected_event = {.event = MSC_DEVICE_SUSPENDED,};
    wait_for_app_event(&expected_event, 10);

    TEST_ASSERT_EQUAL(DATA_SIZE, fwrite(data, 1, DATA_SIZE, file));

    expected_event.event = MSC_DEVICE_RESUMED;
    wait_for_app_event(&expected_event, 10);

    fclose(file);
}

TEST_CASE("sudden_suspend", "[usb_msc]")
{
    msc_setup();
    check_sudden_suspend();
    msc_teardown();
}

/**
 * @brief USB MSC sudden Suspend from another task
 *
 * Purpose:
 *     - Test suspending the root port from another task during file write operation
 *
 * Procedure:
 *     - Install USB Host lib, Install MSC driver, open a device, register VFS
 *     - In another task, Open a file and write some data
 *     - Issue global suspend and verify, the MSC_DEVICE_SUSPENDED event is delivered
 *     - Write a data again after the port was suspended, expect not all data to be written
 *     - Teardown
 */
static void write_read_task(void *args)
{
    uint8_t data[512];
    const size_t DATA_SIZE = sizeof(data);

    ESP_LOGI(TAG, "Creating test.txt");
    FILE *file = fopen("/usb/test.txt", "w");
    TEST_ASSERT(file);

    ESP_LOGI(TAG, "Write data");
    TEST_ASSERT_EQUAL(DATA_SIZE, fwrite(data, 1, DATA_SIZE, file));
    TEST_ASSERT_EQUAL(DATA_SIZE, fwrite(data, 1, DATA_SIZE, file));
    TEST_ASSERT_EQUAL(0, fflush(file));

    xTaskNotifyGive((TaskHandle_t)args);                // Suspend the root port from the main task

    ESP_LOGI(TAG, "Write data after suspend");
    TEST_ASSERT_NOT_EQUAL( DATA_SIZE, fwrite(data, 1, DATA_SIZE, file));

    fclose(file);
    vTaskDelete(NULL);
}

TEST_CASE("sudden_suspend_from_another_task", "[usb_msc]")
{
    msc_setup();

    TEST_ASSERT_EQUAL(pdTRUE, xTaskCreate(write_read_task, "MSC write_read", 4096, xTaskGetCurrentTaskHandle(), 3, NULL));
    ulTaskNotifyTake(false, portMAX_DELAY);             // Wait for the task writes first data

    msc_host_event_t expected_event = {.event = MSC_DEVICE_SUSPENDED};
    ESP_OK_ASSERT(usb_host_lib_root_port_suspend());    // Suspend the root port
    wait_for_app_event(&expected_event, 10);            // Expect MSC device to be suspended

    vTaskDelay(100);                                    // Wait for all the transfer to finish
    msc_teardown();
}

#endif // MSC_HOST_SUSPEND_RESUME_API_SUPPORTED

/**
 * @brief USB MSC Device Mock
 *
 * We use this 'testcase' to provide MSC mock device for our DUT
 */
TEST_CASE("mock_device_app", "[usb_msc_device][ignore]")
{
    device_app();
}

#if SOC_SDMMC_HOST_SUPPORTED
TEST_CASE("mock_device_app", "[usb_msc_device_sdmmc][ignore]")
{
    device_app_sdmmc();
}
#endif /* SOC_SDMMC_HOST_SUPPORTED */

#endif /* SOC_USB_OTG_SUPPORTED */
