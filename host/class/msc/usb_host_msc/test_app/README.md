| Supported Targets | ESP32-S2 | ESP32-S3 | ESP32-P4 |
| ----------------- | -------- | -------- | -------- |

# USB: MSC Class test application

## MSC driver

Basic functionality such as MSC device install/uninstall, file operations,
raw access to MSC device and sudden disconnect is tested.

### Hardware Required

This test requires two ESP32 development board with USB-OTG support. The development boards shall have interconnected USB peripherals,
one acting as host running MSC host driver and another MSC device driver (tinyusb).
