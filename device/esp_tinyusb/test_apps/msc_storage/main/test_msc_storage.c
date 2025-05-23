/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "soc/soc_caps.h"

#if SOC_USB_OTG_SUPPORTED
//
#include <stdio.h>
#include <string.h>
//
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
//
#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
//
#include "unity.h"
#include "device_common.h"
#include "tinyusb.h"
#include "tinyusb_default_configs.h"
#include "tinyusb_msc.h"
#include "storage_common.h"

//
// ========================== Test Configuration Parameters =====================================
//

#define TEST_QUEUE_LEN                          6 // Length of the queue for storage events
#define TEST_STORAGE_EVENT_TIMEOUT_MS           5000 // Timeout for waiting storage events
#define TEST_STORAGE_DISK_PRESENCE_TIMEOUT_MS   5000 // Timeout for checking disk presence

//
// ========================== TinyUSB General Device Descriptors ===============================
//
#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)


static tusb_desc_device_t test_device_descriptor = {
    .bLength = sizeof(test_device_descriptor),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A, // This is Espressif VID. This needs to be changed according to Users / Customers
    .idProduct = 0x4002,
    .bcdDevice = 0x100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01
};

static uint8_t const test_fs_configuration_desc[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    // Interface number, string index, EP Out & EP In address, EP size
    TUD_MSC_DESCRIPTOR(0, 4, 0x01, 0x81, 64),
};

#if (TUD_OPT_HIGH_SPEED)
static const tusb_desc_device_qualifier_t test_device_qualifier = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 0x01,
    .bReserved = 0
};

static uint8_t const test_hs_configuration_desc[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    // Interface number, string index, EP Out & EP In address, EP size
    TUD_MSC_DESCRIPTOR(0, 4, 0x01, 0x81, 512),
};
#endif // TUD_OPT_HIGH_SPEED

static char const *string_desc_arr[] = {
    (const char[]) { 0x09, 0x04 },  // 0: is supported language is English (0x0409)
    "TinyUSB",                      // 1: Manufacturer
    "TinyUSB Device",               // 2: Product
    "123456",                       // 3: Serials
    "Test MSC Class",               // 4. MSC
};

//
typedef struct {
    tinyusb_msc_event_id_t event_id;  /*!< Event ID */
} test_storage_event_t;

const char *storage_event_str[] = {
    "Mount Start",
    "Mount Complete",
    "Mount Failed",
    "Format Required",
};

static QueueHandle_t _test_storage_event_queue = NULL;

//
// ========================== TinyUSB MSC Storage Event Handling =================================
//

/**
 * @brief Callback to handle MSC Storage mount changed events
 *
 * This callback is triggered when the storage mount state changes.
 *
 * @param event Pointer to the event data structure containing mount state information.
 */
static void test_storage_event_cb(tinyusb_msc_storage_handle_t handle, tinyusb_msc_event_t *event, void *arg)
{
    printf("Storage event\n");

    switch (event->id) {
    case TINYUSB_MSC_EVENT_MOUNT_START:
    case TINYUSB_MSC_EVENT_MOUNT_COMPLETE:
        printf("\t-> %s, mounted to %s\n", storage_event_str[event->id], (event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB) ? "USB" : "APP");
        break;
    case TINYUSB_MSC_EVENT_MOUNT_FAILED:
    case TINYUSB_MSC_EVENT_FORMAT_REQUIRED:
        printf("\t-> %s\n", storage_event_str[event->id]);
        break;
    default:
        printf("Unknown storage event: %d\n", event->id);
        TEST_ASSERT_MESSAGE(0, "Unknown storage event received");
        break;
    }

    test_storage_event_t msg = {
        .event_id = event->id,
    };
    xQueueSend(_test_storage_event_queue, &msg, portMAX_DELAY);
}

/**
 * @brief Wait for a specific storage event to be generated
 *
 * @param event_id The expected event ID to wait for
 */
void test_storage_wait_callback(tinyusb_msc_event_id_t event_id)
{
    TEST_ASSERT_NOT_NULL(_test_storage_event_queue);
    // Wait for port callback to send an event message
    test_storage_event_t msg;
    BaseType_t ret = xQueueReceive(_test_storage_event_queue, &msg, pdMS_TO_TICKS(TEST_STORAGE_EVENT_TIMEOUT_MS));
    TEST_ASSERT_EQUAL_MESSAGE(pdPASS, ret, "MSC storage event not generated on time");
    // Check the contents of that event message
    printf("\tMSC storage event: %s\n", storage_event_str[msg.event_id]);
    TEST_ASSERT_EQUAL_MESSAGE(event_id, msg.event_id, "Unexpected MSC storage event type received");
}

