# Espressif's Additions to TinyUSB - Disconnect Detection Test Application

This directory contains Unity tests that validate Espressif-specific integration of TinyUSB.

The tests focus on:

- USB device disconnect detection via VBUS monitoring.
- Delivery of `TINYUSB_EVENT_DETACHED` events when VBUS drops.
- Repeated attach/detach cycles using a simulated VBUS signal on GPIO (for automated CI testing).
