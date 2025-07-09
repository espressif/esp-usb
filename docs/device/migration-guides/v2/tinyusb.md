# Migration guide to Espressif's TinyUSB addition

## 1. Do I need to migrate?

This migration guide is intended for users upgrading from Espressif's TinyUSB addition v1.x.x to the v2.0.0 of the component. If you are already using v2.0.0 or later, no migration is needed.

The primary feature of this update is the introduction of dynamic configuration for the TinyUSB Device Stack. This enhancement allows users to modify the USB Device configuration at run-time, without needing to reconfigure the entire project.

These changes are reflected in the updated configuration structure for the TinyUSB driver and its member names.

## 2. Changes Required After Migration

You will need to update how the TinyUSB driver is configured. This includes:

- Adding the default configuration using the `TINYUSB_DEFAULT_CONFIG()` macro.
- Updating members of the default configuration with custom values, if necessary.
- Removing the `tud_mount_cb()` and `tud_umount_cb()` functions from your code, and instead implementing the new USB Device Event callback (`tinyusb_config_t::event_cb`) to handle device events such as attach and detach.

After migration, you will be able to configure the TinyUSB Device Stack at run time and handle device events in your code, also when MSC Storage is enabled.

```c
#include "tinyusb_default_config.h"
#include "tinyusb.h"

/**
 * @brief TinyUSB callback for device event handler
 *
 * @note
 * For Linux-based Hosts: Reflects the SetConfiguration() request from the Host Driver.
 * For Win-based Hosts: SetConfiguration() request is present only with available Class in device descriptor.
 */
void device_event_handler(tinyusb_event_t *event, void *arg)
{
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        // Device has been attached to the USB Host and configured
        break;
    case TINYUSB_EVENT_DETACHED:
        // Device has been detached from the USB Host
        break;
    default:
        break;
    }
}

void main(void)
{
    tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG(device_event_handler);
    // Run TinyUSB on the High-speed port
    config.port = TINYUSB_PORT_HIGH_SPEED_0;
    // Install TinyUSB driver
    tinyusb_driver_install(&config);
    // Use the USB device, then uninstall the TinyUSB stack
    tinyusb_driver_uninstall();

    // Re-configure the TinyUSB to the Full-speed port
    config.port = TINYUSB_PORT_FULL_SPEED_0;
    // Install TinyUSB driver
    tinyusb_driver_install(&config);
    // Do something and uninstall the TinyUSB Stack
    tinyusb_driver_uninstall();
}
```

## 3. List of changes

### Added

- Default configuration macros for the TinyUSB Device Stack, which can be called with zero, one, or two arguments:

    - `TINYUSB_DEFAULT_CONFIG()`,
    - `TINYUSB_DEFAULT_CONFIG(device_event_handler)`,
    - `TINYUSB_DEFAULT_CONFIG(device_event_handler, device_event_handler_argument)`.

- Port configuration `tinyusb_config_t::port`
- Internal Task size configuration `tinyusb_config_t::task.size`
- Internal Task priority configuration `tinyusb_config_t::task.priority`
- Internal Task CPU Affinity mask `tinyusb_config_t::task.xCoreID`
- USB Device Event callback `tinyusb_config_t::event_cb` and its argument `tinyusb_config_t::event_arg`

### Removed

- Task configuration from the Kconfig:
    - `TINYUSB_NO_DEFAULT_TASK`,
    - `TINYUSB_TASK_PRIORITY`,
    - `TINYUSB_TASK_STACK_SIZE`,
    - `TINYUSB_TASK_AFFINITY`,
    - `TINYUSB_INIT_IN_DEFAULT_TASK`.
- Choice for the Peripheral port from Kconfig:
    - `TINYUSB_RHPORT`,
    - `TINYUSB_RHPORT_HS`,
    - `TINYUSB_RHPORT_FS`.
- Configure and run the `tud_task()` in the External Task.
- Universal `tinyusb_config_t::configuration_descriptor` member for Full- and High-speed Configuration Descriptor.

### Renamed

