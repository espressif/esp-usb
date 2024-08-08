## 1.0.3
- Fixed a bug with interface mismatch on EP IN transfer complete while several HID devices are present.
- Fixed a bug during device freeing, while detaching one of several attached HID devices.

## 1.0.2

- Added support for ESP32-P4
- Fixed device open procedure for HID devices with multiple non-sequential interfaces.

## 1.0.1

- Fixed a bug where configuring the driver with `create_background_task = false` did not properly initialize the driver. This lead to `the hid_host_uninstall()` hang-up.
- Fixed a bug where `hid_host_uninstall()` would cause a crash during the call while USB device has not been removed.
- Added `hid_host_get_device_info()` to get the basic information of a connected USB HID device.

## 1.0.0

- Initial version



