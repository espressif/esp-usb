# Changelog for FTDI UART-USB converters driver

All notable changes to this component will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [2.1.0] - 2026-01-26

### Added
- Added CTS state to `CDC_ACM_HOST_SERIAL_STATE` event (https://github.com/espressif/esp-usb/issues/360)
- Added PID autodetection
- Added C API

### Fixed

- Fixed FTDI VCP SerialState parsing (correct 1-bit flags, avoid short-packet reads)
- Fixed data receiving with configured RX FIFO greater than IN endpoint's MPS

## [2.0.0] - 2023-03-15

### Changed

- Update to [CDC-ACM driver](https://components.espressif.com/components/espressif/usb_host_cdc_acm) to v2

## [1.0.1] - 2023-02-07

### Fixed

- Fix GCC-12 compile errors

## [1.0.0] - 2022-11-30

- Initial version
