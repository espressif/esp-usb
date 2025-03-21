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
#include "soc/usb_pins.h" // TODO: Will be removed in IDF v6.0 IDF-9029
#include "tinyusb.h"
#include "descriptors_control.h"
#include "tusb.h"

const static char *TAG = "TinyUSB";

// Tinyusb supports only one instance of the driver per peripheral
// Otherwise, the next parameters could be stored in the context
static usb_phy_handle_t phy_hdl;
static tusb_port_t rhport;
static tusb_rhport_init_t rhport_init;
static tinyusb_task_type_t task_type;

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
    ESP_RETURN_ON_FALSE(config->periph < TINYUSB_PERIPH_MAX, ESP_ERR_INVALID_ARG, TAG, "Periph number should be supported by the hardware");
    ESP_RETURN_ON_FALSE(config->speed < TINYUSB_SPEED_MAX, ESP_ERR_INVALID_ARG, TAG, "Speed should be supported by the hardware");

#if (CONFIG_IDF_TARGET_ESP32P4)
    if (config->periph == TINYUSB_PERIPH_HS) {
        ESP_RETURN_ON_FALSE(config->speed != TINYUSB_SPEED_FULL_SPEED, ESP_ERR_INVALID_ARG, TAG, "High-speed peripheral does not support Full-speed mode");
    }
#ifndef USB_PHY_SUPPORTS_P4_OTG11
    ESP_RETURN_ON_FALSE(config->periph != TINYUSB_PERIPH_FS, ESP_ERR_INVALID_ARG, TAG, "USB PHY support for OTG1.1 has not been implemented, please update your esp-idf");
#endif // ESP-IDF supports OTG1.1 peripheral
#endif // CONFIG_IDF_TARGET_ESP32P4

    if (config->task_type == TINYUSB_TASK_TYPE_INTERNAL) {
        // For internal task type, size, priority and affinity should be set correctly
        ESP_RETURN_ON_FALSE(config->task_config.size != 0, ESP_ERR_INVALID_ARG, TAG, "Size should be set for the task");
        ESP_RETURN_ON_FALSE(config->task_config.priority != 0, ESP_ERR_INVALID_ARG, TAG, "Priority should be set for the task");
        ESP_RETURN_ON_FALSE(config->task_config.affinity <= TINYUSB_TASK_AFFINITY_MAX, ESP_ERR_INVALID_ARG, TAG, "Affinity should be set for the task");
    } else {
        // For external task type, init in task is not possible
        ESP_RETURN_ON_FALSE(config->task_config.init_in_task == false, ESP_ERR_INVALID_ARG, TAG, "Init in task not possible for external task type");
    }
    return ESP_OK;
}

esp_err_t tinyusb_driver_install(const tinyusb_config_t *config)
{
    ESP_RETURN_ON_ERROR(tinyusb_check_config(config), TAG, "TinyUSB configuration check failed");

    esp_err_t ret;
    tusb_port_t tusb_port;
    // Configure params for TinyUSB stack
    // Role selection: esp_tinyusb is always a device
    rhport_init.role = TUSB_ROLE_DEVICE;
    // Periph & speed selection
    if (config->periph == TINYUSB_PERIPH_FS) {
        tusb_port = TUSB_PORT_FS;
        rhport_init.speed = TUSB_SPEED_FULL;
    } else {
        tusb_port = TUSB_PORT_HS;
        rhport_init.speed = (config->speed == TINYUSB_SPEED_FULL_SPEED) ? TUSB_SPEED_FULL : TUSB_SPEED_HIGH ;
    }

    // Configure USB PHY
    usb_phy_config_t phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .otg_mode = USB_OTG_MODE_DEVICE,
#if (USB_PHY_SUPPORTS_P4_OTG11)
        // OTG speed selects the PHY indirectly
        .otg_speed = (rhport_init.speed == TUSB_SPEED_HIGH)
        ? USB_PHY_SPEED_HIGH
        : USB_PHY_SPEED_FULL,
#endif // USB_PHY_SUPPORTS_P4_OTG11
    };

    // External PHY IOs config
    usb_phy_ext_io_conf_t ext_io_conf = {
        .vp_io_num = USBPHY_VP_NUM,
        .vm_io_num = USBPHY_VM_NUM,
        .rcv_io_num = USBPHY_RCV_NUM,
        .oen_io_num = USBPHY_OEN_NUM,
        .vpo_io_num = USBPHY_VPO_NUM,
        .vmo_io_num = USBPHY_VMO_NUM,
    };
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
    // OTG IOs config
    const usb_phy_otg_io_conf_t otg_io_conf = USB_PHY_SELF_POWERED_DEVICE(config->vbus_monitor_io);
    if (config->self_powered) {
        phy_conf.otg_io_conf = &otg_io_conf;
    }
    ESP_RETURN_ON_ERROR(usb_new_phy(&phy_conf, &phy_hdl), TAG, "Install USB PHY failed");

    // Descriptors config
    ESP_GOTO_ON_ERROR(tinyusb_set_descriptors(config), del_phy, TAG, "Descriptors config failed");

    // Init TinyUSB stack (external or not deferred to the task)
    if (config->task_type != TINYUSB_TASK_TYPE_INTERNAL || !config->task_config.init_in_task) {
        ESP_GOTO_ON_FALSE(tusb_rhport_init(tusb_port, &rhport_init), ESP_FAIL, free_desc, TAG, "Init TinyUSB stack failed");
    }

    // Run task if internal
    if (config->task_type == TINYUSB_TASK_TYPE_INTERNAL) {
        if (config->task_config.init_in_task) {
            // Save parameters in the context to init in the stack later in the task
            const tinyusb_task_init_params_t init_params = {
                .rhport = tusb_port,
                .rhport_init = &rhport_init,
            };
            ESP_GOTO_ON_ERROR(tinyusb_task_start(&config->task_config, &init_params), tusb_tear, TAG, "Init TinyUSB task failed");
        } else {
            // No need parameters in the context, init the internal task
            ESP_GOTO_ON_ERROR(tinyusb_task_start(&config->task_config, NULL), tusb_tear, TAG, "Init TinyUSB task failed");
        }
    }

    // Save the parameters in the context to uninstall driver later
    rhport = tusb_port;
    task_type = config->task_type;

    ESP_LOGI(TAG, "TinyUSB Driver installed on port %d", rhport);

    return ESP_OK;

tusb_tear:
    tusb_rhport_teardown(tusb_port);
free_desc:
    tinyusb_free_descriptors();
del_phy:
    usb_del_phy(phy_hdl);
    return ret;

}

esp_err_t tinyusb_driver_uninstall(void)
{
    if (task_type == TINYUSB_TASK_TYPE_INTERNAL) {
        ESP_RETURN_ON_ERROR(tinyusb_task_stop(), TAG, "Deinit TinyUSB task failed");
    } else {
        // NOTE: tud_task() should be stopped by the application before calling this function
    }
    ESP_RETURN_ON_FALSE(tusb_rhport_teardown(rhport), ESP_ERR_NOT_FINISHED, TAG, "Unable to teardown TinyUSB stack");
    tinyusb_free_descriptors();
    ESP_RETURN_ON_ERROR(usb_del_phy(phy_hdl), TAG, "Unable to delete PHY");
    return ESP_OK;
}
