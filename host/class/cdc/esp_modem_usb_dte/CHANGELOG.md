## Unreleased

## 1.2.0

- Fixed C++ build error for `usb_host_config_t` backward compatibility
- Added default configuration for SimCom SIM7070G, SIM7080 and SIMA7672E modems
- Added default configuration for Quectel EC20
- Added ESP32-P4 support

## 1.1.0

- Updated to [CDC-ACM driver](https://components.espressif.com/components/espressif/usb_host_cdc_acm) to v2
- Added default configurations for tested modems
- Fixed USB receive path bug, where received data could be overwritten by new data
- Added initial support for modems with two AT ports

## 1.0.0

- Initial version
