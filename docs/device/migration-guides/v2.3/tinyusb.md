# Migration guide to Espressif's TinyUSB addition v2.3

## 1. Change description

**If your project depends only on `esp_tinyusb` and already uses ESP-IDF 5.3 or later, the upgrade requires no application code changes.**

v2.3.0 integrates the TinyUSB stack into the `esp_tinyusb` component as a git submodule. `esp_tinyusb` no longer depends on a separate `tinyusb` component from the ESP Component Registry.

You need to take action if any of the following applies:

- Your project selected a TinyUSB version independently of `esp_tinyusb`. That selection has no effect. The build uses the TinyUSB version bundled in `esp_tinyusb`.
- Your project uses ESP-IDF older than 5.3.

## 2. Changes Required After Migration

- Use ESP-IDF 5.3 or later.
- Use the TinyUSB version that the `esp_tinyusb` release bundles. You can no longer pair `esp_tinyusb` with a different TinyUSB version.

Remove any explicit `tinyusb` (or `espressif/tinyusb`) dependency from your project's `idf_component.yml` unless you have a reason to keep it. The standalone component is unused when `esp_tinyusb` is present. Keeping it still compiles a second copy of TinyUSB and can confuse version selection.

Applications that use TinyUSB without `esp_tinyusb` continue to use the standalone [`tinyusb`](https://components.espressif.com/components/espressif/tinyusb) component.
