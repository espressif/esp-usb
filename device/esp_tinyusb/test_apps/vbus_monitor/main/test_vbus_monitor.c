/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "soc/soc_caps.h"

#if SOC_USB_OTG_SUPPORTED

#include <stdio.h>
#include <string.h>
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "unity.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"

// For GPIO matrix
#include "esp_rom_gpio.h"
#include "soc/gpio_sig_map.h"
// For DWC USB registers access
#include "soc/usb_dwc_struct.h"


#if (CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3)
#define USB_BVALID_PAD_IN_IDX           USB_SRP_BVALID_IN_IDX
#elif (CONFIG_IDF_TARGET_ESP32P4)
// Only for USB OTG 1.1
#define USB_BVALID_PAD_IN_IDX           USB_SRP_BVALID_PAD_IN_IDX
#else
#error "This example is not supported on the selected target"
#endif //

#define DEVICE_DETACH_TEST_ROUNDS           3
#define DEVICE_DETACH_ROUND_DELAY_MS        1000
#define VBUS_TRIGGER_GPIO_NUM               5      // GPIO to be used to software VBUS signal triggering
#define VBUS_MONITOR_GPIO_NUM               4      // GPIO connected to VBUS via resistor divider, used for VBUS monitoring

#define TEST_DEVICE_EVENT_QUEUE_LEN          4 // Length of the queue for device events
#define TEST_DEVICE_EVENT_TIMEOUT_MS          5000 // Timeout for waiting device events
// Device event queue
typedef struct {
    tinyusb_event_id_t id;  /*!< Event ID */
} test_device_event_t;

static QueueHandle_t _test_device_event_queue = NULL;

void test_device_event_queue_setup(void)
{
    _test_device_event_queue = xQueueCreate(TEST_DEVICE_EVENT_QUEUE_LEN, sizeof(test_device_event_t));
    TEST_ASSERT_NOT_NULL(_test_device_event_queue);
}

void test_device_event_queue_teardown(void)
{
    if (_test_device_event_queue) {
        vQueueDelete(_test_device_event_queue);
        _test_device_event_queue = NULL;
    }
}

/**
 * @brief TinyUSB callback for device event
 *
 * @note
 * For Linux-based Hosts: Reflects the SetConfiguration() request from the Host Driver.
 * For Win-based Hosts: SetConfiguration() request is present only with available Class in device descriptor.
 */
static void test_dconn_event_handler(tinyusb_event_t *event, void *arg)
{
    printf("\t Device event: \n");
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        printf("\t\t -> ATTACHED\n");
        break;
    case TINYUSB_EVENT_DETACHED:
        printf("\t\t <- DETACHED\n");
        break;
    default:
        break;
    }

    test_device_event_t dev_evt = {
        .id = event->id,
    };
    if (_test_device_event_queue) {
        xQueueSend(_test_device_event_queue, &dev_evt, portMAX_DELAY);
    }
}

/**
 * @brief Wait for a specific device event to be received in the event queue
 *
 * @param event_id The expected event ID to wait for
 */
static void test_device_wait_event(tinyusb_event_id_t event_id)
{
    TEST_ASSERT_NOT_NULL(_test_device_event_queue);
    // Wait for port callback to send an event message
    test_device_event_t dev_evt;
    BaseType_t ret = xQueueReceive(_test_device_event_queue, &dev_evt, pdMS_TO_TICKS(TEST_DEVICE_EVENT_TIMEOUT_MS));
    TEST_ASSERT_EQUAL_MESSAGE(pdPASS, ret, "Device event not received on time");
    // Check the contents of that event message
    TEST_ASSERT_EQUAL_MESSAGE(event_id, dev_evt.id, "Unexpected device event type received");
}

