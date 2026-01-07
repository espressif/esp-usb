# Migration guide to Espressif's TinyUSB addition: NCM

## 1. Do I need to migrate?

Update brings the consistency for the Espressif's TinyUSB addition extensions public API.

## 2. Changes Required After Migration

Update the function prototype of the NCM driver initialization.

## 3. List of Changes

### API Updates

The following function prototypes have been updated:

| Old Function Prototype                                                                  | New Function Prototype                                        |
| --------------------------------------------------------------------------------------- | ------------------------------------------------------------- |
| `esp_err_t tinyusb_net_init(tinyusb_usbdev_t usb_dev, const tinyusb_net_config_t *cfg)` | `esp_err_t tinyusb_net_init(const tinyusb_net_config_t *cfg)` |

Update your code to use the new function names as shown above.

## 4. Common Migration Errors and Solutions

**Possible error**

```bash
error: 'TINYUSB_USBDEV_0' undeclared (first use in this function)
```

**How to fix?**

`TINYUSB_USBDEV_0` was removed. Please, update your code:

```c
    tinyusb_net_init(TINYUSB_USBDEV_0, &net_config);
```

to:

```c
    tinyusb_net_init(&net_config);
```

**Possible error**

```bash
error: too many arguments to function 'tinyusb_net_init'; expected 1, have 2
```

**How to fix?**

TINYUSB_USBDEV_0 was removed. Please, update your code:

```c
    tinyusb_net_init(TINYUSB_USBDEV_0, &net_config);
```

to:

```c
    tinyusb_net_init(&net_config);
```