//
// ========================== TinyUSB MSC Storage Initialization Tests =============================
//

/**
 * @brief Test case for checking the consistency of the public API functions
 *
 * This test case verifies that all public API functions are defined and have the expected signatures.
 */
TEST_CASE("MSC: Public API consistency", "[ci][driver]")
{
    // Check that the public API functions are consistent with the expected signatures
    TEST_ASSERT_NOT_NULL(tinyusb_msc_install_driver);
    TEST_ASSERT_NOT_NULL(tinyusb_msc_uninstall_driver);
    TEST_ASSERT_NOT_NULL(tinyusb_msc_new_storage_spiflash);
#if (SOC_SDMMC_HOST_SUPPORTED)
    TEST_ASSERT_NOT_NULL(tinyusb_msc_new_storage_sdmmc);
#endif // SOC_SDMMC_HOST_SUPPORTED
    TEST_ASSERT_NOT_NULL(tinyusb_msc_delete_storage);
    TEST_ASSERT_NOT_NULL(tinyusb_msc_set_storage_callback);
    TEST_ASSERT_NOT_NULL(tinyusb_msc_set_storage_mount_point);
    TEST_ASSERT_NOT_NULL(tinyusb_msc_config_storage_fat_fs);
    TEST_ASSERT_NOT_NULL(tinyusb_msc_format_storage);
    TEST_ASSERT_NOT_NULL(tinyusb_msc_get_storage_capacity);
    TEST_ASSERT_NOT_NULL(tinyusb_msc_get_storage_sector_size);
    TEST_ASSERT_NOT_NULL(tinyusb_msc_get_storage_mount_point);

    // Functions signatures should match the expected ones and do not fall during compilation
    // Driver
    tinyusb_msc_driver_config_t config = { 0 };
    tinyusb_msc_install_driver(&config);
    tinyusb_msc_uninstall_driver();

    // Storage
    tinyusb_msc_storage_handle_t storage_hdl = NULL;
    tinyusb_msc_storage_config_t storage_config = { 0 };
    tinyusb_msc_new_storage_spiflash(&storage_config, &storage_hdl);
#if (SOC_SDMMC_HOST_SUPPORTED)
    tinyusb_msc_new_storage_sdmmc(&storage_config, &storage_hdl);
#endif // SOC_SDMMC_HOST_SUPPORTED
    tinyusb_msc_delete_storage(storage_hdl);

    // Setters, Getters & Config
    tinyusb_msc_set_storage_callback(test_storage_event_cb, NULL);
    tinyusb_msc_set_storage_mount_point(storage_hdl, TINYUSB_MSC_STORAGE_MOUNT_USB);
    tinyusb_msc_config_storage_fat_fs(storage_hdl, NULL);
    tinyusb_msc_format_storage(storage_hdl);
    uint32_t sector_count = 0;
    tinyusb_msc_get_storage_capacity(storage_hdl, &sector_count);
    uint32_t sector_size = 0;
    tinyusb_msc_get_storage_sector_size(storage_hdl, &sector_size);
    tinyusb_msc_mount_point_t mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB;
    tinyusb_msc_get_storage_mount_point(storage_hdl, &mount_point);
}

/**
 * @brief Test case for installing TinyUSB MSC driver without storage
 *
 * Scenario:
 * 1. Install TinyUSB MSC driver without storage.
 * 2. Install TinyUSB driver with the MSC configuration.
 * 3. Wait for the device to be recognized.
 * 4. Uninstall TinyUSB driver and deinitialize the MSC driver.
 */
TEST_CASE("MSC: driver install & uninstall without storage", "[ci][driver]")
{
    tinyusb_msc_driver_config_t driver_cfg = {
        .callback = NULL,                           // Register the callback for mount changed events
        .callback_arg = NULL,                       // No additional argument for the callback
    };
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_install_driver(&driver_cfg), "Failed to install TinyUSB MSC driver");

    // Install TinyUSB driver
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(test_device_event_handler);

    tusb_cfg.descriptor.device = &test_device_descriptor;
    tusb_cfg.descriptor.full_speed_config = test_fs_configuration_desc;
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.qualifier = &test_device_qualifier;
    tusb_cfg.descriptor.high_speed_config = test_hs_configuration_desc;
