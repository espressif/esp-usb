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
//
#include "class/hid/hid_device.h"
//
#include "unity.h"
#include "device_common.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"

//
// ========================== TinyUSB HID Device Descriptors ===============================
//

#define TUSB_DESC_TOTAL_LEN      (TUD_CONFIG_DESC_LEN + CFG_TUD_HID * TUD_HID_DESC_LEN)

/**
 * @brief HID report descriptor
 *
 * In this example we implement Keyboard + Mouse HID device,
 * so we must define both report descriptors
 */
const uint8_t hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(HID_ITF_PROTOCOL_KEYBOARD)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(HID_ITF_PROTOCOL_MOUSE))
};

/**
 * @brief Configuration descriptor
 *
 * This is a simple configuration descriptor that defines 1 configuration and 1 HID interface
 */
static const uint8_t hid_configuration_descriptor[] = {
    // Configuration number, interface count, string index, total length, attribute, power in mA
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUSB_DESC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    // Interface number, string index, boot protocol, report descriptor len, EP In address, size & polling interval
    TUD_HID_DESCRIPTOR(0, 4, false, sizeof(hid_report_descriptor), 0x81, 16, 10),
};

//
// =================================== TinyUSB callbacks ===========================================
//

/**
 * @brief Get Report Descriptor callback
 *
 * - Invoked when received GET HID REPORT DESCRIPTOR request
 * - Application return pointer to descriptor, whose contents must exist long enough for transfer to complete
 */
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    // We use only one interface and one HID report descriptor, so we can ignore parameter 'instance'
    return hid_report_descriptor;
}

/**
 * @brief Get Report callback
 *
 * - Invoked when received GET_REPORT control request
 * - Application must fill buffer report's content and return its length.
 * - Return zero will cause the stack to STALL request
 */
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen)
{
    (void) instance;
    (void) report_id;
    (void) report_type;
    (void) buffer;
    (void) reqlen;

    return 0;
}

/**
 * @brief Set Report callback
 *
 * - Invoked when received SET_REPORT control request or received data on OUT endpoint ( Report ID = 0, Type = 0 )
 */
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize)
{
    (void) instance;
    (void) report_id;
    (void) report_type;
    (void) buffer;
    (void) bufsize;

}

/**
 * @brief Test the remote wakeup functionality of the USB HID device.
 *
 * This test function performs the following sequence:
 * 1. Waits for the device event TINYUSB_EVENT_SUSPENDED.
 * 2. Initiates remote wakeup to resume the device from suspension.
 * 3. Waits for the device event TINYUSB_EVENT_RESUMED.
 * 4. Sends a HID keyboard input report to simulate an 'A' key press.
 *
 * Expected behavior:
 * - The device should be suspended by the host.
 * - Upon remote wakeup, the device should resume operation.
 * - After resuming, the device sends a keyboard report to the host.
 *
 * This test validates that the device can correctly signal remote wakeup and resume communication with the host.
 */
void test_device_remote_wakeup(void)
{
    // Suspend logic is tested on the Host side, so here we just wait for some time to allow manual testing
    printf("Device is configured, you can now suspend it from the Host side to test remote wakeup...\n");

    // Wait device to be suspended
    test_device_wait_event(TINYUSB_EVENT_SUSPENDED);

    printf("Device suspended, initialize remote wakeup...\n");
    tud_remote_wakeup();

    // Wait device to be resumed
    test_device_wait_event(TINYUSB_EVENT_RESUMED);

    // Initiate input report to send 'A' key press
    printf("Press A\r\n");
    uint8_t keycode[6] = { HID_KEY_A };
    tud_hid_keyboard_report(HID_ITF_PROTOCOL_KEYBOARD, 0, keycode);
    vTaskDelay(pdMS_TO_TICKS(50));
    tud_hid_keyboard_report(HID_ITF_PROTOCOL_KEYBOARD, 0, NULL);
}

/**
 * @brief Test case for USB HID Remote Wakeup functionality
 *
 * Notes:
 * - This is a manual test, requiring user interaction on the host side to suspend the device.
 * - This test uses the default port (USB 1.1 when hardware FS-only, USB 2.0 HS when hardware supports HS).
 */
TEST_CASE("HID class, Remote Wakeup", "[hid]")
{
    // Install TinyUSB driver
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(test_device_event_handler);
    tusb_cfg.descriptor.full_speed_config = hid_configuration_descriptor;
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.high_speed_config = hid_configuration_descriptor;
#endif // TUD_OPT_HIGH_SPEED
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    // Wait device to be attached
    test_device_wait_event(TINYUSB_EVENT_ATTACHED);
    //
    test_device_remote_wakeup();
    // Cleanup
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
}

#if (SOC_USB_OTG_PERIPH_NUM > 1)
/**
 * @brief Test case for USB HID Remote Wakeup functionality
 *
 * Notes:
 * - This is a manual test, requiring user interaction on the host side to suspend the device.
 * - This test uses the USB 1.1 when hardware supports both FS and HS.
 */
TEST_CASE("HID class, Remote Wakeup, USB OTG 1.1", "[hid][full_speed]")
{
    // Install TinyUSB driver
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(test_device_event_handler);

    // Select USB OTG 1.1 port
    tusb_cfg.port = TINYUSB_PORT_FULL_SPEED_0;

    tusb_cfg.descriptor.full_speed_config = hid_configuration_descriptor;
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.high_speed_config = hid_configuration_descriptor;
#endif // TUD_OPT_HIGH_SPEED
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    // Wait device to be attached
    test_device_wait_event(TINYUSB_EVENT_ATTACHED);
    //
    test_device_remote_wakeup();
    // Cleanup
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
}
#endif // (SOC_USB_OTG_PERIPH_NUM > 1)

#endif // SOC_USB_OTG_SUPPORTED
