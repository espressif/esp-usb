/*
 * SPDX-FileCopyrightText: 2020-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_private/usb_phy.h"
#include "tinyusb.h"
#include "tinyusb_task.h"
#include "tinyusb_vbus_monitor.h"
#include "tusb.h"

#if (CONFIG_TINYUSB_MSC_ENABLED)
#include "tinyusb_msc.h"
#include "msc_storage.h"
#endif // CONFIG_TINYUSB_MSC_ENABLED

#if (CONFIG_TINYUSB_PM)
#include "tinyusb_pm.h"
#endif // CONFIG_TINYUSB_PM

#if (CONFIG_TINYUSB_USB_OTG_WAKEUP)
#include "tinyusb_usb_wakeup.h"
#endif // CONFIG_TINYUSB_USB_OTG_WAKEUP


const static char *TAG = "TinyUSB";

/**
 * @brief TinyUSB context
 */
typedef struct {
    tinyusb_port_t port;                      /*!< USB Peripheral hardware port number. Available when hardware has several available peripherals. */
    usb_phy_handle_t phy_hdl;                 /*!< USB PHY handle */
    tinyusb_event_cb_t event_cb;              /*!< Callback function that will be called when USB events occur. */
    void *event_arg;                          /*!< Pointer to the argument passed to the callback */
    bool remote_wakeup_en;                    /*!< Remote wakeup enabled flag */
} tinyusb_ctx_t;

static tinyusb_ctx_t s_ctx; // TinyUSB context

static inline void pm_event(tinyusb_event_t *event);

// ==================================================================================
// ============================= TinyUSB Callbacks ==================================
// ==================================================================================

/**
 * @brief Callback function invoked when device is mounted (configured)
 *
 * This function is called by TinyUSB stack when:
 *
 * - SetConfiguration(n) is called by the host, where n is the configuration number and not zero.
 *
 * @note
 * For Win-based Hosts: SetConfiguration(n) request is present only with available Class in Device Descriptor.
 */
void tud_mount_cb(void)
{
#if (CONFIG_TINYUSB_MSC_ENABLED)
    msc_storage_mount_to_usb();
#endif // CONFIG_TINYUSB_MSC_ENABLED
    tinyusb_event_t event = {
        .id = TINYUSB_EVENT_ATTACHED,
        .rhport = s_ctx.port,
    };
    pm_event(&event);

    if (s_ctx.event_cb) {
        s_ctx.event_cb(&event, s_ctx.event_arg);
    }
}

/**
 * @brief Callback function invoked when device is unmounted
 *
 * This function is called by TinyUSB stack when:
 *
 * - SetConfiguration(0) is called by the host.
 * - Device is disconnected (DCD_EVENT_UNPLUGGED) from the host.
 */
void tud_umount_cb(void)
{
#if (CONFIG_TINYUSB_MSC_ENABLED)
    msc_storage_mount_to_app();
#endif // CONFIG_TINYUSB_MSC_ENABLED
    tinyusb_event_t event = {
        .id = TINYUSB_EVENT_DETACHED,
        .rhport = s_ctx.port,
    };
    pm_event(&event);

    if (s_ctx.event_cb) {
        s_ctx.event_cb(&event, s_ctx.event_arg);
    }
}

#ifdef CONFIG_TINYUSB_SUSPEND_CALLBACK
/**
 * @brief Callback function invoked when device is suspended
 *
 * This function is called by TinyUSB stack when:
 *
 * - Host suspends the root port
 *
 * @param[in] remote_wakeup_en Remote wakeup is currently enabled/disabled on the device
 */
void tud_suspend_cb(bool remote_wakeup_en)
{
    tinyusb_event_t event = {
        .id = TINYUSB_EVENT_SUSPENDED,
        .rhport = s_ctx.port,
        .suspended = {
            .remote_wakeup = remote_wakeup_en,
        },
    };
    pm_event(&event);

    // Save the remote wakeup enabled flag
    s_ctx.remote_wakeup_en = remote_wakeup_en;

    if (s_ctx.event_cb) {
        s_ctx.event_cb(&event, s_ctx.event_arg);
    }
}
#endif // CONFIG_TINYUSB_SUSPEND_CALLBACK

#ifdef CONFIG_TINYUSB_RESUME_CALLBACK
/**
 * @brief Callback function invoked when device is resumed
 *
 * This function is called by TinyUSB stack when:
 *
 * - Host resumes the the root port
 */
void tud_resume_cb(void)
{
    tinyusb_event_t event = {
        .id = TINYUSB_EVENT_RESUMED,
        .rhport = s_ctx.port,
    };
    pm_event(&event);

    if (s_ctx.event_cb) {
        s_ctx.event_cb(&event, s_ctx.event_arg);
    }
}
#endif // CONFIG_TINYUSB_RESUME_CALLBACK

// ==================================================================================
// ============================== Power management ==================================
// ==================================================================================

#if CONFIG_TINYUSB_PM

