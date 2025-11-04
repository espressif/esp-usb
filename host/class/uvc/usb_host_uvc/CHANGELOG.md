# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [2.4.1] - 2025-11-27

### Added

- Added SOI check to prevent output of corrupted MJPEG frames
- Added UVC payload header check

### Fixed

- Moved `usb_types_uvc.h` to `private_include` directory
- Fixed bulk transfer EOF handling to improve robustness
- Fixed parsing of non-conforming UVC VideoStreaming descriptors where `bNumFormats` is inconsistent with actual `bFormatIndex` values (e.g., `bNumFormats=1` but format descriptors use `bFormatIndex=2`)
- Fixed potential `ESP_ERR_INVALID_STATE` error when uninstalling UVC driver by always waiting for teardown completion regardless of driver state

## [2.4.0] - 2025-11-21

### Added

- Added support for user-provided frame buffers via `user_frame_buffers` field in `uvc_host_stream_config_t.advanced`
- Added global suspend/resume support

### Fixed

- Fixed potential buffer overflow in descriptor printing functions by removing unnecessary memcpy operations
- Removed unaligned access workaround; ESP targets support unaligned memory access natively
- Fixed assertion failure when receiving packets with unexpected `frame_id`

## [2.3.1] - 2025-09-24

### Added

- Added support for ESP32-H4

## [2.3.0] - 2025-05-27

### Added

- Added `uvc_host_stream_format_get()` function that returns current stream's format
- Added `uvc_host_buf_info_get()` function for esp_video binding
- Added option to request default FPS by setting FPS = 0

### Fixed

- Fixed UVC driver re-installation
- Fixed abort on unexpected EoF flag in bulk transfers

## [2.2.0] - 2025-04-06

### Added

- Added `uvc_host_stream_format_select()` function that allows change of format of an opened stream

### Fixed

- Fixed MPS limitation on FS targets from 600 to 596 bytes
- Fixed device opening procedure. Now the frame format is committed when the stream is started

## [2.1.0] - 2025-02-14

### Added

- Added support for get frame list when device is inserted
- Added support for multiple cameras with USB hub

## [2.0.1] - 2025-02-04

### Fixed

- Fixed negotiation for some non-conforming UVC devices (https://github.com/espressif/esp-idf/issues/9868)

## [2.0.0] - 2025-12-19

### Changed

- New version of the driver, native to Espressif's USB Host Library
- Removed libuvc dependency

## [1.0.4] - 2024-05-09

### Added

- Support printf frame based descriptor

## [1.0.3] - 2024-03-11

### Added

- Added support for ESP32-P4
- Added `libuvc_get_usb_device_info` function

### Changed

- Bumped libuvc version to relax frame format negotiation

### Fixed

- Fixed crash on opening non-UVC devices

## [1.0.2] - 2023-09-27

### Changed

- Updated libuvc library to 0.0.7 https://github.com/libuvc/libuvc/tree/v0.0.7
- Added Software BoM information

## [1.0.1] - 2023-04-19

### Fixed

- Fixed compatibility with IDF v4.4

## [1.0.0] - 2022-08-22

### Added

- Initial version