void test_vbus_monitor_control_setup(void)
{
    printf("VBUS monitor control setup:\n");
#if CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3
    // Triggering is done by signal multiplexing through connection VBUS monitor GPIO and BVALID signal
    printf("\t - might be triggered by signal multiplexing.\n");
#elif CONFIG_IDF_TARGET_ESP32P4
    // On ESP32P4 OTG signals from USB-DWC are not wired to GPIO matrix, we need to toggle the USB OTG GOTGCTL value in application
    // Note: this approach fits S2/S3 as well, but we use signal mux there as more convenient way
    printf("\t - might be triggered by manipulating the GOTGCTL register.\n");
#endif //
    // Triggering is done by external GPIO, connected to the VBUS monitor GPIO
    printf("\t - might be triggered by GPIO%d. \n", VBUS_TRIGGER_GPIO_NUM);
    printf("\t\t To use this, connect GPIO%d to VBUS monitor GPIO%d. \n", VBUS_TRIGGER_GPIO_NUM, VBUS_MONITOR_GPIO_NUM);
    // Configure the GPIO to output
    const gpio_config_t vbus_pin = {
        .pin_bit_mask = BIT64(VBUS_TRIGGER_GPIO_NUM),
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, gpio_config(&vbus_pin), "Failed to configure the GPIO for VBUS triggering");
    // Set initial level to HIGH, as we assume that the device is connected at the start
    gpio_set_level(VBUS_TRIGGER_GPIO_NUM, 1);
}

void test_vbus_monitor_control_teardown(void)
{
    // Disable the GPIO
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, gpio_reset_pin(VBUS_TRIGGER_GPIO_NUM), "Failed to reset the GPIO for VBUS triggering");
}

/**
 * @brief Control routines for VBUS monitoring
 *
 * Connect the GPIO signal 1 to Bvalid input with GPIO matrix
 */
static void test_vbus_monitor_control_signal_connect(void)
{
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT, USB_BVALID_PAD_IN_IDX, false);
}

/**
 * @brief Control routines for VBUS monitoring
 *
 * Connect the GPIO signal 0 to Bvalid input with GPIO matrix
 */
static void test_vbus_monitor_control_signal_disconnect(void)
{
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ZERO_INPUT, USB_BVALID_PAD_IN_IDX, false);
}

#if (SOC_USB_OTG_PERIPH_NUM > 1)
/**
 * @brief Control routines for VBUS monitoring
 *
 * Connect by manipulating the GOTGCTL register
 */
static void test_vbus_monitor_control_gotgctl_connect(usb_dwc_dev_t *dwc_otg)
{
    // Hardware should allow to override the BVALIDOVVAL register value
    TEST_ASSERT_EQUAL_MESSAGE(1, dwc_otg->gotgctl_reg.bvalidoven, "Bvalid overriding is not enabled");
    // Set Bvalid signal to 1
    dwc_otg->gotgctl_reg.bvalidovval = 1;

    /* TODO: Verify, why device staying connected after setting value to 0 and why signaling works differently */

    // We need to manipulate the D+ and D- lines to trigger the USB Host detection, as we
    // do not use the real VBUS value, as the device stays connected in the USB Host port

    // Enable pull-up resistor on D+ D-
    tud_connect();
}

/**
 * @brief Control routines for VBUS monitoring
 *
 * Disconnect by manipulating the GOTGCTL register
 */
static void test_vbus_monitor_control_gotgctl_disconnect(usb_dwc_dev_t *dwc_otg)
{
    // Hardware should allow to override the BVALIDOVVAL register value
    TEST_ASSERT_EQUAL_MESSAGE(1, dwc_otg->gotgctl_reg.bvalidoven, "Bvalid overriding is not enabled");
    // Set Bvalid signal to 1
    dwc_otg->gotgctl_reg.bvalidovval = 0;

    /* TODO: Verify, why device staying connected after setting value to 0 and why signaling works differently */

    // We need to manipulate the D+ and D- lines to trigger the USB Host detection, as we
    // do not use the real VBUS value, as the device stays connected in the USB Host port

    // Disable pull-up resistor on D+ D-
    tud_disconnect();
}
#endif // (SOC_USB_OTG_SUPPORTED > 1)

/**
 * @brief Control routines for VBUS monitoring
 *
 * Set the GPIO, which represent VBUS triggering to HIGH level to emulate the VBUS presence
 */
