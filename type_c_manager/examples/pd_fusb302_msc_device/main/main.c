/*
 * SPDX-FileCopyrightText: 2022-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdlib.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "wear_levelling.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "esp_typec_pd.h"
#include "esp_typec_manager.h"

#define I2C_SDA            7
#define I2C_SCL            8
#define INT_GPIO           9
#define FUSB_ADDR_7B       0x22
#define SRC_VBUS_GPIO      28  // VBUS enable (active-high)
#define SRC_VBUS_GPIO_N    15  // VBUS enable (active-low, e.g., PW2609A)

static const char *TAG = "example_pd_msc_device";

static void tusb_evt_handler(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        ESP_LOGI(TAG, "TinyUSB: configured by host");
        break;
    case TINYUSB_EVENT_DETACHED:
        ESP_LOGI(TAG, "TinyUSB: detached from host");
        break;
    default:
        break;
    }
}

/* -------------------------------------------------------------------------- */
/* TinyUSB descriptors                                                        */
/* -------------------------------------------------------------------------- */
#define EPNUM_MSC           1
#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

enum {
    ITF_NUM_MSC = 0,
    ITF_NUM_TOTAL
};

enum {
    EDPT_MSC_OUT  = 0x01,
    EDPT_MSC_IN   = 0x81,
};

static tusb_desc_device_t descriptor_config = {
    .bLength = sizeof(descriptor_config),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A, // Espressif VID for examples; change for products
    .idProduct = 0x4002,
    .bcdDevice = 0x100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01
};

static uint8_t const msc_fs_configuration_desc[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    // Interface number, string index, EP Out & EP In address, EP size
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EDPT_MSC_OUT, EDPT_MSC_IN, 64),
};

#if (TUD_OPT_HIGH_SPEED)
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

static uint8_t const msc_hs_configuration_desc[] = {
    // Config number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    // Interface number, string index, EP Out & EP In address, EP size
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EDPT_MSC_OUT, EDPT_MSC_IN, 512),
};
#endif // TUD_OPT_HIGH_SPEED

static char const *string_desc_arr[] = {
    (const char[]) { 0x09, 0x04 },  // 0: English (0x0409)
    "TinyUSB",                      // 1: Manufacturer
    "TinyUSB MSC Device",           // 2: Product
    "123456",                       // 3: Serials
    "Example MSC",                  // 4: MSC
};

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */
static esp_err_t init_tinyusb(void)
{
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(tusb_evt_handler);

    tusb_cfg.descriptor.device = &descriptor_config;
    tusb_cfg.descriptor.full_speed_config = msc_fs_configuration_desc;
    tusb_cfg.descriptor.string = string_desc_arr;
    tusb_cfg.descriptor.string_count = sizeof(string_desc_arr) / sizeof(string_desc_arr[0]);
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.high_speed_config = msc_hs_configuration_desc;
    tusb_cfg.descriptor.qualifier = &device_qualifier;
#endif // TUD_OPT_HIGH_SPEED

    return tinyusb_driver_install(&tusb_cfg);
}

static esp_err_t init_msc_storage(wl_handle_t *wl_handle, tinyusb_msc_storage_handle_t *storage_hdl)
{
    const esp_partition_t *data_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, NULL);
    ESP_RETURN_ON_FALSE(data_partition, ESP_ERR_NOT_FOUND, TAG, "No FATFS partition found");

    ESP_RETURN_ON_ERROR(wl_mount(data_partition, wl_handle), TAG, "wl_mount failed");

    tinyusb_msc_storage_config_t storage_cfg = {
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB, // start exposed to host
        .fat_fs = {
            .base_path = NULL,    // default base path (/data)
            .config.max_files = 5,
            .format_flags = 0,
        },
        .medium = {
            .wl_handle = *wl_handle,
        },
    };

    return tinyusb_msc_new_storage_spiflash(&storage_cfg, storage_hdl);
}

