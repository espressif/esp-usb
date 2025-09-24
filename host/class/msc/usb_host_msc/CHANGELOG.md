## 1.1.4

- Added public API support for formatting
- Added support for ESP32-H4

## 1.1.3

- Implemented request sense, to get sense data from USB device in case of an error
- Fixed initialization of some flash drives, which require more time to initialize (https://github.com/espressif/esp-idf/issues/14319)

## 1.1.2

- Added support for ESP32-P4
- Reverted zero-copy bulk transfers. Data are now copied to USB buffers with negligible effect on performance

## 1.1.1

- Fix `msc_host_get_device_info` for devices without Serial Number string descriptor https://github.com/espressif/esp-idf/issues/12163
- Fix regression from version 1.1.0 that files could not be opened in PSRAM https://github.com/espressif/idf-extra-components/issues/202
- Fix MSC driver event handling without background task

## 1.1.0 - yanked

- Significantly increase performance with Virtual File System by allowing longer transfers
- Optimize used heap memory by reusing the Virtual File System buffer
- Optimize CPU usage by putting the background MSC task to 'Blocked' state indefinitely when there is nothing to do
- Fix MSC commands for devices on interface numbers other than zero
- Replace unsafe debug functions for direct access of MSC sectors with private SCSI commands

## 1.0.4

- Claim support for USB composite devices

## 1.0.2

- Increase transfer timeout to 5 seconds to handle slower flash disks

## 1.0.1

- Fix compatibility with IDF v4.4

## 1.0.0

- Initial version
