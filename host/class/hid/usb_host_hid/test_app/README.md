| Supported Targets | ESP32-S2 | ESP32-S3 | ESP32-P4 |
| ----------------- | -------- | -------- | -------- |

# USB: HID Class test application

## HID driver

Basic functionality such as HID device install/uninstall, class specific requests as well as reaction to sudden disconnection and other error states.

### Hardware Required

This test requires two ESP32 development board with USB-OTG support. The development boards shall have interconnected USB peripherals,
one acting as host running HID host driver and another HID device driver (tinyusb).
