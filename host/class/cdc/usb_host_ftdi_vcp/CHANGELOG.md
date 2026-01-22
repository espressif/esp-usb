## 1.0.0
- Initial version

## 1.0.1
- Fix GCC-12 compile errors

## 2.0.0
- Update to [CDC-ACM driver](https://components.espressif.com/components/espressif/usb_host_cdc_acm) to v2

## Unreleased
- Fix FTDI VCP SerialState parsing (correct 1-bit flags, avoid short-packet reads)
- Added CTS state to `CDC_ACM_HOST_SERIAL_STATE` event (https://github.com/espressif/esp-usb/issues/360)
- Fixed data receiving with configured RX FIFO greater than IN endpoint's MPS
