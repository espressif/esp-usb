# USB Host MSC (Mass Storage Class) Driver

[![Component Registry](https://components.espressif.com/components/espressif/usb_host_msc/badge.svg)](https://components.espressif.com/components/espressif/usb_host_msc) ![maintenance-status](https://img.shields.io/badge/maintenance-passively--maintained-yellowgreen.svg) ![changelog](https://img.shields.io/badge/Keep_a_Changelog-blue?logo=keepachangelog&logoColor=E05735)

This directory contains an implementation of a USB Mass Storage Class Driver implemented on top of the [USB Host Library](https://components.espressif.com/components/espressif/usb).

MSC driver allows access to USB flash drivers using the BOT (Bulk-Only Transport) protocol and the Transparent SCSI command set.

## Usage

- First, usb host library has to be initialized by calling `usb_host_install`
- USB Host Library events have to be handled by invoking `usb_host_lib_handle_events` periodically. In general, an application should spawn a dedicated task handle USB Host Library events. However, in order to save RAM, an already existing task can also be used to call `usb_host_lib_handle_events`.
- Mass Storage Class driver is installed by calling `usb_msc_install` function along side with configuration.
- Supplied configuration contains user provided callback function invoked whenever MSC device is connected/disconnected and optional parameters for creating background task handling MSC related events. Alternatively, user can call `usb_msc_handle_events` function from already existing task.
- After receiving `MSC_DEVICE_CONNECTED` event, the application installs LUN 0 with `msc_host_install_device`, or chooses another LUN as described below, obtaining an MSC device handle. Perform this blocking operation outside the event callback while another task continues USB event processing.
- USB descriptors can be printed out with `usb_msc_print_descriptors` and general information about MSC device retrieved with `from usb_msc_get_device_info` function.
- Obtained device handle is then used in helper function `usb_msc_vfs_register` mounting USB Disk to Virtual filesystem.
- At this point, standard C functions for accessing storage (`fopen`, `fwrite`, `fread`, `mkdir` etc.) can be carried out.
- In order to uninstall the whole USB stack, deinitializing counterparts to functions above has to be called in reverse order.

## Logical units

Multi-slot card readers can expose each slot as a different logical unit (LUN). The application decides which LUN to use:

- `msc_host_install_device(address, &device)` keeps the original LUN 0 selection without issuing `GET_MAX_LUN` or searching other slots.
- `msc_host_install_device_lun(address, lun, &device)` installs exactly the specified LUN in the range 0..15. LUN 0 does not require `GET_MAX_LUN`; a nonzero LUN is checked against the device's response. Installation never falls back to another LUN if the requested one is absent, unready, or fails initialization.
- `msc_host_probe_luns(address, timeout_ms, &info)` opens a temporary session, queries `GET_MAX_LUN`, and scans all advertised LUNs before closing the session. It does not install, mount, or select a LUN. A legal `GET_MAX_LUN` STALL means only LUN 0 is advertised; malformed responses and USB transport errors fail the operation.

Each installation or probe starts with a Bulk-Only Mass Storage Reset and clears both bulk endpoint halts before sending SCSI commands. This synchronizes the device with the newly allocated host pipes, including after a previous session failed or another LUN was uninstalled. Uninstallation releases the session without sending commands to the device, so it also works after disconnection.

Use the probe result only when the call returns `ESP_OK`. `msc_host_lun_info_t` contains:

| Field             | Meaning                                                                                                                                 |
| ----------------- | --------------------------------------------------------------------------------------------------------------------------------------- |
| `max_lun`         | Highest advertised LUN, not the number of inserted cards.                                                                               |
| `ready_lun_mask`  | Bit n is set when LUN n passes TEST UNIT READY and READ CAPACITY with a power-of-two sector size from 512 through 4096 bytes.           |
| `failed_lun_mask` | Bit n is set when LUN n fails INQUIRY or READ CAPACITY, reports a non-retryable readiness error, or reports an unsupported sector size. |

An advertised LUN with neither bit set remained unready, including an empty slot. A complete probe with no ready LUNs still returns `ESP_OK`. A failed probe clears the output, so it cannot be used as a partial list of candidates. Ready means a usable block-device candidate; the probe does not check filesystems or write permissions, and does not format any media.

A zero `timeout_ms` scans every advertised LUN once. LUNs reporting NOT READY / MEDIUM NOT PRESENT (`02/3A/xx`) are skipped without retrying, with neither result bit set. A nonzero timeout gives other retryable readiness failures one shared retry budget; finding a ready LUN does not end the scan. The probe returns as soon as all LUNs are resolved, so an empty slot does not delay a ready candidate until the budget expires. Each USB transfer retains its own timeout, so this budget is not a strict end-to-end deadline. Installation and reset recovery retain their existing readiness retry behavior.

Insert the cards before probing or installing, and keep the reader connected and the cards unchanged through selection and installation. Probe results describe observations during the call, not persistent media identities. Explicit installation revalidates the selected LUN. Only one LUN of the selected MSC interface can be installed at a time. Its binding stays fixed for I/O and reset recovery until uninstall. To choose a different LUN or change cards, first unmount the filesystem and uninstall the device, then install again. A filesystem mount failure does not switch LUNs automatically.

Probe only before installation; probing a device that is already installed returns `ESP_ERR_INVALID_STATE`. Serialize probing, installation, and uninstallation in the application, and stop I/O before uninstalling. These calls block and must not run in the MSC event callback; another task must continue processing USB events. Temporary probe handles are not delivered in MSC events.

### Application selection examples

These independent snippets run in an application function returning `esp_err_t`. Include `usb/msc_host.h`; `address` is the connected device address, and `device` is a valid `msc_host_device_handle_t *` output argument with `*device` initially NULL. Mount the filesystem after the selected installation succeeds.

**Known slot: install LUN 1 directly.** No preliminary discovery is needed when the application already knows the required slot.

```c
return msc_host_install_device_lun(address, 1, device);
```

**Accept a unique candidate automatically.** No ready candidates returns `ESP_ERR_NOT_FOUND`; multiple candidates require an application decision, represented here by `ESP_ERR_INVALID_STATE`.

```c
msc_host_lun_info_t info;
esp_err_t err = msc_host_probe_luns(address, 1000, &info);
if (err != ESP_OK) {
    return err;
}
uint16_t candidates = info.ready_lun_mask;
if (candidates == 0) {
    return ESP_ERR_NOT_FOUND;
}
if ((candidates & (candidates - 1U)) != 0) {
    return ESP_ERR_INVALID_STATE; // Application must choose among candidates.
}
uint8_t lun = 0;
while ((candidates & (1U << lun)) == 0) {
    ++lun;
}
return msc_host_install_device_lun(address, lun, device);
```

**Choose among multiple candidates in the application.** The application may use a configured slot, a priority rule, or a user choice. Here, `chosen_lun` is an application-supplied `uint8_t` input; the completed probe confirms that it is a ready candidate.

```c
if (chosen_lun > 15) {
    return ESP_ERR_INVALID_ARG;
}
msc_host_lun_info_t info;
esp_err_t err = msc_host_probe_luns(address, 1000, &info);
if (err != ESP_OK) {
    return err;
}
if ((info.ready_lun_mask & (1U << chosen_lun)) == 0) {
    return ESP_ERR_NOT_FOUND;
}
return msc_host_install_device_lun(address, chosen_lun, device);
```

**Explicitly accept any ready candidate.** This application policy chooses the lowest set bit. The driver itself does not apply this preference.

```c
msc_host_lun_info_t info;
esp_err_t err = msc_host_probe_luns(address, 1000, &info);
if (err != ESP_OK) {
    return err;
}
if (info.ready_lun_mask == 0) {
    return ESP_ERR_NOT_FOUND;
}
uint8_t lun = 0;
while ((info.ready_lun_mask & (1U << lun)) == 0) {
    ++lun;
}
return msc_host_install_device_lun(address, lun, device);
```

## Performance tuning

The following performance tuning options have significant impact on data throughput in USB HighSpeed implementations. For original FullSpeed implementations, the effects are negligible.

- By default, Newlib (the implementation of C Standard Library) creates cache for each opened file
- The greater the cache, the better performance for the cost of RAM
- Size of the cache can be set with C STD library function `setvbuf()`
- Sizes over 16kB do not improve the performance any more

## Known issues

- Driver only supports flash drives using the BOT (Bulk-Only Transport) protocol and the Transparent SCSI command set

## Examples

- For an example, refer to [msc_host_example](https://github.com/espressif/esp-idf/tree/master/examples/peripherals/usb/host/msc) in ESP-IDF
