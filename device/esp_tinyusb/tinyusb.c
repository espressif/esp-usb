/*
 * SPDX-FileCopyrightText: 2020-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_private/usb_phy.h"
#include "tinyusb.h"
#include "descriptors_control.h"
#include "tusb.h"

const static char *TAG = "TinyUSB";

// Tinyusb supports only one instance of the driver per peripheral
// Otherwise, the next parameters could be stored in the context
static usb_phy_handle_t phy_hdl;

// For the tinyusb component without tusb_teardown() implementation
#ifndef tusb_teardown
#   define tusb_teardown()   (true)
#endif // tusb_teardown

/**
 * @brief Check the TinyUSB configuration
 */
static esp_err_t tinyusb_check_config(const tinyusb_config_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "Config can't be NULL");
    ESP_RETURN_ON_FALSE(config->port < TINYUSB_PORT_MAX, ESP_ERR_INVALID_ARG, TAG, "Port number should be supported by the hardware");
#if (CONFIG_IDF_TARGET_ESP32P4)
#ifndef USB_PHY_SUPPORTS_P4_OTG11
    ESP_RETURN_ON_FALSE(config->port != TINYUSB_PORT_0, ESP_ERR_INVALID_ARG, TAG, "USB PHY support for OTG1.1 has not been implemented, please update your esp-idf");
#endif // ESP-IDF supports OTG1.1 peripheral
#endif // CONFIG_IDF_TARGET_ESP32P4
    return ESP_OK;
}

esp_err_t tinyusb_driver_install(const tinyusb_config_t *config)
{
    ESP_RETURN_ON_ERROR(tinyusb_check_config(config), TAG, "TinyUSB configuration check failed");
    ESP_RETURN_ON_ERROR(tinyusb_task_check_config(&config->task), TAG, "TinyUSB task configuration check failed");

    esp_err_t ret;

    if (!config->skip_phy_setup) {
        // Configure USB PHY
        usb_phy_config_t phy_conf = {
            .controller = USB_PHY_CTRL_OTG,
            .target = USB_PHY_TARGET_INT,
            .otg_mode = USB_OTG_MODE_DEVICE,
            // OTG speed selects the PHY indirectly
            .otg_speed = (config->port == TINYUSB_PORT_0) ? USB_PHY_SPEED_FULL : USB_PHY_SPEED_HIGH,
        };

    /*
    Following ext. PHY IO configuration is here to provide compatibility with IDFv5.x releases,
    where ext. PHY IOs were mapped to predefined GPIOs.
    In reality, ESP32-S2 and ESP32-S3 can map ext. PHY IOs to any GPIOs.
    This option is implemented in esp_tinyusb v2.0.0 and later.
    */
    usb_phy_ext_io_conf_t ext_io_conf;
    // Use memset to be compatible with IDF < 5.4.1 where suspend_n_io_num and fs_edge_sel_io_num were added
    memset(&ext_io_conf, -1, sizeof(usb_phy_ext_io_conf_t));
#if CONFIG_IDF_TARGET_ESP32S2
    ext_io_conf.vp_io_num  = 33;
    ext_io_conf.vm_io_num  = 34;
    ext_io_conf.rcv_io_num = 35;
    ext_io_conf.oen_io_num = 36;
    ext_io_conf.vpo_io_num = 37;
    ext_io_conf.vmo_io_num = 38;
#elif CONFIG_IDF_TARGET_ESP32S3
    ext_io_conf.vp_io_num  = 42;
    ext_io_conf.vm_io_num  = 41;
    ext_io_conf.rcv_io_num = 21;
    ext_io_conf.oen_io_num = 40;
    ext_io_conf.vpo_io_num = 39;
    ext_io_conf.vmo_io_num = 38;
#endif // IDF_TARGET_ESP32S3

    if (config->external_phy) {
        phy_conf.target = USB_PHY_TARGET_EXT;
        phy_conf.ext_io_conf = &ext_io_conf;

        /*
        There is a bug in esp-idf that does not allow device speed selection
        when External PHY is used.
        Remove this when proper fix is implemented in IDF-11144
        */
        phy_conf.otg_speed = USB_PHY_SPEED_UNDEFINED;
    } else {
        phy_conf.target = USB_PHY_TARGET_INT;
    }

    // Descriptors config
    ESP_GOTO_ON_ERROR(tinyusb_set_descriptors(config), del_phy, TAG, "Descriptors config failed");
    // Init TinyUSB stack in task
    ESP_GOTO_ON_ERROR(tinyusb_task_start(config->port, &config->task), free_desc, TAG, "Init TinyUSB task failed");

    ESP_LOGI(TAG, "TinyUSB Driver installed on port %d", config->port);
    return ESP_OK;

free_desc:
    tinyusb_free_descriptors();
del_phy:
    if (!config->skip_phy_setup) {
        usb_del_phy(phy_hdl);
    }
    return ret;
}

esp_err_t tinyusb_driver_uninstall(void)
{
    ESP_RETURN_ON_ERROR(tinyusb_task_stop(), TAG, "Deinit TinyUSB task failed");
    tinyusb_free_descriptors();
    ESP_RETURN_ON_ERROR(usb_del_phy(phy_hdl), TAG, "Unable to delete PHY");
    return ESP_OK;
}