static void test_vbus_monitor_control_trigger_connect(void)
{
    // Set the GPIO to HIGH to emulate the VBUS presence
    gpio_set_level(VBUS_TRIGGER_GPIO_NUM, 1);

    // We need to manipulate the D+ and D- lines to trigger the USB Host detection, as we
    // do not use the real VBUS value, as the device stays connected in the USB Host port

    // Enable pull-up resistor on D+ D-
    tud_connect();
}

/**
 * @brief Control routines for VBUS monitoring
 *
 * Set the GPIO, which represent VBUS triggering to LOW level to emulate the VBUS absence
 */
static void test_vbus_monitor_control_trigger_disconnect(void)
{
    // Set the GPIO to LOW to emulate the VBUS absence
    gpio_set_level(VBUS_TRIGGER_GPIO_NUM, 0);

    // We need to manipulate the D+ and D- lines to trigger the USB Host detection, as we
    // do not use the real VBUS value, as the device stays connected in the USB Host port

    // Disable pull-up resistor on D+ D-
    tud_disconnect();
}

/**
 * @brief Test routine to emulate device attach/detach by manipulating the Bvalid signal directly
 *
 * @return
 *      Number of times the device was mounted
 */
static uint8_t test_vbus_emulated_via_bvalid_signal(void)
{
    unsigned int rounds = DEVICE_DETACH_TEST_ROUNDS;
    uint8_t mount_cnt = 0;

    while (rounds--) {
        // Allow some time for the device class to be recognized
        vTaskDelay(pdMS_TO_TICKS(DEVICE_DETACH_ROUND_DELAY_MS));
        // Connect Bvalid signal to LOW level (0) to emulate the detachment
        test_vbus_monitor_control_signal_disconnect();
        // Wait for the TINYUSB_EVENT_DETACHED event
        test_device_wait_event(TINYUSB_EVENT_DETACHED);
        // Allow some time for the device to be removed from the USB Host driver
        vTaskDelay(pdMS_TO_TICKS(DEVICE_DETACH_ROUND_DELAY_MS));
        // Connect Bvalid signal to HIGH level (1) to emulate the attachment
        test_vbus_monitor_control_signal_connect();
        // Wait for the TINYUSB_EVENT_ATTACHED event
        test_device_wait_event(TINYUSB_EVENT_ATTACHED);
        // Increase the mount_cnt value
        mount_cnt++;
    }
    // Allow some time for the device class to be recognized
    vTaskDelay(pdMS_TO_TICKS(DEVICE_DETACH_ROUND_DELAY_MS));
    return mount_cnt;
}

#if (SOC_USB_OTG_PERIPH_NUM > 1)
/**
 * @brief Test routine to emulate device attach/detach by toggling the GOTGCTL register Bvalid value
 *
 * @return
 *     Number of times the device was mounted
 */
static uint8_t test_vbus_emulated_via_gotgctl_bvalid(usb_dwc_dev_t *dwc_otg)
{
    unsigned int rounds = DEVICE_DETACH_TEST_ROUNDS;
    uint8_t mount_cnt = 0;

    while (rounds--) {
        // Allow some time for the device class to be recognized
        vTaskDelay(pdMS_TO_TICKS(DEVICE_DETACH_ROUND_DELAY_MS));
        // Emulate the detachment by setting the VBUS to Lo state
        test_vbus_monitor_control_gotgctl_disconnect(dwc_otg);
        // Wait for the TINYUSB_EVENT_DETACHED event
        test_device_wait_event(TINYUSB_EVENT_DETACHED);
        // Allow some time for the device to be removed from the USB Host driver
        vTaskDelay(pdMS_TO_TICKS(DEVICE_DETACH_ROUND_DELAY_MS));
        // Emulate the attachment by setting the VBUS to Hi state
        test_vbus_monitor_control_gotgctl_connect(dwc_otg);
        // Wait for the TINYUSB_EVENT_ATTACHED event
        test_device_wait_event(TINYUSB_EVENT_ATTACHED);
        // Increase the dev_mounted value
        mount_cnt++;
    }
    // Allow some time for the device class to be recognized
    vTaskDelay(pdMS_TO_TICKS(DEVICE_DETACH_ROUND_DELAY_MS));
    return mount_cnt;
}
#endif // (SOC_USB_OTG_SUPPORTED > 1)

