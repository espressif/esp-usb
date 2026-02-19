# ESP Type-C (usb_tcpm)

[![Component Registry](https://components.espressif.com/components/espressif/usb_tcpm/badge.svg)](https://components.espressif.com/components/espressif/usb_tcpm) ![maintenance-status](https://img.shields.io/badge/maintenance-experimental-blue.svg) ![changelog](https://img.shields.io/badge/Keep_a_Changelog-blue?logo=keepachangelog&logoColor=E05735)

Minimal USB Type-C CC library for attach/detach, cable orientation, power-role control, and status snapshot. The current implementation uses the FUSB302 TCPC for CC detection and interrupt handling.

## Supported Controllers

- FUSB302 (CC attach/detach and role control)

## Usage

1. Install the TCPM library with `usb_tcpm_install()`
2. Create a port using `usb_tcpm_port_create_fusb302()`
3. Register for `USB_TCPM_EVENT` events via `esp_event_handler_register()`
4. (Optional) Read a snapshot with `usb_tcpm_get_status()`
5. Destroy the port with `usb_tcpm_port_destroy()`
6. Uninstall the library with `usb_tcpm_uninstall()`

### Install Configuration

`usb_tcpm_install_config_t` configures the shared worker task:

- `task_stack`: shared task stack size in bytes (`0` = default)
- `task_prio`: shared task priority (`0` = default)

Example:

```c
const usb_tcpm_install_config_t install_cfg = {
    .task_stack = 4096,
    .task_prio = 5,
};
ESP_ERROR_CHECK(usb_tcpm_install(&install_cfg));
```

### Runtime Model

- One shared worker task is created during `usb_tcpm_install()`.
- Each created port only registers its backend and IRQ.
- Port IRQs notify the shared worker, which services pending ports one by one.
- `usb_tcpm_uninstall()` succeeds only when no ports are active.

## Examples

- `examples/pd_fusb302` (logs attach/detach events and prints a one-shot status snapshot after startup)