#endif // TUD_OPT_HIGH_SPEED
    tusb_cfg.descriptor.string = string_desc_arr;
    tusb_cfg.descriptor.string_count = sizeof(string_desc_arr) / sizeof(string_desc_arr[0]);

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    test_device_wait();

    // No storage events without storage

    vTaskDelay(pdMS_TO_TICKS(TEST_STORAGE_DISK_PRESENCE_TIMEOUT_MS)); // Allow some time for the device to be recognized

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_uninstall_driver(), "Failed to uninstall TinyUSB MSC driver");
}

/**
 * @brief Test case for installing TinyUSB MSC storage without installing the driver
 *
 * Scenario:
 * 1. Init SPI Flash storage to obtain the wear leveling handle.
 * 2. Create TinyUSB MSC Storage with SPI Flash.
 * 3. Wait for the device to be recognized.
 * 4. Uninstall TinyUSB driver, delete storage and cleanup test.
 */
TEST_CASE("MSC: enable storage without driver install", "[ci][driver]")
{
    _test_storage_event_queue = xQueueCreate(TEST_QUEUE_LEN, sizeof(test_storage_event_t));
    TEST_ASSERT_NOT_NULL(_test_storage_event_queue);

    wl_handle_t wl_handle = WL_INVALID_HANDLE;
    storage_init_spiflash(&wl_handle);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(WL_INVALID_HANDLE, wl_handle, "Wear leveling handle is invalid, check the partition configuration");

    tinyusb_msc_storage_config_t config = {
        .medium.wl_handle = wl_handle,                      // Set the context to the wear leveling handle
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,       // Initial mount point to USB
        .fat_fs = {
            .base_path = NULL,                              // Use default base path
            .config.max_files = 5,                          // Maximum number of files that can be opened simultaneously
            .format_flags = 0,                              // No special format flags
        },
    };

    // Initialize TinyUSB MSC storage
    tinyusb_msc_storage_handle_t storage_hdl = NULL;
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_new_storage_spiflash(&config, &storage_hdl), "Failed to initialize TinyUSB MSC storage with SPIFLASH");
    // Configure the callback for mount changed events
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_set_storage_callback(test_storage_event_cb, NULL), "Failed to set storage event callback");

    // Install TinyUSB driver
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(test_device_event_handler);

    tusb_cfg.descriptor.device = &test_device_descriptor;
    tusb_cfg.descriptor.full_speed_config = test_fs_configuration_desc;
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.qualifier = &test_device_qualifier;
    tusb_cfg.descriptor.high_speed_config = test_hs_configuration_desc;
#endif // TUD_OPT_HIGH_SPEED
    tusb_cfg.descriptor.string = string_desc_arr;
    tusb_cfg.descriptor.string_count = sizeof(string_desc_arr) / sizeof(string_desc_arr[0]);

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    test_device_wait();
    vTaskDelay(pdMS_TO_TICKS(TEST_STORAGE_DISK_PRESENCE_TIMEOUT_MS)); // Allow some time for the device to be recognized

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_delete_storage(storage_hdl), "Failed to delete TinyUSB MSC storage");
    storage_deinit_spiflash(wl_handle);
    vQueueDelete(_test_storage_event_queue);
}

/**
 * @brief Test case for initializing TinyUSB MSC storage with SPIFLASH
 *
 * Scenario:
 * 1. Create a queue for storage events.
 * 2. Initialize SPIFLASH storage with wear levelling.
 * 3. Configure TinyUSB MSC with the SPIFLASH storage and mounting to APP.
 * 4. Install TinyUSB driver with the MSC configuration.
 * 5. Wait for the storage to be mounted, verify that re-mount callbacks are received.
 * 7. Uninstall TinyUSB driver and deinitialize SPIFLASH storage.
 * 8. Delete the storage event queue.
 */
