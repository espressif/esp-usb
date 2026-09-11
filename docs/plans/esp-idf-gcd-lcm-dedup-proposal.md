# Proposal: expose sector-size math helpers from ESP-IDF's esp_blockdev

## What

Two tiny helper functions — `gcd_size()` and `lcm2_size()` — live as `static inline` functions inside ESP-IDF's `components/fatfs/diskio/diskio_bdl.c`. They compute the FatFS logical sector size from an `esp_blockdev_handle_t`'s geometry: `LCM(FF_MIN_SS, read_size, write_size[, erase_size])`.

They are not declared in `diskio_bdl.h`, and no equivalent exists in the public `esp_blockdev` or `esp_blockdev_util` components.

## Why

esp-usb's `device/esp_tinyusb` component needed the exact same sector-size derivation for its new generic MSC storage backend (`storage_blockdev.c`, on branch `feat/usb-device-msc-blockdev`), so that the sector size reported to the USB/SCSI layer matches what FatFS itself would compute for the same block device handle. Since IDF doesn't expose these helpers, we had to copy them verbatim into our own file.

This is a real, general problem: **any component that wraps an `esp_blockdev_handle_t` into some other block-addressable interface** (not just FatFS, not just us) needs this same "give me a valid sector size from this geometry" logic. Right now every such consumer would have to reimplement or copy it, same as we did.

## Where

- Current (private) location: `components/fatfs/diskio/diskio_bdl.c` in ESP-IDF, functions `gcd_size()`/`lcm2_size()`, used inside `compute_fs_sector_size()`.
- Our copy: `device/esp_tinyusb/storage_blockdev.c` in esp-usb (this repo).
- Proposed new home (pick one):
  1. `components/esp_blockdev/include/esp_blockdev.h` — arguably the more correct home, since this is generic block-geometry math, not FatFS-specific.
  2. `components/esp_blockdev_util/` — already exists specifically to host shared helpers for blockdev consumers (see `generic_partition.h`, `memory.h` there today).

## Ask

Would the esp_blockdev/FatFS maintainers accept a small PR promoting `gcd_size`/`lcm2_size` (and maybe the full `compute_fs_sector_size`-style derivation, generalized) to one of the above public locations, so `diskio_bdl.c` and any other blockdev-adapter component (including ours) can share one implementation instead of copy-pasting it?

## Scope/impact if accepted

- Small, additive, non-breaking IDF change (new public inline functions).
- Once available, `device/esp_tinyusb/storage_blockdev.c` would need an `ESP_IDF_VERSION` gate to switch to the shared helper only on IDF versions that have it, keeping the local copy for older 6.0.x versions that predate the change.
