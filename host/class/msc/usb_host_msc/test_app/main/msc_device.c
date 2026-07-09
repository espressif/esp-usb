/*
 * SPDX-FileCopyrightText: 2022-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */


#include "esp_log.h"
#include "unity.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "soc/soc_caps.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "tinyusb_msc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#if SOC_SDMMC_HOST_SUPPORTED
#include "sdmmc_cmd.h"
#endif /* SOC_SDMMC_HOST_SUPPORTED */

#if SOC_USB_OTG_SUPPORTED

#define DEV_EVT_QUEUE_LENGTH                5

static QueueHandle_t dev_evt_queue = NULL;
static StaticQueue_t s_queue_buf;
static uint8_t ucQueueStorage[DEV_EVT_QUEUE_LENGTH * sizeof(tinyusb_event_t)];

static const char *TAG = "msc_example";

/* TinyUSB descriptors
   ********************************************************************* */
#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

enum {
    ITF_NUM_MSC = 0,
    ITF_NUM_TOTAL
};

enum {
    EDPT_MSC_OUT  = 0x01,
    EDPT_MSC_IN   = 0x81,
};

static uint8_t const msc_fs_desc_configuration[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    // Interface number, string index, EP Out & EP In address, EP size
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EDPT_MSC_OUT, EDPT_MSC_IN, 64),
};

#if (TUD_OPT_HIGH_SPEED)
static const uint8_t msc_hs_desc_configuration[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    // Interface number, string index, EP Out & EP In address, EP size
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EDPT_MSC_OUT, EDPT_MSC_IN, 512),
};

static const tusb_desc_device_qualifier_t device_qualifier = {
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
#endif // TUD_OPT_HIGH_SPEED

static tusb_desc_device_t descriptor_config = {
    .bLength = sizeof(descriptor_config),
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

static char const *string_desc_arr[] = {
    (const char[]) { 0x09, 0x04 },  // 0: is supported language is English (0x0409)
    "TinyUSB",                      // 1: Manufacturer
    "TinyUSB Device",               // 2: Product
    // We intentionally do not implement Serial String descriptor to make sure that the driver can handle it
    //"123456",                       // 3: Serials
    //"Test MSC",                  // 4. MSC
};

/**
 * @brief TinyUSB device events handler
 *
 * @param[in] event Device event
 * @param[in] arg User arguments
 */
static void device_event_handler(tinyusb_event_t *event, void *arg)
{
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        ESP_LOGI(TAG, "TinyUSB event: Device attached");
        break;
    case TINYUSB_EVENT_DETACHED:
        ESP_LOGI(TAG, "TinyUSB event: Device detached");
        break;
    case TINYUSB_EVENT_SUSPENDED:
        ESP_LOGI(TAG, "TinyUSB event: Device suspended");
        break;
    case TINYUSB_EVENT_RESUMED:
        ESP_LOGI(TAG, "TinyUSB event: Device resumed");
        break;
    default:
        break;
    }

    xQueueSend(dev_evt_queue, event, 0);
}

static void usb_device_init(void)
{
    // Create static app message queue
    dev_evt_queue = xQueueCreateStatic(DEV_EVT_QUEUE_LENGTH, sizeof(tinyusb_event_t), &(ucQueueStorage[0]), &s_queue_buf);
    configASSERT(dev_evt_queue);

    ESP_LOGI(TAG, "USB MSC initialization");

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(device_event_handler);
    tusb_cfg.descriptor.device = &descriptor_config;
    tusb_cfg.descriptor.full_speed_config = msc_fs_desc_configuration;
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.high_speed_config = msc_hs_desc_configuration;
    tusb_cfg.descriptor.qualifier = &device_qualifier;
#endif // TUD_OPT_HIGH_SPEED
    tusb_cfg.descriptor.string = string_desc_arr;
    tusb_cfg.descriptor.string_count = sizeof(string_desc_arr) / sizeof(string_desc_arr[0]);

    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    ESP_LOGI(TAG, "USB initialization DONE");
}

static esp_err_t storage_init_spiflash(wl_handle_t *wl_handle)
{
    ESP_LOGI(TAG, "Initializing wear levelling");

    const esp_partition_t *data_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, NULL);
    if (data_partition == NULL) {
        ESP_LOGE(TAG, "Failed to find FATFS partition. Check the partition table.");
        return ESP_ERR_NOT_FOUND;
    }

    return wl_mount(data_partition, wl_handle);
}

