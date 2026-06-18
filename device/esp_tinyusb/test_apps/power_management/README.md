| Supported Targets | ESP32-H4 | ESP32-P4 | ESP32-S2 | ESP32-S3 |
| ----------------- | -------- | -------- | -------- | -------- |

# Espressif's Additions to TinyUSB - Power management Test Application

This directory contains Unity tests that validate Espressif-specific integration of TinyUSB.

The tests focus on:

- Power management of the USB Device
- Testing tinyusb suspend/resume callbacks delivery
- Testing remote wakeup signalizing by the device
- Calling esp_tinyusb public API when the USB Device is in suspended state
- Init/Deinit of esp_tinyusb driver with device in suspended state

## Running the tests on a host PC

These pytest cases rely on the Linux kernel **USB automatic suspend** feature. After a short period of USB inactivity (about 2–3 seconds), the host suspends the device. That suspend/resume cycle is what triggers the TinyUSB power-management events validated by the tests.

### Linux (Ubuntu)

The tests are verified on **Ubuntu** (CI runners are using Ubuntu). The USB `power/control` could be `auto` or `on`, depending on the linux distribution build.

If automatic suspend is disabled on your Linux host (for example on Raspberry Pi CI runners, where `power/control` defaults to `on`), enable it with a UDEV rule as described in [Set power management of the USB device](../README.md#set-power-management-of-the-usb-device) in the parent README.

Additional Linux setup:

- User needs to [set permissions](../README.md#set-root-permissions-for-low-level-access-to-usb-devices) to the USB device to successfully run the test app on a Linux host PC
- The `pyusb` Python package must be installed manually before running pytest (`pip install pyusb`)

### Windows, macOS, and other non-Linux hosts

These pytest tests were **not tested** with Windows, macOS, or other non-Linux host PCs. They depend on Linux kernel USB power management (automatic suspend/resume) and on Linux CDC-ACM driver behavior used by the test scripts.
