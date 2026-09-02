# Migration guide to Espressif's TinyUSB addition

This migration guide is intended for users upgrading from Espressif's TinyUSB addition v2.x.x to v3.0.0 of the component. If you are already using v3.0.0 or later, no migration is needed.

v3.0.0 removes DCD Slave/IRQ mode. The TinyUSB DWC2 Device Controller Driver uses Buffer DMA mode only.

If your project used the default DCD mode (Buffer DMA), you do not need to change application code. If your project selected Slave/IRQ mode, you must switch to Buffer DMA. Slave/IRQ mode is no longer available.

## Changes Required After Migration

- Do not set `CONFIG_TINYUSB_MODE_SLAVE` or `CONFIG_TINYUSB_MODE_DMA` as they do not have any effect anymore

## List of changes

### Removed

- Kconfig menu `TinyUSB DCD` and choice `TINYUSB_MODE`:
  - `TINYUSB_MODE_SLAVE`,
  - `TINYUSB_MODE_DMA`.
- Compile-time enable of DCD Slave/IRQ mode in `tusb_config.h`:
  - `CFG_TUD_DWC2_SLAVE_ENABLE`.

### Changed

- Buffer DMA is always enabled. `CFG_TUD_DWC2_DMA_ENABLE` is set to `1` unconditionally. Previously it was set only when `CONFIG_TINYUSB_MODE_DMA` was selected.