/**
 * @brief Test routine to emulate device attach/detach by toggling the VBUS monitoring GPIO
 *
 * Note:
 * To run this test, connect VBUS monitor GPIO (VBUS_MONITOR_GPIO_NUM) to GPIO (VBUS_TRIGGER_GPIO_NUM)
 *
 * @return
 *      Number of times the device was mounted
 */
static uint8_t test_vbus_controlled_by_gpio(void)
{
    unsigned int rounds = DEVICE_DETACH_TEST_ROUNDS;
    uint8_t mount_cnt = 0;

    while (rounds--) {
        // Allow some time for the device class to be recognized
        vTaskDelay(pdMS_TO_TICKS(DEVICE_DETACH_ROUND_DELAY_MS));
        // Emulate the detachment by setting the VBUS to Lo state
        test_vbus_monitor_control_trigger_disconnect();
        // Wait for the TINYUSB_EVENT_DETACHED event
        test_device_wait_event(TINYUSB_EVENT_DETACHED);
        // Allow some time for the device to be removed from the USB Host driver
        vTaskDelay(pdMS_TO_TICKS(DEVICE_DETACH_ROUND_DELAY_MS));
        // Emulate the attachment by setting the VBUS to Hi state
        test_vbus_monitor_control_trigger_connect();
        // Wait for the TINYUSB_EVENT_ATTACHED event
        test_device_wait_event(TINYUSB_EVENT_ATTACHED);
        // Increase the dev_mounted value
        mount_cnt++;
    }
    // Allow some time for the device class to be recognized
    vTaskDelay(pdMS_TO_TICKS(DEVICE_DETACH_ROUND_DELAY_MS));
    return mount_cnt;
}

/**
 * @brief Test routine to emulate device attach/detach by manually connecting/disconnecting the device
 *
 * Note: this test requires external hardware (resistor divider or similar) to connect VBUS to the VBUS monitor GPIO
 * and an operator, who will manually connect/disconnect the device from the USB Host port according the prompts
 *
 * @return
 *      Number of times the device was mounted
 */
static uint8_t test_vbus_manual_attach_detach(void)
{
    unsigned int rounds = DEVICE_DETACH_TEST_ROUNDS;
    uint8_t mount_cnt = 0;

    while (rounds--) {
        printf("Detach device from the Host port ...\n");
        // Allow some time for the device class to be recognized
        vTaskDelay(pdMS_TO_TICKS(DEVICE_DETACH_ROUND_DELAY_MS));
        // Wait for the TINYUSB_EVENT_DETACHED event
        test_device_wait_event(TINYUSB_EVENT_DETACHED);
        printf("OK\n");
        //
        printf("Attach device to the Host port ...\n");
        // Allow some time for the device to be removed from the USB Host driver
        vTaskDelay(pdMS_TO_TICKS(DEVICE_DETACH_ROUND_DELAY_MS));
        // Wait for the TINYUSB_EVENT_ATTACHED event
        test_device_wait_event(TINYUSB_EVENT_ATTACHED);
        printf("OK\n");
        // Increase the dev_mounted value
        mount_cnt++;
    }
    // Allow some time for the device class to be recognized
    vTaskDelay(pdMS_TO_TICKS(DEVICE_DETACH_ROUND_DELAY_MS));
    return mount_cnt;
}

#if (SOC_USB_OTG_PERIPH_NUM == 1 && (CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3))

/**
 * @brief TinyUSB Attach/Detach events test, when Bvalid value is manipulated directly
 */
TEST_CASE("Emulated VBUS, verify attach/detach events callback (via Bvalid signal)", "[ci][dconn]")
{
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(test_dconn_event_handler);
    // In this test we do not use VBUS monitoring GPIO, because we manipulate Bvalid value directly
    tusb_cfg.phy.self_powered = false;

    // Install TinyUSB driver
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_driver_install(&tusb_cfg), "Failed to install TinyUSB driver");
    test_device_wait_event(TINYUSB_EVENT_ATTACHED);

    uint8_t dev_mounted = test_vbus_emulated_via_bvalid_signal();

    // Cleanup
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_driver_uninstall(), "Failed to uninstall TinyUSB driver");
    // Verify test results
    TEST_ASSERT_EQUAL_MESSAGE(DEVICE_DETACH_TEST_ROUNDS, dev_mounted, "Mount events count mismatch with rounds number");
}

