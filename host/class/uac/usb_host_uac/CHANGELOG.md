# Changelog for USB Host UAC

All notable changes to this component will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.5.0] - 2026-05-14

### Breaking Changes

- `uac_host_dev_alt_param_t`: added `uint8_t subframe_size` field between `channels` and `bit_resolution`, reflecting the UAC Type I `bSubframeSize` descriptor field. This changes the struct layout; any code that initializes the struct with positional initializers or compares it with `memcmp` must be updated.

### Fixed

- Fixed TX isochronous packet size calculation for non-integer sample rates (e.g. 44100 Hz). The driver previously rounded up the packet size unconditionally, causing audio drift. A fractional accumulator now distributes the extra byte across packets to maintain correct throughput.
- Fixed TX packet size calculation for PCM formats whose audio subframe size differs from the bit resolution byte width (for example, 24-bit samples in 4-byte subframes). The driver now uses `bSubframeSize` for USB packet sizing instead of `bit_resolution / 8`.
- Fixed the audio_player example siren generation for higher sample rates such as 48 kHz. The example now adapts the tone-switching period when the fixed PCM buffer is shorter than the original siren cycle, ensuring the generated buffer contains both high and low tones.

### Changed

- `uac_host_printf_device_param` now prints `subframe_size` for each alternate setting.
- audio_player example: added `CONFIG_EXAMPLE_SPK_SAMPLE_FREQ` and `CONFIG_EXAMPLE_MIC_SAMPLE_FREQ` Kconfig options (default 0 = auto) to select the target sample frequency; falls back to the device default when the requested frequency is not supported.
- audio_player example: uses `subframe_size` from the UAC alternate setting when generating PCM data and sizing microphone recording buffers, improving compatibility with non-16-bit PCM layouts.

## [1.4.0] - 2026-05-06

### Added

- Added support for ESP32-S31

### Fixed

- Allow driver background task to have no core affinity
- Fixed build with PSRAM enabled

## [1.3.3] - 2025-11-27

### Changed

- Fixed a playback stuttering issue when bInterval was not equal to 1 in the full-speed device audio endpoint descriptor, and forced bInterval = 1 to make it compatible with these non-standard devices.

## [1.3.2] - 2025-10-21

### Changed

- uac_host_device_read() now waits for the full timeout duration until the required amount of data is read, preventing premature returns with partial data (https://github.com/espressif/esp-usb/issues/248)

## [1.3.1] - 2025-09-24

- Added support for ESP32-H4

## [1.3.0] - 2025-03-28

- Added Linux target build for the UAC component, host tests (https://github.com/espressif/esp-usb/issues/143)

## [1.2.0] - 2024-10-10

### Breaking Changes:

- Changed the parameter type of `uac_host_device_set_volume_db` from uint32_t to int16_t

### Improvements:

- Support get current volume and mute status

### Bugfixes:

- Fixed incorrect volume conversion. Using actual device volume range.
- Fixed concurrency issues when suspend/stop during read/write

## [1.1.0] - 2024-07-08

### Improvements

- Added add `uac_host_device_open_with_vid_pid` to open connected audio devices with known VID and PID
- Print component version message `uac-host: Install Succeed, Version: 1.1.0` in `uac_host_install` function

## [1.0.0] - 2024-05-23

- Initial version