esp_err_t tinyusb_pm_get_lock_status(bool *acquired)
{
    ESP_RETURN_ON_FALSE(acquired, ESP_ERR_INVALID_ARG, TAG, "acquired can't be NULL");
    ESP_RETURN_ON_FALSE(tud_inited(), ESP_ERR_INVALID_STATE, TAG, "TinyUSB driver is not installed");
    ESP_RETURN_ON_ERROR(tinyusb_pm_lock_get(acquired), TAG, "Error getting tinyusb's PM status");
    return ESP_OK;
}
#endif // CONFIG_TINYUSB_PM

/**
 * @brief Forward TinyUSB device events to the PM lock handler
 *
 * Dispatches attach, detach, suspend, and resume events to tinyusb power management
 * when `CONFIG_TINYUSB_PM` is enabled.
 *
 * @param[in] event TinyUSB device event
 */
static inline void pm_event(tinyusb_event_t *event)
{
#if CONFIG_TINYUSB_PM
    tinyusb_pm_on_event(event);
#else
    (void)event;
#endif // CONFIG_TINYUSB_PM
}

/**
 * @brief Install enabled TinyUSB power-management submodules
 *
 * Initializes USB Device light-sleep wakeup (`CONFIG_TINYUSB_USB_OTG_WAKEUP`) and/or
 * PM lock management (`CONFIG_TINYUSB_PM`) according to Kconfig.
 *
 * @param[in] config Driver configuration passed to `tinyusb_driver_install()`
 *
 * @return
 *      - ESP_OK on success or when no PM submodule is enabled in Kconfig
 *      - Other error codes from USB wakeup or PM lock initialization
 */
static esp_err_t tinyusb_power_management_install(const tinyusb_config_t *config)
{
#if CONFIG_TINYUSB_USB_OTG_WAKEUP
    ESP_RETURN_ON_ERROR(tinyusb_usb_wakeup_init(config->port), TAG, "USB wakeup initialization failed");
#endif // CONFIG_TINYUSB_USB_OTG_WAKEUP
#if CONFIG_TINYUSB_PM
    const tinyusb_pm_config_t pm_cfg = {
        .lock_enable = config->pm_lock_enable,
    };
    esp_err_t err = tinyusb_pm_init(&pm_cfg);
    if (err != ESP_OK) {
#if CONFIG_TINYUSB_USB_OTG_WAKEUP
        ESP_ERROR_CHECK(tinyusb_usb_wakeup_deinit());
#endif // CONFIG_TINYUSB_USB_OTG_WAKEUP
        ESP_LOGE(TAG, "TinyUSB PM initialization failed");
        return err;
    }
#else
    if (config->pm_lock_enable) {
        ESP_LOGW(TAG, "Tinyusb Power management is allowed in the configuration, but disabled in menuconfig");
    }
#endif // CONFIG_TINYUSB_PM
    return ESP_OK;
}

/**
 * @brief Uninstall enabled TinyUSB power-management submodules
 *
 * USB wakeup is torn down before PM lock deletion when both are enabled so light-sleep
 * event callbacks cannot run while the PM lock is being released or deleted.
 *
 * @return
 *      - ESP_OK on success or when no PM submodule is enabled in Kconfig
 *      - Other error codes from PM lock or USB wakeup deinitialization
 */
static esp_err_t tinyusb_power_management_uninstall(void)
{
#if CONFIG_TINYUSB_USB_OTG_WAKEUP
    ESP_ERROR_CHECK(tinyusb_usb_wakeup_deinit());
#endif // CONFIG_TINYUSB_USB_OTG_WAKEUP
#if CONFIG_TINYUSB_PM
    ESP_ERROR_CHECK(tinyusb_pm_deinit());
#endif // CONFIG_TINYUSB_PM

    return ESP_OK;
}


// ==================================================================================
// ============================= ESP TinyUSB Driver =================================
// ==================================================================================

