| Supported Targets | ESP32-S2 | ESP32-S3 |
| ----------------- | -------- | -------- |

# USB: CDC Class test application

## MSC driver

Basic functionality such as MSC device install/uninstall, file operations, 
raw access to MSC device and sudden disconnect is tested.

### Hardware Required

This test requires two ESP32-S2/S3 boards with a interconnected USB peripherals,
one acting as host running MSC host driver and another MSC device driver (tinyusb).