| Old name                                        | New name                                        |
|-------------------------------------------------|-------------------------------------------------|
| `tinyusb_config_t::device_descriptor`           | `tinyusb_config_t::descriptor.device`           |
| `tinyusb_config_t::string_descriptor`           | `tinyusb_config_t::descriptor.string`           |
| `tinyusb_config_t::string_descriptor_count`     | `tinyusb_config_t::descriptor.string_count`     |
| `tinyusb_config_t::qualifier_descriptor`        | `tinyusb_config_t::descriptor.qualifier`        |
| `tinyusb_config_t::fs_configuration_descriptor` | `tinyusb_config_t::descriptor.full_speed_config`|
| `tinyusb_config_t::hs_configuration_descriptor` | `tinyusb_config_t::descriptor.high_speed_config`|
| `tinyusb_config_t::external_phy`                | `tinyusb_config_t::phy.skip_setup`              |
| `tinyusb_config_t::self_powered`                | `tinyusb_config_t::phy.self_powered`            |
| `tinyusb_config_t::vbus_monitor_io`             | `tinyusb_config_t::phy.vbus_monitor_io`         |

## 4. Common Migration Errors and Solutions

**Possible error**

```bash
error: 'tinyusb_config_t' has no member named 'device_descriptor'
```
or
```bash
error: incompatible types when initializing type 'enum <anonymous>' using type 'void *'
    |         .device_descriptor = NULL,
    |                              ^~~~
```

**How to fix**

The member `device_descriptor` was renamed to `descriptor.device` in the new version.
Update your code from:

```c
const tinyusb_config_t config = {
    .device_descriptor = ...,
};
```
to:
```c
tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG();
config.descriptor.device = ...;
```

**Possible error**

```bash
error: 'tinyusb_config_t' has no member named 'string_descriptor'
```
or
```bash
error: the address of 'hid_string_descriptor' will always evaluate as 'true' [-Werror=address]
    |         .string_descriptor = hid_string_descriptor,
    |         ^
```

**How to fix**

The member `string_descriptor` was renamed to `descriptor.string` in the new version.
Update your code from:
```c
const tinyusb_config_t config = {
    .string_descriptor = ...,
};
```
to:
```c
tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG();
config.descriptor.string = ...;
```

**Possible error**

```bash
error: 'tinyusb_config_t' has no member named 'string_descriptor_count'
```

**How to fix**

The member `string_descriptor_count` was renamed to `descriptor.string_count` in the new version.
Update your code from:
```c
const tinyusb_config_t config = {
    .string_descriptor_count = ...,
};
```
to:
```c
tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG();
config.descriptor.string_count = ...;
```

**Possible error**

```bash
error: 'tinyusb_config_t' has no member named 'configuration_descriptor'
```
or:
```bash
error: initialization of 'void (*)(tinyusb_event_t *, void *)' from incompatible pointer type 'const uint8_t *'
      |         .configuration_descriptor = ...,
      |
```

**How to fix**

The member `fs_configuration_descriptor` was renamed to `descriptor.full_speed_config` in the new version.
Update your code from:
```c
const tinyusb_config_t config = {
    .fs_configuration_descriptor = ...,
};
```
to:
```c
tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG();
config.descriptor.full_speed_config = ...;
```

**Possible error**

```bash
error: 'tinyusb_config_t' has no member named 'fs_configuration_descriptor'
```
or:
```bash
error: initialization of 'void (*)(tinyusb_event_t *, void *)' from incompatible pointer type 'const uint8_t *'
      |         .fs_configuration_descriptor = ...,
      |
```

**How to fix**

The member `fs_configuration_descriptor` was renamed to `descriptor.full_speed_config` in the new version.
Update your code from:
```c
const tinyusb_config_t config = {
    .fs_configuration_descriptor = ...,
};
```
to:
```c
tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG();
config.descriptor.full_speed_config = ...;
```

**Possible error**

```bash
error: 'tinyusb_config_t' has no member named 'hs_configuration_descriptor'
```
or:
```bash
error: initialization of 'void (*)(tinyusb_event_t *, void *)' from incompatible pointer type 'const uint8_t *'
      |         .hs_configuration_descriptor = ...,
      |
```

**How to fix**

The member `hs_configuration_descriptor` was renamed to `descriptor.high_speed_config` in the new version.
Update your code from:
```c
const tinyusb_config_t config = {
    .hs_configuration_descriptor = ...,
};
```
to:
```c
tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG();
config.descriptor.high_speed_config = ...;
```

**Possible error**

```bash
error: 'tinyusb_config_t' has no member named 'qualifier_descriptor'
```

**How to fix**

The member `qualifier_descriptor` was renamed to `descriptor.qualifier` in the new version.
Update your code from:
```c
const tinyusb_config_t config = {
    .qualifier_descriptor = ...,
};
```
to:
```c
tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG();
config.descriptor.qualifier = ...;
```

**Possible error**