/**
 * @brief Validate the TinyUSB driver configuration before installation
 *
 * @param[in] config Driver configuration to validate
 *
 * @return
 *      - ESP_OK if the configuration is valid
 *      - ESP_ERR_INVALID_ARG if the input argument is NULL, the port is out of range, or the
 *        selected port is not supported on the current target
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

    esp_err_t ret = ESP_OK;
    usb_phy_handle_t phy_hdl = NULL;
    if (!config->phy.skip_setup) {
        // Configure USB PHY
        usb_phy_config_t phy_conf = {
            .controller = USB_PHY_CTRL_OTG,
            .target = USB_PHY_TARGET_INT,
            .otg_mode = USB_OTG_MODE_DEVICE,
            .otg_speed = USB_PHY_SPEED_FULL,
        };

#if CONFIG_IDF_TARGET_ESP32S31
        // ESP32-S31 has only UTMI (HS) PHY, no internal FSLS PHY
        phy_conf.target = USB_PHY_TARGET_UTMI;
        phy_conf.otg_speed = USB_PHY_SPEED_HIGH;
#elif (SOC_USB_OTG_PERIPH_NUM > 1)
        if (config->port == TINYUSB_PORT_HIGH_SPEED_0) {
            // Default PHY for OTG2.0 is UTMI
            phy_conf.target = USB_PHY_TARGET_UTMI;
            phy_conf.otg_speed = USB_PHY_SPEED_HIGH;
        }
#endif // CONFIG_IDF_TARGET_ESP32S31

        // OTG IOs config
        const usb_phy_otg_io_conf_t otg_io_conf = USB_PHY_SELF_POWERED_DEVICE(config->phy.vbus_monitor_io);
        if (config->phy.self_powered) {
            phy_conf.otg_io_conf = &otg_io_conf;
        }
        ESP_RETURN_ON_ERROR(usb_new_phy(&phy_conf, &phy_hdl), TAG, "Install USB PHY failed");
    }
    // Init TinyUSB stack in task
    ESP_GOTO_ON_ERROR(tinyusb_task_start(config->port, &config->task, &config->descriptor), del_phy, TAG, "Init TinyUSB task failed");
    // Install PM after PHY and task are up
    ESP_GOTO_ON_ERROR(tinyusb_power_management_install(config), del_task, TAG, "TinyUSB power management install failed");

#if (CONFIG_IDF_TARGET_ESP32P4) || (CONFIG_IDF_TARGET_ESP32S31)
    // Due to hardware limitations, VBUS cannot be monitored automatically by the High-Speed USB-OTG peripheral,
    // so we need to initialize VBUS GPIO monitoring manually.

    // Initialize VBUS monitoring only for High-Speed ports and self-powered devices
#if (CONFIG_IDF_TARGET_ESP32P4)
    if (config->port == TINYUSB_PORT_HIGH_SPEED_0 && config->phy.self_powered) {
#else
    if (config->phy.self_powered) {
#endif
        const tinyusb_vbus_monitor_config_t vbus_cfg = {
            .gpio_num = config->phy.vbus_monitor_io,
            .port = (int)config->port,
        };
        ret = tinyusb_vbus_monitor_init(&vbus_cfg);
        if (ret != ESP_OK) {
            tinyusb_power_management_uninstall();
            tinyusb_task_stop();
            goto del_phy;
        }
    }
#endif // CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31

    s_ctx.port = config->port;              // Save the port number
    s_ctx.phy_hdl = phy_hdl;                // Save the PHY handle for uninstallation
    s_ctx.event_cb = config->event_cb;      // Save the event callback
    s_ctx.event_arg = config->event_arg;    // Save the event callback argument
    s_ctx.remote_wakeup_en = false;         // Remote wakeup is disabled by default

    ESP_LOGI(TAG, "TinyUSB Driver installed on port %d", config->port);
    return ESP_OK;

del_task:
    tinyusb_task_stop();
del_phy:
    if (!config->phy.skip_setup && phy_hdl) {
        usb_del_phy(phy_hdl);
    }
    return ret;
}

esp_err_t tinyusb_driver_uninstall(void)
{
    ESP_RETURN_ON_ERROR(tinyusb_task_stop(), TAG, "Deinit TinyUSB task failed");
    ESP_RETURN_ON_ERROR(tinyusb_power_management_uninstall(), TAG, "Deinit TinyUSB power management failed");
    if (s_ctx.phy_hdl) {
        ESP_RETURN_ON_ERROR(usb_del_phy(s_ctx.phy_hdl), TAG, "Unable to delete PHY");
        s_ctx.phy_hdl = NULL;
    }
#if (CONFIG_IDF_TARGET_ESP32P4)
    if (s_ctx.port == TINYUSB_PORT_HIGH_SPEED_0) {
        tinyusb_vbus_monitor_deinit();
    }
#elif (CONFIG_IDF_TARGET_ESP32S31)
    tinyusb_vbus_monitor_deinit();
#endif // CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31
    return ESP_OK;
}

esp_err_t tinyusb_remote_wakeup(void)
{
    ESP_RETURN_ON_FALSE(tud_inited(), ESP_ERR_INVALID_STATE, TAG, "TinyUSB driver is not installed");
    ESP_RETURN_ON_FALSE(tud_suspended(), ESP_ERR_NOT_ALLOWED, TAG, "USB device is not suspended");
    // Check if the remote wakeup flag was set by the esp_tinyusb's suspend callback
    // In case of user-defined suspend callback, user manages remote wakeup capability on it's own
#ifdef CONFIG_TINYUSB_SUSPEND_CALLBACK
    ESP_RETURN_ON_FALSE(s_ctx.remote_wakeup_en, ESP_ERR_INVALID_STATE, TAG, "Remote wakeup is not enabled by the host");
#endif // CONFIG_TINYUSB_SUSPEND_CALLBACK

#ifdef CONFIG_TINYUSB_PM
    ESP_RETURN_ON_ERROR(tinyusb_pm_remote_wake(), TAG, "Remote wakeup request to tinyusb PM failed");
#endif // CONFIG_TINYUSB_PM
    ESP_RETURN_ON_FALSE(tud_remote_wakeup(), ESP_FAIL, TAG, "Remote wakeup request failed");

#ifdef CONFIG_TINYUSB_SUSPEND_CALLBACK
    s_ctx.remote_wakeup_en = false; // Remote wakeup can be used only once, disable it until next suspend
#endif // CONFIG_TINYUSB_SUSPEND_CALLBACK

    return ESP_OK;
}
