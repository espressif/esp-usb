# Migration guide to Espressif's TinyUSB addition: MSC Storage

## 1. Do I need to migrate?

The new version has been redesigned for better maintainability and to enable future feature extensions without introducing breaking changes. If you are already using v2.0.0 or later, no migration is needed.

Potential new features include:

- Specific settings to define the behavior of the storage medium (e.g., whether to mount on connection);
- Support for multiple storage devices (LUNs) on a single device.

## 2. Changes Required After Migration

To simplify the migration process, partial compatibility with the previous API has been retained. During the first build, you will see the following warning:

```bash
"This header file is deprecated since v2.0.0. Callback prototype has changed. Use tinyusb_msc.h for the new development and refer to the migration guide for more details."
```

The old API is still supported, except for the callbacks.

You can disable the callbacks and continue running your old code, but it is recommended to update your code, as the old API may be removed in future releases.

The new version simplifies event handling and provides additional features, such as notifications if the device storage was not mounted or requires formatting or the ability to format storage.

## 3. List of changes

### Removed

- Event callbacks for SPI Flash Storage:
  - `tinyusb_msc_spiflash_config_t::callback_mount_changed`;
  - `tinyusb_msc_spiflash_config_t::callback_premount_changed`.
- Event callbacks for SD/MMC Storage:
  - `tinyusb_msc_sdmmc_config_t::callback_mount_changed`;
  - `tinyusb_msc_sdmmc_config_t::callback_premount_changed`.
- FAT Filesystem Kconfig format options:
  - `TINYUSB_FAT_FORMAT_TYPE`;
  - `TINYUSB_FAT_FORMAT_ANY`;
  - `TINYUSB_FAT_FORMAT_FAT`;
  - `TINYUSB_FAT_FORMAT_FAT32`;
  - `TINYUSB_FAT_FORMAT_EXFAT`;
  - `TINYUSB_FAT_FORMAT_SFD`.
- Configuration structure for SPI Flash storage `tinyusb_msc_spiflash_config_t`.
- Configuration structure for SD/MMC storage `tinyusb_msc_sdmmc_config_t`.

### Added

- One callback for Storage events `tinyusb_msc_driver_config_t::callback` with optional argument `tinyusb_msc_driver_config_t::callback_arg`. This callback allows to get the following events:
  - `TINYUSB_MSC_EVENT_MOUNT_START`;
  - `TINYUSB_MSC_EVENT_MOUNT_COMPLETE`;
  - `TINYUSB_MSC_EVENT_MOUNT_FAILED`;
  - `TINYUSB_MSC_EVENT_FORMAT_REQUIRED`.
- Storage handle `tinyusb_msc_storage_handle_t`.
- Configuration structure for Storage `tinyusb_msc_storage_config_t`.
- Storage Format `tinyusb_msc_format_storage()`
- Storage set FAT Filesystem configuration `tinyusb_msc_config_storage_fat_fs`

## 4. Common Migration Errors and Solutions

**Possible error**

```bash
warning: #warning "This header file is deprecated since v2.0.0. Callback prototype has changed. Use tinyusb_msc.h for the new development and refer to the migration guide for more details." [-Wcpp]
```

**How to fix?**

New header file was introduced. Update your code from:

```c
#include "tusb_msc_storage.h"
```

to:

```c
#include "tinyusb_msc.h"
```

**Possible error**

```bash
warning: 'mount_changed_data' is deprecated [-Wdeprecated-declarations]
      |     ESP_LOGI(TAG, "Storage mounted to application: %s", event->mount_changed_data.is_mounted ? "Yes" : "No");
      |     ^~~~~~~~
```

**How to fix?**

Since the callback prototype has changed, the old members are now marked as deprecated. To obtain the mount point for the storage, use the new API: `tinyusb_msc_get_storage_mount_point()`.

Update your code with:

```c
tinyusb_msc_mount_point_t mp;
tinyusb_msc_get_storage_mount_point(storage_hdl, &mp);
```

**Possible error**

```bash
error: initialization of 'void (*)(struct tinyusb_msc_storage_handle_s *, tinyusb_msc_event_t *, void *)' from incompatible pointer type 'void (*)(tinyusb_msc_event_t *)' [-Wincompatible-pointer-types]
      |         .callback_mount_changed = storage_mount_changed_cb,
      |
```

**How to fix?**

Callback prototype has changed. Update your code from:

```c
static void storage_mount_changed_cb(tinyusb_msc_event_t *event)
{
    ESP_LOGI(TAG, "Storage mounted to application: %s", event->mount_changed_data.is_mounted ? "Yes" : "No");
}

void main(void)
{
    const tinyusb_msc_spiflash_config_t config_spi = {
        .wl_handle = wl_handle,
        .callback_mount_changed = storage_mount_changed_cb,
        .mount_config.max_files = 5,
    };
    tinyusb_msc_storage_init_spiflash(&config_spi);
}
```

to:

```c
static void storage_event_cb(tinyusb_msc_storage_handle_t handle, tinyusb_msc_event_t *event, void *arg)
{
    tinyusb_msc_mount_point_t mp;
    tinyusb_msc_get_storage_mount_point(handle, &mp);

    switch (event->id) {
    case TINYUSB_MSC_EVENT_MOUNT_COMPLETE:
        ESP_LOGI(TAG, "Storage mounted to application: %s", mp == TINYUSB_MSC_STORAGE_MOUNT_APP ? "Yes" : "No");
        break;
    case TINYUSB_MSC_EVENT_MOUNT_START:
    case TINYUSB_MSC_EVENT_MOUNT_FAILED:
    case TINYUSB_MSC_EVENT_FORMAT_REQUIRED:
    default:
        break;
    }
}

void main(void)
{
    const tinyusb_msc_spiflash_config_t config_spi = {
        .wl_handle = wl_handle,
        .mount_config.max_files = 5,
    };
    tinyusb_msc_storage_init_spiflash(&config_spi);
    tinyusb_msc_set_storage_callback(storage_event_cb, NULL /* optional argument */);
}

```
