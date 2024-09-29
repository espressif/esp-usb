# Changelog for USB Host UAC

## 1.2.0 2024-09-27

### Improvements:

1. Support get current volume and mute status

### Bugfixes:

1. Fixed incorrect volume conversion. Using actual device volume range.
2. Fixed concurrency issues when suspend/stop during read/write

## 1.1.0

### Improvements

- Added add `uac_host_device_open_with_vid_pid` to open connected audio devices with known VID and PID
- Print component version message `uac-host: Install Succeed, Version: 1.1.0` in `uac_host_install` function

## 1.0.0

- Initial version
