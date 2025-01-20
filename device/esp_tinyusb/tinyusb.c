/*
 * SPDX-FileCopyrightText: 2020-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_private/periph_ctrl.h"
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

        // OTG IOs config
        const usb_phy_otg_io_conf_t otg_io_conf = USB_PHY_SELF_POWERED_DEVICE(config->vbus_monitor_io);
        if (config->self_powered) {
            phy_conf.otg_io_conf = &otg_io_conf;
        }
        ESP_RETURN_ON_ERROR(usb_new_phy(&phy_conf, &phy_hdl), TAG, "Install USB PHY failed");
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