/**
 * @brief TinyUSB Attach/Detach events test, when VBUS monitoring is enabled and connected to GPIO
 */
TEST_CASE("Controlled VBUS, verify attach/detach events callback", "[dconn]")
{
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(test_dconn_event_handler);
    // In this test we use VBUS monitoring GPIO, so enable it
    tusb_cfg.phy.self_powered = true;
    tusb_cfg.phy.vbus_monitor_io = VBUS_MONITOR_GPIO_NUM;

    // Install TinyUSB driver
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_driver_install(&tusb_cfg), "Failed to install TinyUSB driver");
    test_device_wait_event(TINYUSB_EVENT_ATTACHED);

    uint8_t dev_mounted = test_vbus_controlled_by_gpio();

    // Cleanup
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_driver_uninstall(), "Failed to uninstall TinyUSB driver");
    // Verify test results
    TEST_ASSERT_EQUAL_MESSAGE(DEVICE_DETACH_TEST_ROUNDS, dev_mounted, "Mount events count mismatch with rounds number");
}

/**
 * @brief TinyUSB TinyUSB Attach/Detach events test, when VBUS monitoring is enabled and connected to real VBUS
 */
TEST_CASE("Real VBUS, verify attach/detach events callback (requires manual handling)", "[dconn]")
{
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(test_dconn_event_handler);
    // In this test we use VBUS monitoring GPIO, we need to enable it
    tusb_cfg.phy.self_powered = true;
    tusb_cfg.phy.vbus_monitor_io = VBUS_MONITOR_GPIO_NUM;

    // Install TinyUSB driver
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_driver_install(&tusb_cfg), "Failed to install TinyUSB driver");
    test_device_wait_event(TINYUSB_EVENT_ATTACHED);

    uint8_t dev_mounted = test_vbus_manual_attach_detach();

    // Cleanup
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_driver_uninstall(), "Failed to uninstall TinyUSB driver");
    // Verify test results
    TEST_ASSERT_EQUAL_MESSAGE(DEVICE_DETACH_TEST_ROUNDS, dev_mounted, "Mount events count mismatch with rounds number");
}
#endif // (SOC_USB_OTG_PERIPH_NUM == 1 && (CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3))

#if (SOC_USB_OTG_PERIPH_NUM > 1 && CONFIG_IDF_TARGET_ESP32P4)
/**
 * @brief TinyUSB Attach/Detach events test, when Bvalid value is manipulated directly
 */
TEST_CASE("Emulated VBUS USB OTG 1.1, verify attach/detach events callback (via Bvalid signal)", "[dconn]")
{
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(test_dconn_event_handler);
    // Use USB OTG 1.1
    tusb_cfg.port = TINYUSB_PORT_FULL_SPEED_0;
    // In this test we do not use VBUS monitoring GPIO, because we manipulate Bvalid value directly
    tusb_cfg.phy.self_powered = false;

    // Install TinyUSB driver
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_driver_install(&tusb_cfg), "Failed to install TinyUSB driver");
    test_device_wait_event(TINYUSB_EVENT_ATTACHED);

    uint8_t dev_mounted = test_vbus_emulated_via_bvalid_signal();

    // Cleanup
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_driver_uninstall(), "Failed to uninstall TinyUSB driver");
    // Verify test results
    TEST_ASSERT_EQUAL_MESSAGE(DEVICE_DETACH_TEST_ROUNDS, dev_mounted, "Mount events count mismatch with rounds number");
}

/**
 * @brief TinyUSB Attach/Detach events test, when Bvalid value is manipulated directly
 */