static esp_err_t init_pd_stack(esp_typec_pd_port_handle_t *port_out, i2c_master_bus_handle_t *bus_out)
{
    ESP_RETURN_ON_ERROR(esp_typec_pd_install(NULL), TAG, "pd install failed");
    ESP_LOGI(TAG, "PD core installed");

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = 0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = 1 },
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, bus_out), TAG, "i2c bus init failed");

    i2c_device_config_t dev_cfg = {
        .device_address = FUSB_ADDR_7B,
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .scl_speed_hz = 400000,
    };
    i2c_master_dev_handle_t fusb_i2c = NULL;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(*bus_out, &dev_cfg, &fusb_i2c), TAG, "fusb add failed");

    esp_typec_pd_port_config_t port_cfg = {
        .default_power_role = ESP_TYPEC_PWR_SINK,
        .task_stack = 4096,
        .task_prio  = 5,
        .rp_current = ESP_TYPEC_RP_DEFAULT,
        .src_vbus_gpio   = SRC_VBUS_GPIO,
        .src_vbus_gpio_n = SRC_VBUS_GPIO_N,
    };

    esp_typec_pd_fusb302_config_t hw = {
        .i2c_dev  = fusb_i2c,
        .gpio_int = INT_GPIO,
    };

    ESP_RETURN_ON_ERROR(esp_typec_pd_port_create_fusb302(&port_cfg, &hw, port_out), TAG, "pd port create failed");
    ESP_LOGI(TAG, "PD port created");
    return ESP_OK;
}

static esp_err_t start_typec_manager(esp_typec_pd_port_handle_t port, esp_typec_manager_handle_t *manager_out)
{
    esp_typec_manager_config_t manager_cfg = {
        .mode          = ESP_TYPEC_MANAGER_MODE_SINK_DEVICE,
        .pd_port       = port,
        .min_rp_cur_ma = 500,    // require at least 500 mA before enumerating
    };
    ESP_RETURN_ON_ERROR(esp_typec_manager_install(&manager_cfg, manager_out), TAG, "manager install failed");
    ESP_LOGI(TAG, "Type-C manager installed");
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* Main                                                                       */
/* -------------------------------------------------------------------------- */
void app_main(void)
{
    ESP_LOGI(TAG, "Starting TinyUSB MSC + Type-C PD example");

    // 1) Bring up TinyUSB stack (manager will connect/disconnect on PD events)
    ESP_ERROR_CHECK(init_tinyusb());
    ESP_LOGI(TAG, "TinyUSB initialized");

    // 2) Initialize PD stack and Type-C manager
    esp_typec_pd_port_handle_t pd_port = NULL;
    i2c_master_bus_handle_t i2c_bus = NULL;
    ESP_ERROR_CHECK(init_pd_stack(&pd_port, &i2c_bus));

    esp_typec_manager_handle_t manager = NULL;
    ESP_ERROR_CHECK(start_typec_manager(pd_port, &manager));

    // 3) Prepare MSC storage on SPI flash (FATFS partition required)
    wl_handle_t wl_handle = WL_INVALID_HANDLE;
    tinyusb_msc_storage_handle_t storage_hdl = NULL;
    ESP_ERROR_CHECK(init_msc_storage(&wl_handle, &storage_hdl));

    // Expose storage to host; manager will connect when PD attached
    ESP_ERROR_CHECK(tinyusb_msc_set_storage_mount_point(storage_hdl, TINYUSB_MSC_STORAGE_MOUNT_USB));

    ESP_LOGI(TAG, "Setup complete. Attach USB-C power/host to enumerate MSC device.");

    // Keep task alive; PD/Type-C manager drives tud_connect/disconnect.
    while (true) {
        bool isPresent = false;
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (ESP_OK != esp_typec_manager_is_bus_present(manager, &isPresent)) {
            ESP_LOGE(TAG, "Failed to get bus present status");
        }
        if (isPresent) {
            ESP_LOGI(TAG, "Type-C cable is connected");
        }
    }
}
