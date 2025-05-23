| Supported Targets | ESP32-S2 | ESP32-S3 | ESP32-P4 |
| ----------------- | -------- | -------- | -------- |

# USB: UVC Class test application

## UVC driver

Target tests for USB Host UVC driver.

### Hardware Required

A USB camera and USB enabled ESP SoC is needed. Connect the camera to USB host port on ESP.
Format negotiation results are tested against values from Logitech C270 camera. These constants must be modified if different camera is used.