TEST_CASE("Emulated VBUS USB OTG 1.1, verify attach/detach events callback (via GOTGCTL register)", "[dconn]")
{
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(test_dconn_event_handler);
    // Use USB OTG 1.1
    tusb_cfg.port = TINYUSB_PORT_FULL_SPEED_0;
    // In this test we do not use VBUS monitoring GPIO, because we manipulate Bvalid value directly
    tusb_cfg.phy.self_powered = false;

    // Install TinyUSB driver
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_driver_install(&tusb_cfg), "Failed to install TinyUSB driver");

    // When we do not set self_powered mode, the Bvalid override should be not enabled
    TEST_ASSERT_EQUAL_MESSAGE(0, USB_DWC_FS.gotgctl_reg.bvalidoven, "Bvalid override value is already enabled");
    // Set Bvalid signal to 1 initially
    USB_DWC_FS.gotgctl_reg.bvalidovval = 1;
    // Wait 5 PHY clocks
    esp_rom_delay_us(1);
    // Enable to override the signal from PHY
    USB_DWC_FS.gotgctl_reg.bvalidoven = 1;

    test_device_wait_event(TINYUSB_EVENT_ATTACHED);

    uint8_t dev_mounted = test_vbus_emulated_via_gotgctl_bvalid(&USB_DWC_FS);

    // Cleanup
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_driver_uninstall(), "Failed to uninstall TinyUSB driver");
    // Verify test results
    TEST_ASSERT_EQUAL_MESSAGE(DEVICE_DETACH_TEST_ROUNDS, dev_mounted, "Mount events count mismatch with rounds number");
}

/**
 * @brief TinyUSB Attach/Detach events test, when Bvalid value is manipulated via GOTGCTL register
 */
TEST_CASE("Emulated VBUS USB OTG 2.0, verify attach/detach events callback (via GOTGCTL register)", "[ci][dconn]")
{
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(test_dconn_event_handler);
    // Use USB OTG 2.0
    tusb_cfg.port = TINYUSB_PORT_HIGH_SPEED_0;
    // In this test we do not use VBUS monitoring GPIO, because we manipulate Bvalid value directly
    tusb_cfg.phy.self_powered = false;

    // Install TinyUSB driver
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_driver_install(&tusb_cfg), "Failed to install TinyUSB driver");

    // When we do not set self_powered mode, the Bvalid override should be not enabled
    TEST_ASSERT_EQUAL_MESSAGE(0, USB_DWC_HS.gotgctl_reg.bvalidoven, "Bvalid override value is already enabled");
    // Set Bvalid signal to 1 initially
    USB_DWC_HS.gotgctl_reg.bvalidovval = 1;
    // Wait 5 PHY clocks
    esp_rom_delay_us(1);
    // Enable to override the signal from PHY
    USB_DWC_HS.gotgctl_reg.bvalidoven = 1;

    test_device_wait_event(TINYUSB_EVENT_ATTACHED);

    uint8_t dev_mounted = test_vbus_emulated_via_gotgctl_bvalid(&USB_DWC_HS);

    // Cleanup
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_driver_uninstall(), "Failed to uninstall TinyUSB driver");
    // Verify test results
    TEST_ASSERT_EQUAL_MESSAGE(DEVICE_DETACH_TEST_ROUNDS, dev_mounted, "Mount events count mismatch with rounds number");
}

/**
 * @brief TinyUSB Attach/Detach events test, when VBUS monitoring is enabled and connected to GPIO
 */
TEST_CASE("Controlled VBUS USB OTG 1.1, verify attach/detach events callback", "[dconn]")
{
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(test_dconn_event_handler);
    // Use USB OTG 1.1
    tusb_cfg.port = TINYUSB_PORT_FULL_SPEED_0;
    // In this test we use VBUS monitoring GPIO, so enable it
    tusb_cfg.phy.self_powered = true;
    tusb_cfg.phy.vbus_monitor_io = VBUS_MONITOR_GPIO_NUM;

    // Install TinyUSB driver
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_driver_install(&tusb_cfg), "Failed to install TinyUSB driver");
    test_device_wait_event(TINYUSB_EVENT_ATTACHED);

    uint8_t dev_mounted = test_vbus_controlled_by_gpio();

    // Cleanup
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_driver_uninstall(), "Failed to uninstall TinyUSB driver");
    // Verify test results
    TEST_ASSERT_EQUAL_MESSAGE(DEVICE_DETACH_TEST_ROUNDS, dev_mounted, "Mount events count mismatch with rounds number");
}

