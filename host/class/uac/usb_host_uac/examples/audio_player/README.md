| Supported Targets | ESP32-S2 | ESP32-S3 | ESP32-P4 |
| ----------------- | -------- | -------- | -------- |

# UAC driver example

## Selecting the USB Component

To manually select which USB Component shall be used to build this example, please refer to the following documentation page: [Manual USB component selection](../../../../../../docs/host/usb_host_lib/usb_component_manual_selection.md).

## How to use this example

This example demonstrates how to use a USB Audio Class (UAC) driver to play siren sounds or audio from a USB microphone.

### Enable MIC playback

By default, the example plays siren sounds. To enable MIC playback, you need to enable the MIC playback feature in the menuconfig `(Top) → Example USB Audio Player → Playback audio from microphone`

### Hardware Required

- USB audio device
- ESP32-xx board with USB support
- USB cable

### Build and Flash

```sh
idf.py set-target (YOUR_TARGET)
idf.py build
idf.py -p (PORT) flash monitor
```

### Running the example

After flashing the example, connect the USB audio device to the ESP32-xx board. The audio should start playing automatically.
