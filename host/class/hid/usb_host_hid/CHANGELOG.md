# Changelog for USB Host HID driver

All notable changes to this component will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed

- Fixed disconnect cleanup chain aborting on `usb_host_endpoint_halt` / `usb_host_endpoint_flush` / per-interface close failures, which left the USB device context dangling and unrecoverable without a physical re-plug. The disconnect cleanup path now uses dedicated best-effort helpers (`hid_host_disable_interface_disconnect`, `hid_host_device_close_disconnect`) that log and continue on those expected failures so `hid_host_uninstall_device()` is always reached. The best-effort close path also forces the interface state forward on each failure so list removal is guaranteed, and frees `iface->in_xfer` if `usb_host_interface_release()` fails partway through, preserving the leak-free invariant of the strict path. The public graceful-close API (`hid_host_device_close`, `hid_host_device_stop`) keeps the existing strict error propagation unchanged. See issue #470.

## [1.2.0] - 2026-04-08

### Added

- Added remote wakeup feature
- Added support for ESP32-S31

## [1.1.0] - 2026-01-09

### Fixed

- Fixed a vulnerability with overwrite freed heap memory during `hid_host_get_report_descriptor()`
- Fixed race condition in `hid_host_device_close()` that could lead to double-free and list corruption under concurrent close/disconnect

### Added

- Added a limitation for the HID report descriptor size to a maximum of 2048 bytes
- Added global suspend/resume support

## [1.0.4] - 2025-09-24

### Added

- Added support for ESP32-H4

## [1.0.3] - 2024-08-20

### Fixed

- Fixed a bug with interface mismatch on EP IN transfer complete while several HID devices are present
- Fixed a bug during device freeing, while detaching one of several attached HID devices

## [1.0.2] - 2024-01-25

### Added

- Added support for ESP32-P4
- Fixed device open procedure for HID devices with multiple non-sequential interfaces

## [1.0.1] - 2023-10-04

### Added

- Added `hid_host_get_device_info()` to get the basic information of a connected USB HID device

### Fixed

- Fixed a bug where configuring the driver with `create_background_task = false` did not properly initialize the driver. This lead to `the hid_host_uninstall()` hang-up
- Fixed a bug where `hid_host_uninstall()` would cause a crash during the call while USB device has not been removed

## [1.0.0] - 2023-06-22

### Added

- Initial version
