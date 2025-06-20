# Espressif's additions to TinyUSB

[![Component Registry](https://components.espressif.com/components/espressif/esp_tinyusb/badge.svg)](https://components.espressif.com/components/espressif/esp_tinyusb)

This component adds features to TinyUSB that help users with integrating TinyUSB with their ESP-IDF application.

It provides simple and advanced usage of the TinyUSB to configure and run USB Device on the ESP chips and additional wrappers to configure and use several USB Device classes.

### Simple usage
* Default configuration for TinyUSB: descriptors, peripheral port, task and usb phy.

### Advanced usage
* Manual configuration for TinyUSB: descriptors, peripheral port, task and usb phy.

### USB Device classes
* USB Serial Device (CDC-ACM) with optional Virtual File System support
* Input and output streams through USB Serial Device. This feature is available only when Virtual File System support is enabled.
* Mass Storage Device Class (MSC): allows to create USB flash drives with storages in SPI Flash or SD/MMC.
* Other USB classes (MIDI, NET, HID…) support directly via TinyUSB

## Documentation and examples
You can find documentation in [ESP-IDF Programming Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s2/api-reference/peripherals/usb_device.html).

You can find examples in [ESP-IDF on GitHub](https://github.com/espressif/esp-idf/tree/master/examples/peripherals/usb/device).
## How to use?

This component is distributed via [IDF component manager](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/tools/idf-component-manager.html). Just add `idf_component.yml` file to your main component with the following content:

``` yaml
## IDF Component Manager Manifest File
dependencies:
  esp_tinyusb: "~1.0.0"
```

Or simply run:
```
idf.py add-dependency esp_tinyusb~1.0.0
```