/**
 * @brief TinyUSB Attach/Detach events test, when VBUS monitoring is enabled and connected to GPIO
 */
TEST_CASE("Controlled VBUS USB OTG 2.0, verify attach/detach events callback", "[dconn]")
{
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(test_dconn_event_handler);
    // Use USB OTG 2.0
    tusb_cfg.port = TINYUSB_PORT_HIGH_SPEED_0;
    // In this test we use VBUS monitoring GPIO, so enable it
    tusb_cfg.phy.self_powered = true;
    tusb_cfg.phy.vbus_monitor_io = VBUS_MONITOR_GPIO_NUM;

    // Install TinyUSB driver
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_driver_install(&tusb_cfg), "Failed to install TinyUSB driver");
    test_device_wait_event(TINYUSB_EVENT_ATTACHED);

    uint8_t dev_mounted = test_vbus_controlled_by_gpio();

    // Cleanup
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_driver_uninstall(), "Failed to uninstall TinyUSB driver");
    // Verify test results
    TEST_ASSERT_EQUAL_MESSAGE(DEVICE_DETACH_TEST_ROUNDS, dev_mounted, "Mount events count mismatch with rounds number");
}

/**
 * @brief TinyUSB TinyUSB Attach/Detach events test, when VBUS monitoring is enabled and connected to real VBUS
 */
TEST_CASE("Real VBUS USB OTG 1.1, verify attach/detach events callback (requires manual handling)", "[dconn]")
{
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(test_dconn_event_handler);
    // Use USB OTG 1.1 peripheral
    tusb_cfg.port = TINYUSB_PORT_FULL_SPEED_0;
    // In this test we use VBUS monitoring GPIO, so enable it
    tusb_cfg.phy.self_powered = true;
    tusb_cfg.phy.vbus_monitor_io = VBUS_MONITOR_GPIO_NUM;

    // Install TinyUSB driver
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_driver_install(&tusb_cfg), "Failed to install TinyUSB driver");
    test_device_wait_event(TINYUSB_EVENT_ATTACHED);

    uint8_t dev_mounted = test_vbus_manual_attach_detach();

    // Cleanup
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_driver_uninstall(), "Failed to uninstall TinyUSB driver");
    // Verify test results
    TEST_ASSERT_EQUAL_MESSAGE(DEVICE_DETACH_TEST_ROUNDS, dev_mounted, "Mount events count mismatch with rounds number");
}

/**
 * @brief TinyUSB TinyUSB Attach/Detach events test, when VBUS monitoring is enabled and connected to real VBUS
 */
TEST_CASE("Real VBUS USB OTG 2.0, verify attach/detach events callback (requires manual handling)", "[dconn]")
{
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(test_dconn_event_handler);
    // Use USB OTG 2.0
    tusb_cfg.port = TINYUSB_PORT_HIGH_SPEED_0;
    // In this test we use VBUS monitoring GPIO, so enable it
    tusb_cfg.phy.self_powered = true;
    tusb_cfg.phy.vbus_monitor_io = VBUS_MONITOR_GPIO_NUM;

    // Install TinyUSB driver
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_driver_install(&tusb_cfg), "Failed to install TinyUSB driver");
    test_device_wait_event(TINYUSB_EVENT_ATTACHED);

    uint8_t dev_mounted = test_vbus_manual_attach_detach();

    // Cleanup
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_driver_uninstall(), "Failed to uninstall TinyUSB driver");
    // Verify test results
    TEST_ASSERT_EQUAL_MESSAGE(DEVICE_DETACH_TEST_ROUNDS, dev_mounted, "Mount events count mismatch with rounds number");
}

#endif // (SOC_USB_OTG_PERIPH_NUM > 1 && CONFIG_IDF_TARGET_ESP32P4)

#endif // SOC_USB_OTG_SUPPORTED