```bash
error: 'tinyusb_config_t' has no member named 'external_phy'
      |         .external_phy = true,
      |          ^~~~~~~~~~~~
```

**How to fix**

The member `external_phy` was renamed to `phy.skip_setup` in the new version.
Update your code from:
```c
const tinyusb_config_t config = {
    .external_phy = true,
};
```
to:
```c
tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG();
config.phy.skip_setup = true;
```

**Possible error**

```bash
error: 'tinyusb_config_t' has no member named 'self_powered'
      |         .self_powered = true,
      |          ^~~~~~~~~~~~
```
or:
```bash
warning: excess elements in struct initializer
      |         .self_powered = true,
      |                         ^~~~
```

**How to fix**

The member `self_powered` was renamed to `phy.self_powered` in the new version.
Update your code from:
```c
const tinyusb_config_t config = {
    .self_powered = true,
};
```
to:
```c
tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG();
config.phy.self_powered = true;
```

**Possible error**

```bash
error: 'tinyusb_config_t' has no member named 'vbus_monitor_io'
  465 |         .vbus_monitor_io = GPIO_NUM_4,
      |          ^~~~~~~~~~~~~~~
```
or:
```bash
warning: excess elements in struct initializer
      |         .vbus_monitor_io = GPIO_NUM_4,
```

**How to fix**

The member `vbus_monitor_io` was renamed to `phy.vbus_monitor_io` in the new version.
Update your code from:
```c
const tinyusb_config_t config = {
    .vbus_monitor_io = GPIO_NUM_4,
};
```
to:
```c
tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG();
config.phy.vbus_monitor_io = GPIO_NUM_4;
```

**Possible error**

```bash
error: missing braces around initializer [-Werror=missing-braces]
      |     const tinyusb_config_t tusb_cfg = {
      |
```

**How to fix**

The default configuration macros `TINYUSB_DEFAULT_CONFIG()` was introduced.
Update your code from:
```c
const tinyusb_config_t config = {
    ...,
};
```
to:
```c
tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG();
```

**Possible error**

```bash
error: implicit declaration of function 'TINYUSB_DEFAULT_CONFIG' [-Wimplicit-function-declaration]
      |     tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
      |                                 ^~~~~~~~~~~~~~~~~~~~~~
```

**How to fix**

Add the additional header to your code:

```c
#include "tinyusb_default_config.h"
```

**Possible error**

Wrong port after using the `TINYUSB_DEFAULT_CONFIG()` macros.

> **Note:**
> For example, on the ESP32P4 there are two available USB ports: High-speed and Full-speed.
> By default, the High-speed port is used if it is available.

**How to fix**

Explicitly select the desired port by setting the `tinyusb_config_t::port` member in your code:
```c
tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG();
config.port = TINYUSB_PORT_FULL_SPEED_0;
```

**Possible error**

```bash
multiple definition of `tud_umount_cb'; esp-idf/main/libmain.a(tusb_hid_example_main.c.obj):/esp-idf/examples/peripherals/usb/device/tusb_hid/main/tusb_hid_example_main.c:162: first defined here
collect2: error: ld returned 1 exit status
```

or:

```bash
multiple definition of `tud_mount_cb'; esp-idf/main/libmain.a(tusb_hid_example_main.c.obj):/esp-idf/examples/peripherals/usb/device/tusb_hid/main/tusb_hid_example_main.c:156: first defined here
collect2: error: ld returned 1 exit status
```


**How to fix**
The `tud_mount_cb()` and `tud_umount_cb()` callbacks are now handled internally. To handle attach and detach events in your application, use the USB Device Event callback (`tinyusb_config_t::event_cb`).

Update your code from:

```c
void tud_mount_cb(void)
{
    ESP_LOGI(TAG, "USB device attached to the Host");
}

void tud_umount_cb(void)
{
    ESP_LOGI(TAG, "USB device detached from the Host");
}

void main(void)
{
    tinyusb_config_t tusb_cfg = { ... };
    tinyusb_driver_install(&tusb_cfg);
}
```

to:

```c
void device_event_handler(tinyusb_event_t *event, void *arg)
{
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        ESP_LOGI(TAG, "USB device attached to the Host");
        break;
    case TINYUSB_EVENT_DETACHED:
        ESP_LOGI(TAG, "USB device detached from the Host");
        break;
    default:
        break;
    }
}

void main(void)
{
    tinyusb_config_t config = TINYUSB_DEFAULT_CONFIG(device_event_handler);
    tinyusb_driver_install(&config);
}
```
