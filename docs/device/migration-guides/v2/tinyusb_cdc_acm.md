# Migration guide to Espressif's TinyUSB addition: CDC-ACM

## 1. Do I need to migrate?

Update brings the consistency for the Espressif's TinyUSB addition extensions public API.

## 2. Changes Required After Migration

To simplify the migration process, compatibility with the previous API has been retained. During the first build, you will see the following warning:

```bash
"This header file is deprecated since v2.0.0.  Use 'tinyusb_cdc_acm.h' for the new development."
```

The old API is still supported, but it is recommended to update your code, as the old API may be removed in future releases.

## 3. List of Changes

### Removed

- Removed `tinyusb_config_cdcacm_t::usb_dev` from the configuration structure.

### API Updates

The following function prototypes have been renamed to improve consistency. Update your code to use the new names:

| Old Function Prototype                                              | New Function Prototype                                              |
|---------------------------------------------------------------------|--------------------------------------------------------------------|
| `esp_err_t tusb_cdc_acm_init(const tinyusb_config_cdcacm_t *cfg)`   | `esp_err_t tinyusb_cdcacm_init(const tinyusb_config_cdcacm_t *cfg)`   |
| `esp_err_t tusb_cdc_acm_deinit(int itf)`                            | `esp_err_t tinyusb_cdcacm_deinit(int itf)`                            |
| `bool tusb_cdc_acm_initialized(tinyusb_cdcacm_itf_t itf)`           | `bool tinyusb_cdcacm_initialized(tinyusb_cdcacm_itf_t itf)`           |

Replace any usage of the old function names with the new ones in your codebase.

## 4. Common Migration Errors and Solutions

**Possible error**

```bash
"This header file is deprecated since v2.0.0.  Use 'tinyusb_cdc_acm.h' for the new development."
```

**How to fix?**

New header file was introduced.
Update your code from:
```c
#include "tusb_cdc_acm.h"

int main(void) {
    tinyusb_config_cdcacm_t acm_cfg = { ... };
    tusb_cdc_acm_init(&acm_cfg);
    tusb_cdc_acm_deinit(TINYUSB_CDC_ACM_0);
}
```
to:
```c
#include "tinyusb_cdc_acm.h"

int main(void) {
    tinyusb_config_cdcacm_t acm_cfg = { ... };
    tinyusb_cdcacm_init(&acm_cfg);
    tinyusb_cdcacm_deinit(TINYUSB_CDC_ACM_0);
}
```
