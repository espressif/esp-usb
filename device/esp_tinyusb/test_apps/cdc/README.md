# Espressif's Additions to TinyUSB - CDC-ACM Test Application

This directory contains Unity tests that validate Espressif-specific integration of TinyUSB.

The tests focus on:

- Dual CDC-ACM interfaces: one accessed via callback API, the other mapped to VFS.
- Data echo over both virtual COM ports, including transfers larger than the endpoint MPS and RX buffer.
- CDC-ACM device-to-host throughput measurement.
