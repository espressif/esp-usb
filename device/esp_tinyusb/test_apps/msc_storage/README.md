# Espressif's Additions to TinyUSB - MSC Storage Test Application

This directory contains Unity tests that validate Espressif-specific integration of TinyUSB.

The tests focus on:

- MSC storage driver install/uninstall and public API consistency.
- Storage backends: SPI Flash and SD/MMC, including dual-storage configurations.
- Filesystem access from both the application and the USB host.
- Storage formatting, auto-format, and multitask driver access.