static void msc_mock_device_run(void)
{
    ESP_LOGI(TAG, "Initialization");

    static wl_handle_t wl_handle = WL_INVALID_HANDLE;
    ESP_ERROR_CHECK(storage_init_spiflash(&wl_handle));

    const tinyusb_msc_storage_config_t config = {
        .medium.wl_handle = wl_handle,  // Set the medium of the storage to the wear leveling
    };
    ESP_ERROR_CHECK(tinyusb_msc_new_storage_spiflash(&config, NULL));

    usb_device_init();
}

/**
 * @brief Do a connection/disconnection delay
 *
 * @param[in] delay_ms Delay in ms
 */
static void dconn_conn_delay(size_t delay_ms)
{
    if (delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

/**
 * @brief Run MSC Device
 * Device performs sudden disconnect followed by a connect upon receiving Suspend callback
 *
 * @param[in] dconn_delay_ms Delay in ms before disconnecting the port
 * @param[in] conn_delay_ms Delay in ms before connecting the port back
 */
static void device_suspend_common(size_t dconn_delay_ms, size_t conn_delay_ms)
{
    tinyusb_event_id_t mask_event = UINT32_MAX;

    while (1) {
        tinyusb_event_t dev_event;
        if (xQueueReceive(dev_evt_queue, &dev_event, portMAX_DELAY)) {

            if (mask_event == dev_event.id) {
                ESP_LOGI(TAG, "Event %d masked", dev_event.id);
                mask_event = UINT32_MAX;    // Refresh masked event, allowing the device test app to handle further events without hard reset
                continue;
            }

            switch (dev_event.id) {
            case TINYUSB_EVENT_SUSPENDED:

                // Optional delay before disconnection (depends on the test case whether sudden disconnect, or just disconnect)
                dconn_conn_delay(dconn_delay_ms);

                // Disconnect
                TEST_ASSERT(tud_disconnect());
                ESP_LOGI(TAG, "Triggering disconnect");

                // Stay disconnected for a while and trigger connect
                dconn_conn_delay(conn_delay_ms);

                ESP_LOGI(TAG, "Triggering connect");
                TEST_ASSERT(tud_connect());

                // End of the test case: Mask next suspend event (auto suspend) from the device after uninstalling the cdc-acm host
                mask_event = TINYUSB_EVENT_SUSPENDED;
                break;
            default:
                break;
            }
        }
    }
}

/**
 * @brief USB MSC Device Mock
 *
 * We use this 'testcase' to provide MSC mock device for our DUT
 */
TEST_CASE("mock_device_app", "[usb_msc_device][spiflash][default][ignore]")
{
    msc_mock_device_run();

    while (1) {
        vTaskDelay(10);
    }
}

TEST_CASE("mock_device_sudden_dconn", "[usb_msc_device][spiflash][suspend_sudden_dconn][ignore]")
{
    msc_mock_device_run();

    device_suspend_common(0, 1000);
}

#if SOC_SDMMC_HOST_SUPPORTED
static esp_err_t storage_init_sdmmc(sdmmc_card_t **card)
{
    esp_err_t ret = ESP_OK;
    bool host_init = false;
    sdmmc_card_t *sd_card;

    ESP_LOGI(TAG, "Initializing SDCard");

    // By default, SD card frequency is initialized to SDMMC_FREQ_DEFAULT (20MHz)
    // For setting a specific frequency, use host.max_freq_khz (range 400kHz - 40MHz for SDMMC)
    // Example: for fixed frequency of 10MHz, use host.max_freq_khz = 10000;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    // This initializes the slot without card detect (CD) and write protect (WP) signals.
    // Modify slot_config.gpio_cd and slot_config.gpio_wp if your board has these signals.
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

#ifdef CONFIG_TEST_SDMMC_BUS_WIDTH_4
    slot_config.width = 4;
#else
    slot_config.width = 1;
#endif  // CONFIG_TEST_SDMMC_BUS_WIDTH_4

    // On chips where the GPIOs used for SD card can be configured, set the user defined values
#ifdef CONFIG_SOC_SDMMC_USE_GPIO_MATRIX
    slot_config.clk = CONFIG_TEST_SDMMC_PIN_CLK;
    slot_config.cmd = CONFIG_TEST_SDMMC_PIN_CMD;
    slot_config.d0 = CONFIG_TEST_SDMMC_PIN_D0;
#ifdef CONFIG_TEST_SDMMC_BUS_WIDTH_4
    slot_config.d1 = CONFIG_TEST_SDMMC_PIN_D1;
    slot_config.d2 = CONFIG_TEST_SDMMC_PIN_D2;
    slot_config.d3 = CONFIG_TEST_SDMMC_PIN_D3;
#endif  // CONFIG_TEST_SDMMC_BUS_WIDTH_4
#endif  // CONFIG_SOC_SDMMC_USE_GPIO_MATRIX

    // Enable internal pullups on enabled pins. The internal pullups
    // are insufficient however, please make sure 10k external pullups are
    // connected on the bus. This is for debug / example purpose only.
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    // not using ff_memalloc here, as allocation in internal RAM is preferred
    sd_card = (sdmmc_card_t *)malloc(sizeof(sdmmc_card_t));
    ESP_GOTO_ON_FALSE(sd_card, ESP_ERR_NO_MEM, clean, TAG, "could not allocate new sdmmc_card_t");

    ESP_GOTO_ON_ERROR((*host.init)(), clean, TAG, "Host Config Init fail");
    host_init = true;

    ESP_GOTO_ON_ERROR(sdmmc_host_init_slot(host.slot, (const sdmmc_slot_config_t *) &slot_config),
                      clean, TAG, "Host init slot fail");

    ESP_GOTO_ON_ERROR(sdmmc_card_init(&host, sd_card),
                      clean, TAG, "The detection pin of the slot is disconnected");

    *card = sd_card;

    return ESP_OK;

clean:
    if (host_init) {
        if (host.flags & SDMMC_HOST_FLAG_DEINIT_ARG) {
            host.deinit_p(host.slot);
        } else {
            (*host.deinit)();
        }
    }
    if (sd_card) {
        free(sd_card);
        sd_card = NULL;
    }
    return ret;
}

void device_app_sdmmc(void)
{
    ESP_LOGI(TAG, "Initializing storage...");
    static sdmmc_card_t *card = NULL;
    ESP_ERROR_CHECK(storage_init_sdmmc(&card));

    const tinyusb_msc_storage_config_t config = {
        .medium.card = card,  // Set the medium of the storage to the SDMMC card
    };
    ESP_ERROR_CHECK(tinyusb_msc_new_storage_sdmmc(&config, NULL));

    usb_device_init();
}

TEST_CASE("mock_device_app", "[usb_msc_device][sdmmc][default][ignore]")
{
    device_app_sdmmc();

    while (1) {
        vTaskDelay(10);
    }
}

TEST_CASE("mock_device_sudden_dconn", "[usb_msc_device][sdmmc][suspend_sudden_dconn][ignore]")
{
    device_app_sdmmc();

    device_suspend_common(0, 1000);
}
#endif /* SOC_SDMMC_HOST_SUPPORTED */

#endif /* SOC_USB_OTG_SUPPORTED */