TEST_CASE("MSC: storage SPIFLASH", "[ci][storage][spiflash]")
{
    _test_storage_event_queue = xQueueCreate(TEST_QUEUE_LEN, sizeof(test_storage_event_t));
    TEST_ASSERT_NOT_NULL(_test_storage_event_queue);

    wl_handle_t wl_handle = WL_INVALID_HANDLE;
    storage_init_spiflash(&wl_handle);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(WL_INVALID_HANDLE, wl_handle, "Wear leveling handle is invalid, check the partition configuration");

    tinyusb_msc_driver_config_t driver_cfg = {
        .callback = test_storage_event_cb,                  // Register the callback for mount changed events
        .callback_arg = NULL,                               // No additional argument for the callback
    };
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_install_driver(&driver_cfg), "Failed to install TinyUSB MSC driver");

    tinyusb_msc_storage_config_t config = {
        .medium.wl_handle = wl_handle,                      // Set the context to the wear leveling handle
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,       // Initial mount point to APP
        .fat_fs = {
            .base_path = NULL,                              // Use default base path
            .config.max_files = 5,                          // Maximum number of files that can be opened simultaneously
            .format_flags = 0,                              // No special format flags
        },
    };

    // Initialize TinyUSB MSC storage
    tinyusb_msc_storage_handle_t storage_hdl = NULL;
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_new_storage_spiflash(&config, &storage_hdl), "Failed to initialize TinyUSB MSC storage with SPIFLASH");
    // Wait for the storage to be mounted
    test_storage_wait_callback(TINYUSB_MSC_EVENT_MOUNT_START);
    test_storage_wait_callback(TINYUSB_MSC_EVENT_MOUNT_COMPLETE);

    // Install TinyUSB driver
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(test_device_event_handler);

    tusb_cfg.descriptor.device = &test_device_descriptor;
    tusb_cfg.descriptor.full_speed_config = test_fs_configuration_desc;
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.qualifier = &test_device_qualifier;
    tusb_cfg.descriptor.high_speed_config = test_hs_configuration_desc;
#endif // TUD_OPT_HIGH_SPEED
    tusb_cfg.descriptor.string = string_desc_arr;
    tusb_cfg.descriptor.string_count = sizeof(string_desc_arr) / sizeof(string_desc_arr[0]);

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    test_device_wait();
    test_storage_wait_callback(TINYUSB_MSC_EVENT_MOUNT_START);
    test_storage_wait_callback(TINYUSB_MSC_EVENT_MOUNT_COMPLETE);

    vTaskDelay(pdMS_TO_TICKS(TEST_STORAGE_DISK_PRESENCE_TIMEOUT_MS)); // Allow some time for the device to be recognized

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_delete_storage(storage_hdl), "Failed to delete TinyUSB MSC storage");
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_uninstall_driver(), "Failed to uninstall TinyUSB MSC driver");
    storage_deinit_spiflash(wl_handle);
    vQueueDelete(_test_storage_event_queue);
}

#if (SOC_SDMMC_HOST_SUPPORTED)
/**
 * @brief Test case for initializing TinyUSB MSC storage with SDMMC
 *
 * Scenario:
 * 1. Create a queue for storage events.
 * 2. Initialize SDMMC storage.
 * 3. Configure TinyUSB MSC with the SDMMC storage mounted to APP.
 * 4. Verify that mount callbacks are registered correctly.
 * 5. Install TinyUSB driver with the MSC configuration.
 * 6. Wait for the storage to be mounted, verify that re-mount callbacks are received.
 * 7. Uninstall TinyUSB driver and deinitialize SDMMC storage.
 * 8. Delete the storage event queue.
 */
TEST_CASE("MSC: storage SDMMC", "[storage][sdmmc]")
{
    _test_storage_event_queue = xQueueCreate(TEST_QUEUE_LEN, sizeof(test_storage_event_t));
    TEST_ASSERT_NOT_NULL(_test_storage_event_queue);

    sdmmc_card_t *card = NULL;
    storage_init_sdmmc(&card);
    TEST_ASSERT_NOT_NULL_MESSAGE(card, "SDMMC card handle is NULL, check the SDMMC configuration");

    tinyusb_msc_driver_config_t driver_cfg = {
        .callback = test_storage_event_cb,                  // Register the callback for mount changed events
        .callback_arg = NULL,                               // No additional argument for the callback
    };
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_install_driver(&driver_cfg), "Failed to install TinyUSB MSC driver");

    tinyusb_msc_storage_config_t config = {
        .medium.card = card,                                // Set the context to the SDMMC card handle
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,       // Initial mount point to APP
        .fat_fs = {
            .base_path = NULL,                              // Use default base path
            .config.max_files = 5,                          // Maximum number of files that can be opened simultaneously
            .format_flags = 0,                              // No special format flags
        },
    };

    // Initialize TinyUSB MSC storage
    tinyusb_msc_storage_handle_t storage_hdl = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_msc_new_storage_sdmmc(&config, &storage_hdl));
    // Wait for the storage to be mounted
    test_storage_wait_callback(TINYUSB_MSC_EVENT_MOUNT_START);
    test_storage_wait_callback(TINYUSB_MSC_EVENT_MOUNT_COMPLETE);

    // Install TinyUSB driver
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(test_device_event_handler);

    tusb_cfg.descriptor.device = &test_device_descriptor;
    tusb_cfg.descriptor.full_speed_config = test_fs_configuration_desc;
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.qualifier = &test_device_qualifier;
    tusb_cfg.descriptor.high_speed_config = test_hs_configuration_desc;
