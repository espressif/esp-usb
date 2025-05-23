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
#include "device_handling.h"
#include "tinyusb.h"
#include "tinyusb_default_configs.h"
#include "tinyusb_msc.h"
//

#define TEST_STORAGE_MEDIA_SPIFLASH   1  // 1 for SPI Flash, 0 for SDMMC
#define TEST_QUEUE_LEN               10 // Length of the queue for storage events
#define STORAGE_EVENT_TIMEOUT_MS    5000 // Timeout for waiting storage events

// ========================== TinyUSB General Device Descriptors ===============================

#define EPNUM_MSC       1
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
};

static QueueHandle_t _test_storage_event_queue = NULL;

static esp_err_t storage_init_spiflash(wl_handle_t *wl_handle)
{
    const esp_partition_t *data_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, NULL);
    if (data_partition == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    return wl_mount(data_partition, wl_handle);
}

static esp_err_t storage_deinit_spiflash(wl_handle_t wl_handle)
{
    return wl_unmount(wl_handle);
}

/**
 * @brief Callback to handle MSC Storage mount changed events
 *
 * This callback is triggered when the storage mount state changes.
 *
 * @param event Pointer to the event data structure containing mount state information.
 */
static void test_storage_event_cb(tinyusb_msc_event_t *event)
{
    printf("Storage event\n");
    printf("\t-> %s, mounted to %s\n", storage_event_str[event->id], (event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB) ? "USB" : "APP");

    test_storage_event_t msg = {
        .event_id = event->id,
    };
    xQueueSend(_test_storage_event_queue, &msg, portMAX_DELAY);
}

/**
 */
void test_storage_wait_callback(tinyusb_msc_event_id_t event_id)
{
    TEST_ASSERT_NOT_NULL(_test_storage_event_queue);
    // Wait for port callback to send an event message
    test_storage_event_t msg;
    BaseType_t ret = xQueueReceive(_test_storage_event_queue, &msg, pdMS_TO_TICKS(STORAGE_EVENT_TIMEOUT_MS));
    TEST_ASSERT_EQUAL_MESSAGE(pdPASS, ret, "MSC storage event not generated on time");
    // Check the contents of that event message
    printf("\tMSC storage event: %s\n", storage_event_str[msg.event_id]);
    TEST_ASSERT_EQUAL_MESSAGE(event_id, msg.event_id, "Unexpected MSC storage event type received");
}


TEST_CASE("run MSC class", "[esp_tinyusb_msc][init]")
{
    _test_storage_event_queue = xQueueCreate(TEST_QUEUE_LEN, sizeof(test_storage_event_t));
    TEST_ASSERT_NOT_NULL(_test_storage_event_queue);

    tinyusb_msc_config_t config = {
#if (SOC_SDMMC_HOST_SUPPORTED)
        .storage_type = TEST_STORAGE_MEDIA_SPIFLASH ? TINYUSB_MSC_STORAGE_TYPE_SPIFLASH : TINYUSB_MSC_STORAGE_TYPE_SDMMC,
#else
        .storage_type = TINYUSB_MSC_STORAGE_TYPE_SPIFLASH,  // Default to SPI Flash storage
#endif // SOC_SDMMC_HOST_SUPPORTED
        .context = NULL,  // Context will be set later based on the storage type
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,  // Set the initial mount point to APP
        .mount_config = {
            .max_files = 5,  // Maximum number of files that can be opened simultaneously
        },
        .callback = test_storage_event_cb,  // Register the callback for mount changed events
        .callback_arg = NULL,  // No additional argument for the callback
    };

#ifdef TEST_STORAGE_MEDIA_SPIFLASH
    wl_handle_t wl_handle = WL_INVALID_HANDLE;
    ESP_ERROR_CHECK(storage_init_spiflash(&wl_handle));
    config.context = (void *) wl_handle;  // Set the context to the wear leveling handle

    ESP_LOGI("test_msc_callbacks", "SPI Flash storage initialized with handle: %p", wl_handle);

#else // TEST_STORAGE_MEDIA_SPIFLASH
    sdmmc_card_t *card = NULL;
    ESP_ERROR_CHECK(storage_init_sdmmc(&card));
    config.context = (void *) card;       // Set the context to the SDMMC card handle
#endif  // TEST_STORAGE_MEDIA_SPIFLASH

    // Initialize TinyUSB MSC storage
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_msc_storage_init(&config));
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

    vTaskDelay(pdMS_TO_TICKS(1000)); // Allow some time for the device to be recognized

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());

#if TEST_STORAGE_MEDIA_SPIFLASH
    TEST_ASSERT_EQUAL(ESP_OK, storage_deinit_spiflash(wl_handle));
#else // TEST_STORAGE_MEDIA_SPIFLASH
    TEST_ASSERT_NOT_NULL(card);
    ESP_ERROR_CHECK(storage_deinit_sdcard(card));
#endif // TEST_STORAGE_MEDIA_SPIFLASH

    tinyusb_msc_storage_deinit();
    vQueueDelete(_test_storage_event_queue);
}
#endif // SOC_USB_OTG_SUPPORTED
