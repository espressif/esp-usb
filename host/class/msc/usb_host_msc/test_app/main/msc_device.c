/*
 * SPDX-FileCopyrightText: 2022-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */


#include "esp_log.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "soc/soc_caps.h"
#include "test_common.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "tinyusb_msc.h"
#if SOC_SDMMC_HOST_SUPPORTED
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#endif /* SOC_SDMMC_HOST_SUPPORTED */

#if SOC_USB_OTG_SUPPORTED

/* sd-card configuration to be done by user */
#if SOC_SDMMC_HOST_SUPPORTED
#define SDMMC_BUS_WIDTH 4 /* Select the bus width of SD or MMC interface (4 or 1).
                            Note that even if 1 line mode is used, D3 pin of the SD card must
                            have a pull-up resistor connected. Otherwise the card may enter
                            SPI mode, the only way to recover from which is to cycle power to the card. */
#define PIN_CMD         35 /* CMD GPIO number */
#define PIN_CLK         36 /* CLK GPIO number */
#define PIN_D0          37 /* D0 GPIO number */
#define PIN_D1          38 /* D1 GPIO number (applicable when width SDMMC_BUS_WIDTH is 4) */
#define PIN_D2          33 /* D2 GPIO number (applicable when width SDMMC_BUS_WIDTH is 4) */
#define PIN_D3          34 /* D3 GPIO number (applicable when width SDMMC_BUS_WIDTH is 4) */
#endif /* SOC_SDMMC_HOST_SUPPORTED */

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
/*********************************************************************** TinyUSB descriptors*/

#define VBUS_MONITORING_GPIO_NUM GPIO_NUM_4
static void configure_vbus_monitoring(void)
{
    // Configure GPIO Pin for vbus monitoring
    const gpio_config_t vbus_gpio_config = {
        .pin_bit_mask = BIT64(VBUS_MONITORING_GPIO_NUM),
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_DISABLE,
        .pull_up_en = true,
        .pull_down_en = false,
    };
    ESP_ERROR_CHECK(gpio_config(&vbus_gpio_config));
}

static void storage_init(void)
{
    ESP_LOGI(TAG, "USB MSC initialization");

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = &descriptor_config;
    tusb_cfg.descriptor.full_speed_config = msc_fs_desc_configuration;
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.high_speed_config = msc_hs_desc_configuration;
    tusb_cfg.descriptor.qualifier = &device_qualifier;
#endif // TUD_OPT_HIGH_SPEED
    tusb_cfg.descriptor.string = string_desc_arr;
    tusb_cfg.descriptor.string_count = sizeof(string_desc_arr) / sizeof(string_desc_arr[0]);
    tusb_cfg.phy.self_powered = true;
    tusb_cfg.phy.vbus_monitor_io = VBUS_MONITORING_GPIO_NUM;

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

void device_app(void)
{
    ESP_LOGI(TAG, "Initializing storage...");
    configure_vbus_monitoring();

    static wl_handle_t wl_handle = WL_INVALID_HANDLE;
    ESP_ERROR_CHECK(storage_init_spiflash(&wl_handle));

    const tinyusb_msc_storage_config_t config = {
        .medium.wl_handle = wl_handle,  // Set the medium of the storage to the wear leveling
    };
    ESP_ERROR_CHECK(tinyusb_msc_new_storage_spiflash(&config, NULL));

    storage_init();
    while (1) {
        vTaskDelay(100);
    }
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

    if (SDMMC_BUS_WIDTH == 4) {
        slot_config.width = 4;
    } else {
        slot_config.width = 1;
    }

    // On chips where the GPIOs used for SD card can be configured, set the user defined values
#ifdef CONFIG_SOC_SDMMC_USE_GPIO_MATRIX
    slot_config.clk = PIN_CLK;
    slot_config.cmd = PIN_CMD;
    slot_config.d0 = PIN_D0;
    if (SDMMC_BUS_WIDTH == 4) {
        slot_config.d1 = PIN_D1;
        slot_config.d2 = PIN_D2;
        slot_config.d3 = PIN_D3;
    }
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
    configure_vbus_monitoring();
    static sdmmc_card_t *card = NULL;
    ESP_ERROR_CHECK(storage_init_sdmmc(&card));

    const tinyusb_msc_storage_config_t config = {
        .medium.card = card,  // Set the medium of the storage to the SDMMC card
    };
    ESP_ERROR_CHECK(tinyusb_msc_new_storage_sdmmc(&config, NULL));

    storage_init();
    while (1) {
        vTaskDelay(100);
    }
}
#endif /* SOC_SDMMC_HOST_SUPPORTED */

#endif /* SOC_USB_OTG_SUPPORTED */