#endif // TUD_OPT_HIGH_SPEED
    tusb_cfg.descriptor.string = string_desc_arr;
    tusb_cfg.descriptor.string_count = sizeof(string_desc_arr) / sizeof(string_desc_arr[0]);

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    test_device_wait();
    test_storage_wait_callback(TINYUSB_MSC_EVENT_MOUNT_START);
    test_storage_wait_callback(TINYUSB_MSC_EVENT_MOUNT_COMPLETE);

    // Verify card capacity and sector size
    uint32_t capacity = 0;
    uint32_t sector_size = 0;

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_msc_get_storage_capacity(NULL, &capacity));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_msc_get_storage_sector_size(NULL, &sector_size));

    TEST_ASSERT_EQUAL_MESSAGE(card->csd.capacity, capacity, "SDMMC card capacity does not match TinyUSB MSC storage sector count");
    TEST_ASSERT_EQUAL_MESSAGE(card->csd.sector_size, sector_size, "SDMMC card sector size does not match TinyUSB MSC storage sector size");

    vTaskDelay(pdMS_TO_TICKS(TEST_STORAGE_DISK_PRESENCE_TIMEOUT_MS)); // Allow some time for the device to be recognized

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_delete_storage(storage_hdl), "Failed to delete TinyUSB MSC storage");
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_uninstall_driver(), "Failed to uninstall TinyUSB MSC driver");
    storage_deinit_sdmmc(card);
    vQueueDelete(_test_storage_event_queue);
}
#endif // SOC_SDMMC_HOST_SUPPORTED

/**
 * @brief Test case for initializing TinyUSB MSC storage with a specific base path
 *
 * Scenario:
 * 1. Create a queue for storage events.
 * 2. Initialize SPIFLASH storage with wear levelling.
 * 3. Configure TinyUSB MSC with the SPIFLASH storage and mounting to APP with a custom base path.
 * 4. Install TinyUSB driver with the MSC configuration.
 * 5. Wait for the storage to be mounted, verify that re-mount callbacks are received.
 * 6. Uninstall TinyUSB driver and deinitialize SPIFLASH storage.
 * 7. Delete the storage event queue.
 */
TEST_CASE("MSC: storage specific base path", "[ci][storage][spiflash]")
{
    _test_storage_event_queue = xQueueCreate(TEST_QUEUE_LEN, sizeof(test_storage_event_t));
    TEST_ASSERT_NOT_NULL(_test_storage_event_queue);

    wl_handle_t wl_handle = WL_INVALID_HANDLE;
    storage_init_spiflash(&wl_handle);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(WL_INVALID_HANDLE, wl_handle, "Wear leveling handle is invalid, check the partition configuration");

    tinyusb_msc_driver_config_t driver_cfg = {
        .callback = test_storage_event_cb,                  // Register the callback for mount changed events
        .callback_arg = NULL,                               // No additional argument for the callback
    };
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_install_driver(&driver_cfg), "Failed to install TinyUSB MSC driver");

    tinyusb_msc_storage_config_t config = {
        .medium.wl_handle = wl_handle,                      // Set the context to the wear leveling handle
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,       // Initial mount point to APP
        .fat_fs = {
            .base_path = "/custom1",                        // Use specific base path
            .config.max_files = 5,                          // Maximum number of files that can be opened simultaneously
            .format_flags = 0,                              // No special format flags
        },
    };

    // Initialize TinyUSB MSC storage
    tinyusb_msc_storage_handle_t storage_hdl = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_msc_new_storage_spiflash(&config, &storage_hdl));
    // Wait for the storage to be mounted
    test_storage_wait_callback(TINYUSB_MSC_EVENT_MOUNT_START);
    test_storage_wait_callback(TINYUSB_MSC_EVENT_MOUNT_COMPLETE);

    // Install TinyUSB driver
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(test_device_event_handler);

    tusb_cfg.descriptor.device = &test_device_descriptor;
    tusb_cfg.descriptor.full_speed_config = test_fs_configuration_desc;
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.qualifier = &test_device_qualifier;
    tusb_cfg.descriptor.high_speed_config = test_hs_configuration_desc;
