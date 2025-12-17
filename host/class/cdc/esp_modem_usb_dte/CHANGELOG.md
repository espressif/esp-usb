# Changelog for esp_modem USB DTE plugin

All notable changes to this component will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.3.0] - 2025-12-19

### Added

- Added support for esp_modem v2

### Deprecated

- Deprecated esp_modem_usb_term_config->cdc_compliant field. CDC compliance is now auto detected by CDC-ACM USB driver.

### Known issues

- ESP32-P4 cannot receive fragmented AT responses from a modem. Thus, some SimCom modems do not work with this version

## [1.2.1] - 2024-06-24

### Added

- Added support to transmit larger payloads than the buffer_size of DTE

### Known issues
- ESP32-P4 cannot receive fragmented AT responses from a modem. Thus, some SimCom modems do not work with this version

## [1.2.0] - 2024-06-17

### Fixed

- Fixed C++ build error for `usb_host_config_t` backward compatibility

### Added

- Added default configuration for SimCom SIM7070G, SIM7080 and SIMA7672E modems
- Added default configuration for Quectel EC20
- Added ESP32-P4 support

### Known issues

- ESP32-P4 cannot receive fragmented AT responses from a modem. Thus, some SimCom modems do not work with this version

## [1.1.0] - 2023-03-15

### Fixed

- Fixed USB receive path bug, where received data could be overwritten by new data

### Added

- Added default configurations for tested modems
- Added initial support for modems with two AT ports

### Changed
- Updated to [CDC-ACM driver](https://components.espressif.com/components/espressif/usb_host_cdc_acm) to v2

## [1.0.0] - 2022-10-1

- Initial version
