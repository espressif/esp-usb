# USB Host UAC Driver

[![Component Registry](https://components.espressif.com/components/espressif/usb_host_uac/badge.svg)](https://components.espressif.com/components/espressif/usb_host_uac)

This directory contains an implementation of a USB UAC Driver implemented on top of the [USB Host Library](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s2/api-reference/peripherals/usb_host.html).

UAC driver allows access to UAC 1.0 devices.

## Usage

The following steps outline the typical API call pattern of the UAC Class Driver:

1. Install the USB Host Library via `usb_host_install()`
2. Install the UAC driver via `uac_host_install()`
3. When the new (logic) UAC device is connected, the driver event callback will be called with USB device address and event:
    - `UAC_HOST_DRIVER_EVENT_TX_CONNECTED`
    - `UAC_HOST_DRIVER_EVENT_RX_CONNECTED`
4. To open/close the UAC device with USB device address and interface number:
    - `uac_host_device_open()`
    - `uac_host_device_close()`
5. To get the device-supported audio format use:
    - `uac_host_get_device_info()`
    - `uac_host_get_device_alt_param()`
6. To enable/disable data streaming with specific audio format use:
    - `uac_host_device_start()`
    - `uac_host_device_stop()`
7. To suspend/resume data streaming use:
    - `uac_host_device_suspend()`
    - `uac_host_device_resume()`
8. To control the volume/mute use:
    - `uac_host_device_set_mute()`
9. To control the volume use:
    - `uac_host_device_set_volume()` or `uac_host_device_set_volume_db()`
10. After the uac device is opened, the device event callback will be called with the following events:
    - UAC_HOST_DEVICE_EVENT_RX_DONE
    - UAC_HOST_DEVICE_EVENT_TX_DONE
    - UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR
    - UAC_HOST_DRIVER_EVENT_DISCONNECTED
11. When the `UAC_HOST_DRIVER_EVENT_DISCONNECTED` event is called, the device should be closed via `uac_host_device_close()`
12. The UAC driver can be uninstalled via `uac_host_uninstall()`

> Note: For physical device with both microphone and speaker, the driver will treat it as two separate logic devices.

> The `UAC_HOST_DRIVER_EVENT_TX_CONNECTED` and `UAC_HOST_DRIVER_EVENT_RX_CONNECTED` event will be called for the device.

## Known issues

- Empty

## Examples

- For an example, refer to [usb_audio_player](https://github.com/espressif/esp-iot-solution/tree/master/examples/usb/host/usb_audio_player)

## Supported Devices

- UAC Driver supports any UAC 1.0 compatible device.
