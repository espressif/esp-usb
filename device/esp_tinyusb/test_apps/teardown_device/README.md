| Supported Targets | ESP32-H4 | ESP32-P4 | ESP32-S2 | ESP32-S3 | ESP32-S31 |
| ----------------- | -------- | -------- | -------- | -------- | --------- |

# Espressif's Additions to TinyUSB - Teardown Test Application

This directory contains Unity tests that validate Espressif-specific integration of TinyUSB.

The tests focus on:

- Repeated install/uninstall cycles of the TinyUSB device driver without any USB class.
- Verifying that the device re-enumerates on the host after each teardown and reinstall.
- Host-side monitoring of USB attach and detach events (VID `0x303A`, PID `0x4002`).
