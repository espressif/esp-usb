# USB Host Library

[![Component Registry](https://components.espressif.com/components/espressif/usb/badge.svg)](https://components.espressif.com/components/espressif/usb)

This directory contains an implementation of a USB Host Library [USB Host Library](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s2/api-reference/peripherals/usb_host.html).

USB Host Library works underneath the USB Host Class drivers:
- [CDC](../class/cdc/) Communication Device Class (ACM)
- [HID](../class/hid/usb_host_hid/) Human Interface Device
- [MSC](../class/msc/usb_host_msc/) Mass Storage Class
- [UAC](../class/uac/usb_host_uac/) USB Audio Class
- [UVC](../class/uvc/usb_host_uvc/) USB Video Class

## Usage

To include this component in your project:

1. Add the USB Component to your project using the [Espressif Component Registry](https://components.espressif.com/):

Using `idf.py` command:

```python
idf.py add-dependency espressif/usb
```
Or modifying you `idf_component.yml` file:

```yaml
## IDF Component Manager Manifest File
dependencies:
  ...
  espressif/usb: "*"
```

2. Include the USB Host header file in your code:

```c
#include "usb/usb_host.h"
```

3. Initialize the USB Host stack:
```c
usb_host_config_t host_config = {/* configuration */};
usb_host_install(&host_config);
```

4. Use class drivers (CDC, HID, ets.) on top of the USB Host Library, to interact with USB devices


## Examples

Refer to following examples, using USB Host library from esp-idf:
- [CDC-ACM Host](https://github.com/espressif/esp-idf/tree/master/examples/peripherals/usb/host/cdc/cdc_acm_host)
- [CDC-ACM VCP](https://github.com/espressif/esp-idf/tree/master/examples/peripherals/usb/host/cdc/cdc_acm_vcp)
- [HID Host](https://github.com/espressif/esp-idf/tree/master/examples/peripherals/usb/host/hid)
- [MSC Host](https://github.com/espressif/esp-idf/tree/master/examples/peripherals/usb/host/msc)
- [USB Host Library](https://github.com/espressif/esp-idf/tree/master/examples/peripherals/usb/host/usb_host_lib)
- [UVC Host](https://github.com/espressif/esp-idf/tree/master/examples/peripherals/usb/host/uvc)

## Supported targets

- ESP32-S2
- ESP32-S3
- ESP32-P4
