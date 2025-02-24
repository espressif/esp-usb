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
#include "tusb_tasks.h"

const static char *TAG = "TinyUSB";
static usb_phy_handle_t phy_hdl;

// For the tinyusb component without tusb_teardown() implementation
#ifndef tusb_teardown
#   define tusb_teardown()   (true)
#endif // tusb_teardown

esp_err_t tinyusb_driver_install(const tinyusb_config_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "Config can't be NULL");

    // Configure USB PHY
    usb_phy_config_t phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .otg_mode = USB_OTG_MODE_DEVICE,
#if (USB_PHY_SUPPORTS_P4_OTG11)
        .otg_speed = (TUD_OPT_HIGH_SPEED) ? USB_PHY_SPEED_HIGH : USB_PHY_SPEED_FULL,
#else
#if (CONFIG_IDF_TARGET_ESP32P4 && CONFIG_TINYUSB_RHPORT_FS)
#error "USB PHY for OTG1.1 is not supported, please update your esp-idf."
#endif // IDF_TARGET_ESP32P4 && CONFIG_TINYUSB_RHPORT_FS
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
    ESP_RETURN_ON_ERROR(tinyusb_set_descriptors(config), TAG, "Descriptors config failed");

    // Init
#if !CONFIG_TINYUSB_INIT_IN_DEFAULT_TASK
    ESP_RETURN_ON_FALSE(tusb_init(), ESP_FAIL, TAG, "Init TinyUSB stack failed");
#endif
#if !CONFIG_TINYUSB_NO_DEFAULT_TASK
    ESP_RETURN_ON_ERROR(tusb_run_task(), TAG, "Run TinyUSB task failed");
#endif
    ESP_LOGI(TAG, "TinyUSB Driver installed");
    return ESP_OK;
}

esp_err_t tinyusb_driver_uninstall(void)
{
#if !CONFIG_TINYUSB_NO_DEFAULT_TASK
    ESP_RETURN_ON_ERROR(tusb_stop_task(), TAG, "Unable to stop TinyUSB task");
#endif // !CONFIG_TINYUSB_NO_DEFAULT_TASK
    ESP_RETURN_ON_FALSE(tusb_teardown(), ESP_ERR_NOT_FINISHED, TAG, "Unable to teardown TinyUSB");
    tinyusb_free_descriptors();
    ESP_RETURN_ON_ERROR(usb_del_phy(phy_hdl), TAG, "Unable to delete PHY");
    return ESP_OK;
}