#endif // TUD_OPT_HIGH_SPEED
    tusb_cfg.descriptor.string = string_desc_arr;
    tusb_cfg.descriptor.string_count = sizeof(string_desc_arr) / sizeof(string_desc_arr[0]);

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    test_device_wait();
    test_storage_wait_callback(TINYUSB_MSC_EVENT_MOUNT_START);
    test_storage_wait_callback(TINYUSB_MSC_EVENT_MOUNT_COMPLETE);

    vTaskDelay(pdMS_TO_TICKS(TEST_STORAGE_DISK_PRESENCE_TIMEOUT_MS)); // Allow some time for the device to be recognized

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_delete_storage(storage_hdl), "Failed to delete TinyUSB MSC storage");
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_uninstall_driver(), "Failed to uninstall TinyUSB MSC driver");
    storage_deinit_spiflash(wl_handle);
    vQueueDelete(_test_storage_event_queue);
}

/**
 * @brief Test case for initializing TinyUSB MSC storage with SPIFLASH and do not format option
 *
 * Scenario:
 * 1. Create a queue for storage events.
 * 2. Erase the SPIFLASH storage.
 * 3. Initialize SPIFLASH storage with wear levelling.
 * 4. Configure TinyUSB MSC with the SPIFLASH storage and mounting to APP with do not format option.
 * 5. Verify callback presence for mount START and FORMAT_REQUIRED events.
 * 6. Uninstall TinyUSB MSC driver and deinitialize SPIFLASH storage.
 * 7. Delete the storage event queue.
 */
TEST_CASE("MSC: storage do not format", "[ci][storage][spiflash]")
{
    _test_storage_event_queue = xQueueCreate(TEST_QUEUE_LEN, sizeof(test_storage_event_t));
    TEST_ASSERT_NOT_NULL(_test_storage_event_queue);

    storage_erase_spiflash();

    wl_handle_t wl_handle = WL_INVALID_HANDLE;
    storage_init_spiflash(&wl_handle);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(WL_INVALID_HANDLE, wl_handle, "Wear leveling handle is invalid, check the partition configuration");


    tinyusb_msc_driver_config_t driver_cfg = {
        .callback = test_storage_event_cb,                  // Register the callback for mount changed events
        .callback_arg = NULL,                               // No additional argument for the callback
    };
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_install_driver(&driver_cfg), "Failed to install TinyUSB MSC driver");

    tinyusb_msc_storage_config_t config = {
        .medium.wl_handle = wl_handle,                      // Set the context to the wear leveling handle
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,       // Initial mount point to APP
        .fat_fs = {
            .base_path = NULL,                              // Use default base path
            .do_not_format = true,                          // Do not format the drive if filesystem is not present
            .config.max_files = 5,                          // Maximum number of files that can be opened simultaneously
            .format_flags = 0,                              // No special format flags
        },
    };

    // Initialize TinyUSB MSC storage
    tinyusb_msc_storage_handle_t storage_hdl = NULL;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, tinyusb_msc_new_storage_spiflash(&config, &storage_hdl));
    test_storage_wait_callback(TINYUSB_MSC_EVENT_MOUNT_START);
    test_storage_wait_callback(TINYUSB_MSC_EVENT_FORMAT_REQUIRED);

    // 4.TODO: Storage Format support

    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_delete_storage(storage_hdl), "Failed to delete TinyUSB MSC storage");
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_uninstall_driver(), "Failed to uninstall TinyUSB MSC driver");
    storage_deinit_spiflash(wl_handle);
    vQueueDelete(_test_storage_event_queue);
}


#endif // SOC_USB_OTG_SUPPORTED
