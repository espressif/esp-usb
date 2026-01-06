# Migration guide to Espressif's TinyUSB addition: Console

## 1. Do I need to migrate?

Update brings the consistency for the Espressif's TinyUSB addition extensions public API.

## 2. Changes Required After Migration

To simplify the migration process, compatibility with the previous API has been retained. During the first build, you will see the following warning:

```bash
"This header file is deprecated since v2.0.0.  Use 'tinyusb_console.h' for the new development."
```

The old API is still supported, but it is recommended to update your code, as the old API may be removed in future releases.

## 3. List of Changes

### API Updates

The following function prototypes have been updated:

| Old Function Prototype                            | New Function Prototype                           |
| ------------------------------------------------- | ------------------------------------------------ |
| `esp_err_t esp_tusb_init_console(int cdc_intf)`   | `esp_err_t tinyusb_console_init(int cdc_intf)`   |
| `esp_err_t esp_tusb_deinit_console(int cdc_intf)` | `esp_err_t tinyusb_console_deinit(int cdc_intf)` |

Update your code to use the new function names as shown above.

## 4. Common Migration Errors and Solutions

**Possible error**

```bash
"This header file is deprecated since v2.0.0.  Use 'tinyusb_console.h' for the new development."
```

**How to fix?**

New header file was introduced. Update your code from:

```c
#include "tusb_console.h"

int main(void) {
    esp_tusb_init_console(TINYUSB_CDC_ACM_0);
    esp_tusb_deinit_console(TINYUSB_CDC_ACM_0);
}
```

to:

```c
#include "tinyusb_console.h"

int main(void) {
    tinyusb_console_init(TINYUSB_CDC_ACM_0);
    tinyusb_console_deinit(TINYUSB_CDC_ACM_0);
}
```
