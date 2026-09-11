# TinyUSB MSC Device — Generic esp_blockdev Storage Backend Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a third, generic MSC storage backend to `device/esp_tinyusb` (`storage_blockdev.c`) that implements the existing `storage_medium_t` vtable on top of any `esp_blockdev_handle_t` (ESP-IDF ≥ 6.0), exposed via a new public API `tinyusb_msc_new_storage_blockdev()`. The existing `storage_spiflash.c` / `storage_sdmmc.c` backends and their public factories stay untouched, forever, as the pre-6.0 / explicit-choice path.

**Architecture:** `storage_blockdev.c` borrows (does not own) an `esp_blockdev_handle_t` supplied by the caller (built via `esp_partition_get_blockdev()` + `wl_get_blockdev()` for SPI flash, or `sdmmc_get_blockdev()` for SD/MMC — both already ship in IDF ≥ 6.0). It translates `storage_medium_t.read/write(lba, offset, size, buf)` into byte-addressed `esp_blockdev_ops_t.read/write(dev, buf, addr, len)` calls, asserting alignment to `geometry.read_size`/`write_size` rather than silently doing partial-sector arithmetic. `tinyusb_msc.c` and its SCSI dispatch are **not modified** — the new backend is a drop-in vtable implementation, identical in shape to `storage_spiflash.c`/`storage_sdmmc.c`.

**Tech Stack:** ESP-IDF ≥ 6.0 (`esp_blockdev` component), C11, Unity (on-target tests), existing `test_apps/msc_storage` pytest-embedded harness.

---

## Before you start

- Read `device/esp_tinyusb/include_private/msc_storage.h` — this is the contract you're implementing.
- Read `device/esp_tinyusb/storage_sdmmc.c` — it's the closest existing analog (also whole-sector-only, also a single static borrowed handle). Model `storage_blockdev.c` after it.
- Reference: `components/esp_blockdev/include/esp_blockdev.h` in ESP-IDF ≥ 6.0 for `esp_blockdev_ops_t`, `esp_blockdev_geometry_t`, `esp_blockdev_handle_t`.
- Use `@superpowers:using-git-worktrees` to create an isolated worktree before starting, since this touches build files (`CMakeLists.txt`, `idf_component.yml`) that differ per-IDF-version and you'll want a clean `git status`.

---

### Task 1: Add the private header contract for the blockdev backend

**Files:**
- Create: `device/esp_tinyusb/include_private/storage_blockdev.h`

**Step 1: Write the header**

```c
/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "esp_blockdev.h"
#include "msc_storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Open a generic esp_blockdev-backed storage medium.
 *
 * The supplied handle is *borrowed*: this module does not call
 * `bdl_handle->ops->release()` on it, mirroring how storage_sdmmc.c treats
 * the sdmmc_card_t* it is given. The caller retains ownership of the
 * blockdev's lifecycle (created via esp_partition_get_blockdev() +
 * wl_get_blockdev(), or sdmmc_get_blockdev(), etc.) and must keep it valid
 * for as long as the MSC storage instance exists.
 *
 * @param[in] bdl_handle Block device handle. Must not be ESP_BLOCKDEV_HANDLE_INVALID.
 * @param[out] medium Output pointer to the storage medium vtable.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if bdl_handle is invalid or medium is NULL
 *      - ESP_ERR_NOT_SUPPORTED if geometry.read_size/write_size is 0 (i.e. no writable
 *        byte-aligned sector size can be derived)
 */
esp_err_t storage_blockdev_open_medium(esp_blockdev_handle_t bdl_handle, const storage_medium_t **medium);

#ifdef __cplusplus
}
#endif
```

**Step 2: Commit**

```bash
git add device/esp_tinyusb/include_private/storage_blockdev.h
git commit -m "feat(device/msc): add storage_blockdev private header contract"
```

---

### Task 2: Implement the blockdev backend — get_info / mount / unmount / close

Do these first because they have no alignment edge cases, so you can validate the borrowed-handle plumbing before tackling read/write.

**Files:**
- Create: `device/esp_tinyusb/storage_blockdev.c`

**Step 1: Write the skeleton with get_info, mount, unmount, close**

```c
/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "diskio_bdl.h"
#include "storage_blockdev.h"

static const char *TAG = "storage_blockdev";

static esp_blockdev_handle_t _bdl_handle = ESP_BLOCKDEV_HANDLE_INVALID; // Borrowed, not owned

static inline size_t storage_blockdev_get_sector_size(void)
{
    assert(_bdl_handle != ESP_BLOCKDEV_HANDLE_INVALID);
    // read_size and write_size must both divide the sector we use; on a
    // read-only device write_size is 0, so fall back to read_size only.
    size_t sector = _bdl_handle->geometry.read_size;
    if (_bdl_handle->geometry.write_size > sector) {
        sector = _bdl_handle->geometry.write_size;
    }
    return sector;
}

static esp_err_t storage_blockdev_mount(BYTE pdrv)
{
    assert(_bdl_handle != ESP_BLOCKDEV_HANDLE_INVALID);
    return ff_diskio_register_bdl(pdrv, _bdl_handle);
}

static esp_err_t storage_blockdev_unmount(void)
{
    assert(_bdl_handle != ESP_BLOCKDEV_HANDLE_INVALID);
    BYTE pdrv = ff_diskio_get_pdrv_bdl(_bdl_handle);
    if (pdrv == 0xff) {
        return ESP_ERR_INVALID_STATE;
    }
    ff_diskio_clear_pdrv_bdl(_bdl_handle);

    char drv[3] = {(char)('0' + pdrv), ':', 0};
    f_mount(0, drv, 0);
    ff_diskio_unregister(pdrv);

    return ESP_OK;
}

static esp_err_t storage_blockdev_get_info(storage_info_t *info)
{
    ESP_RETURN_ON_FALSE(info, ESP_ERR_INVALID_ARG, TAG, "Storage info pointer can't be NULL");
    assert(_bdl_handle != ESP_BLOCKDEV_HANDLE_INVALID);

    size_t sector_size = storage_blockdev_get_sector_size();
    ESP_RETURN_ON_FALSE(sector_size > 0, ESP_ERR_NOT_SUPPORTED, TAG, "Block device reports zero sector size");

    info->sector_size = (uint32_t)sector_size;
    info->total_sectors = (uint32_t)(_bdl_handle->geometry.disk_size / sector_size);
    return ESP_OK;
}

static void storage_blockdev_close(void)
{
    // Borrowed handle: do NOT call ops->release() here. Lifecycle belongs to
    // whoever created the handle and passed it to storage_blockdev_open_medium().
    _bdl_handle = ESP_BLOCKDEV_HANDLE_INVALID;
}

// Forward declarations, implemented in Task 3
static esp_err_t storage_blockdev_sector_read(uint32_t lba, uint32_t offset, size_t size, void *dest);
static esp_err_t storage_blockdev_sector_write(uint32_t lba, uint32_t offset, size_t size, const void *src);

// Constant struct of function pointers
const storage_medium_t blockdev_storage_medium = {
    .type = STORAGE_MEDIUM_TYPE_BLOCKDEV,
    .mount = &storage_blockdev_mount,
    .unmount = &storage_blockdev_unmount,
    .read = &storage_blockdev_sector_read,
    .write = &storage_blockdev_sector_write,
    .get_info = &storage_blockdev_get_info,
    .close = &storage_blockdev_close,
};

esp_err_t storage_blockdev_open_medium(esp_blockdev_handle_t bdl_handle, const storage_medium_t **medium)
{
    ESP_RETURN_ON_FALSE(bdl_handle != ESP_BLOCKDEV_HANDLE_INVALID, ESP_ERR_INVALID_ARG, TAG, "Invalid block device handle");
    ESP_RETURN_ON_FALSE(medium != NULL, ESP_ERR_INVALID_ARG, TAG, "Storage API pointer can't be NULL");
    ESP_RETURN_ON_FALSE(bdl_handle->ops != NULL && bdl_handle->ops->read != NULL && bdl_handle->ops->write != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "Block device must support read and write ops");

    _bdl_handle = bdl_handle;
    *medium = &blockdev_storage_medium;

    return ESP_OK;
}
```

**Step 2: Add `STORAGE_MEDIUM_TYPE_BLOCKDEV` to the shared enum**

Modify `device/esp_tinyusb/include_private/msc_storage.h:25-28`:

```c
typedef enum {
    STORAGE_MEDIUM_TYPE_SPIFLASH = 0, /*!< Storage type is SPI flash with wear leveling. */
    STORAGE_MEDIUM_TYPE_SDMMC,        /*!< Storage type is SDMMC card. */
    STORAGE_MEDIUM_TYPE_BLOCKDEV,     /*!< Storage type is a generic esp_blockdev handle (IDF >= 6.0). */
} storage_medium_type_t;
```

**Step 3: Commit (read/write left as forward decls, won't compile standalone yet — that's fine, next task adds them to the same file before it's added to CMakeLists)**

```bash
git add device/esp_tinyusb/storage_blockdev.c device/esp_tinyusb/include_private/msc_storage.h
git commit -m "feat(device/msc): scaffold storage_blockdev mount/info/close"
```

---

### Task 3: Implement blockdev read/write with alignment guards

This is the part that differs most from `storage_spiflash.c`/`storage_sdmmc.c`: `esp_blockdev_ops_t.read/write` require the byte address and length to be aligned to `geometry.read_size`/`write_size`. TinyUSB's `tud_msc_read10_cb`/`write10_cb` always pass whole-sector-aligned requests in practice (mirrors what `storage_sdmmc.c` already assumes by ignoring `offset`), so the guard should **fail loudly** rather than attempt a bounce-buffer — silently corrupting data on a real misalignment would be worse than an explicit `ESP_ERR_INVALID_ARG`.

**Files:**
- Modify: `device/esp_tinyusb/storage_blockdev.c` (replace the two forward declarations with real implementations)

**Step 1: Replace the forward declarations with implementations**

```c
static esp_err_t storage_blockdev_sector_read(uint32_t lba, uint32_t offset, size_t size, void *dest)
{
    assert(_bdl_handle != ESP_BLOCKDEV_HANDLE_INVALID);
    ESP_RETURN_ON_FALSE(_bdl_handle->ops->read != NULL, ESP_ERR_NOT_SUPPORTED, TAG, "Block device does not support read");

    size_t sector_size = storage_blockdev_get_sector_size();
    uint64_t addr = (uint64_t)lba * sector_size + offset;

    ESP_RETURN_ON_FALSE(_bdl_handle->geometry.read_size > 0, ESP_ERR_NOT_SUPPORTED, TAG, "Block device has zero read_size");
    ESP_RETURN_ON_FALSE((addr % _bdl_handle->geometry.read_size) == 0, ESP_ERR_INVALID_ARG, TAG,
                        "Read address 0x%llx not aligned to read_size %u", addr, (unsigned)_bdl_handle->geometry.read_size);
    ESP_RETURN_ON_FALSE((size % _bdl_handle->geometry.read_size) == 0, ESP_ERR_INVALID_ARG, TAG,
                        "Read size %u not aligned to read_size %u", (unsigned)size, (unsigned)_bdl_handle->geometry.read_size);

    return _bdl_handle->ops->read(_bdl_handle, (uint8_t *)dest, size, addr, size);
}

static esp_err_t storage_blockdev_sector_write(uint32_t lba, uint32_t offset, size_t size, const void *src)
{
    assert(_bdl_handle != ESP_BLOCKDEV_HANDLE_INVALID);
    ESP_RETURN_ON_FALSE(_bdl_handle->ops->write != NULL, ESP_ERR_NOT_SUPPORTED, TAG, "Block device is read-only");

    size_t sector_size = storage_blockdev_get_sector_size();
    uint64_t addr = (uint64_t)lba * sector_size + offset;

    ESP_RETURN_ON_FALSE(_bdl_handle->geometry.write_size > 0, ESP_ERR_NOT_SUPPORTED, TAG, "Block device is read-only (write_size == 0)");
    ESP_RETURN_ON_FALSE((addr % _bdl_handle->geometry.write_size) == 0, ESP_ERR_INVALID_ARG, TAG,
                        "Write address 0x%llx not aligned to write_size %u", addr, (unsigned)_bdl_handle->geometry.write_size);
    ESP_RETURN_ON_FALSE((size % _bdl_handle->geometry.write_size) == 0, ESP_ERR_INVALID_ARG, TAG,
                        "Write size %u not aligned to write_size %u", (unsigned)size, (unsigned)_bdl_handle->geometry.write_size);

    // NOTE: erase-before-write (device_flags.erase_before_write / and_type_write) is expected
    // to be handled transparently by the underlying blockdev stack (e.g. wl_get_blockdev's
    // write already does erase-on-write internally, same as wl_write() does today in
    // storage_spiflash.c). This adapter does not issue its own erase() call. If verification
    // during Task 6 testing shows raw partition/NAND blockdevs need an explicit erase from
    // this layer, add it here guarded by `_bdl_handle->device_flags.erase_before_write`.
    return _bdl_handle->ops->write(_bdl_handle, (const uint8_t *)src, addr, size);
}
```

**Step 2: Verify it compiles in isolation**

Run: `xtensa-esp32s3-elf-gcc -fsyntax-only -c device/esp_tinyusb/storage_blockdev.c -I device/esp_tinyusb/include_private -I device/esp_tinyusb/include` (adjust include paths/toolchain as available; the real compile check happens once wired into CMakeLists in Task 5 — a plain `idf.py build` on the test app is the authoritative check).

**Step 3: Commit**

```bash
git add device/esp_tinyusb/storage_blockdev.c
git commit -m "feat(device/msc): implement aligned read/write for storage_blockdev"
```

---

### Task 4: Public API — config union member + new factory function

**Files:**
- Modify: `device/esp_tinyusb/include/tinyusb_msc.h`
- Modify: `device/esp_tinyusb/tinyusb_msc.c`

**Step 1: Add the IDF-version feature gate near the top of `tinyusb_msc.h`**

After `include/tinyusb_msc.h:20` (`#endif // SOC_SDMMC_HOST_SUPPORTED`), add:

```c
#include "esp_idf_version.h"
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#define TINYUSB_MSC_BDL_SUPPORTED 1
#include "esp_blockdev.h"
#endif // ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
```

**Step 2: Extend the config union at `tinyusb_msc.h:86-95`**

```c
typedef struct {
    union {
        wl_handle_t wl_handle;              /*!< Wear levelling handle for SPI flash storage. */
#if (SOC_SDMMC_HOST_SUPPORTED)
        sdmmc_card_t *card;                 /*!< SD/MMC card descriptor. */
#endif // SOC_SDMMC_HOST_SUPPORTED
#if (TINYUSB_MSC_BDL_SUPPORTED)
        esp_blockdev_handle_t blockdev;      /*!< Generic block device handle (IDF >= 6.0). */
#endif // TINYUSB_MSC_BDL_SUPPORTED
    } medium;                               /*!< Storage medium selector. */
    tinyusb_msc_fatfs_config_t fat_fs;      /*!< FAT filesystem configuration. */
    tinyusb_msc_mount_point_t mount_point;  /*!< Requested initial storage owner after creation. */
} tinyusb_msc_storage_config_t;
```

**Step 3: Add the new factory declaration after `tinyusb_msc_new_storage_sdmmc()` (around `tinyusb_msc.h:170`)**

```c
#if (TINYUSB_MSC_BDL_SUPPORTED)
/**
 * @brief Create a TinyUSB MSC storage instance backed by a generic esp_blockdev handle.
 *
 * This is the recommended entry point on ESP-IDF >= 6.0 for any storage medium exposed
 * as an esp_blockdev (SPI flash via esp_partition_get_blockdev()+wl_get_blockdev(),
 * SD/MMC via sdmmc_get_blockdev(), or any future medium IDF ships a blockdev for —
 * eMMC, external NAND, etc. — with zero additional code in this component).
 *
 * @note The handle is borrowed: this component does not release it. The caller must
 *       keep it valid for the lifetime of the storage instance and release it (via
 *       `handle->ops->release(handle)`) only after calling tinyusb_msc_delete_storage().
 *
 * @param[in] config Storage configuration. Must not be NULL.
 * @param[out] handle Optional output for the created storage handle.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if `config` is NULL or `config->medium.blockdev` is invalid
 *      - ESP_ERR_NO_MEM if memory allocation fails
 *      - ESP_FAIL if the storage cannot be mapped to a LUN
 *      - Other error codes from driver installation, storage medium setup, or filesystem mounting
 */
esp_err_t tinyusb_msc_new_storage_blockdev(const tinyusb_msc_storage_config_t *config,
                                           tinyusb_msc_storage_handle_t *handle);
#endif // TINYUSB_MSC_BDL_SUPPORTED
```

**Step 4: Implement the factory in `tinyusb_msc.c`, mirroring `tinyusb_msc_new_storage_sdmmc()` (add after line 984, before `tinyusb_msc_delete_storage`)**

First add the include near the top (after line 29's `#endif // SOC_SDMMC_HOST_SUPPORTED`):

```c
#if (TINYUSB_MSC_BDL_SUPPORTED)
#include "storage_blockdev.h"
#endif // TINYUSB_MSC_BDL_SUPPORTED
```

Then the factory:

```c
#if (TINYUSB_MSC_BDL_SUPPORTED)
esp_err_t tinyusb_msc_new_storage_blockdev(const tinyusb_msc_storage_config_t *config,
                                           tinyusb_msc_storage_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "Config can't be NULL");
    ESP_RETURN_ON_FALSE(config->medium.blockdev != ESP_BLOCKDEV_HANDLE_INVALID, ESP_ERR_INVALID_ARG, TAG, "Block device handle should be valid");

    bool need_to_install_driver = false;
    const storage_medium_t *medium = NULL;
    msc_storage_obj_t *storage = NULL;
    esp_err_t ret;

    MSC_ENTER_CRITICAL();
    if (p_msc_driver == NULL) {
        need_to_install_driver = true;
    }
    MSC_EXIT_CRITICAL();

    if (need_to_install_driver) {
        tinyusb_msc_driver_config_t default_cfg = {
            .callback = msc_storage_event_default_cb,
        };
        ret = msc_driver_install(&default_cfg, true);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to install MSC driver");
            goto driver_err;
        }
    }

    ret = storage_blockdev_open_medium(config->medium.blockdev, &medium);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open block device medium");
        goto medium_err;
    }
    ret = msc_storage_new(config, medium, &storage);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create MSC storage object");
        goto storage_err;
    }
    MSC_ENTER_CRITICAL();
    if (!_msc_storage_map_to_lun(storage)) {
        MSC_EXIT_CRITICAL();
        ESP_LOGE(TAG, "Failed to map storage to LUN");
        ret = ESP_FAIL;
        goto map_err;
    }
    MSC_EXIT_CRITICAL();

    if (config->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP) {
        ret = msc_storage_mount(storage);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to mount storage to application");
            goto map_err;
        }
    }

    if (handle != NULL) {
        *handle = (tinyusb_msc_storage_handle_t)storage;
    }
    return ESP_OK;

map_err:
    msc_storage_delete(storage);
storage_err:
    medium->close();
medium_err:
    if (need_to_install_driver) {
        tinyusb_msc_uninstall_driver();
    }
driver_err:
    return ret;
}
#endif // TINYUSB_MSC_BDL_SUPPORTED
```

**Step 5: Commit**

```bash
git add device/esp_tinyusb/include/tinyusb_msc.h device/esp_tinyusb/tinyusb_msc.c
git commit -m "feat(device/msc): add tinyusb_msc_new_storage_blockdev public API"
```

---

### Task 5: Wire into CMakeLists.txt and idf_component.yml

**Files:**
- Modify: `device/esp_tinyusb/CMakeLists.txt:38-48`
- Modify: `device/esp_tinyusb/idf_component.yml`

**Step 1: Update the MSC sources block in CMakeLists.txt**

```cmake
if(CONFIG_TINYUSB_MSC_ENABLED)
    list(APPEND srcs
        "tinyusb_msc.c"
        "storage_spiflash.c"
        )
    if(CONFIG_SOC_SDMMC_HOST_SUPPORTED)
        list(APPEND srcs
            "storage_sdmmc.c"
            )
    endif() # CONFIG_SOC_SDMMC_HOST_SUPPORTED
    if(IDF_VERSION VERSION_GREATER_EQUAL "6.0.0")
        list(APPEND srcs
            "storage_blockdev.c"
            )
        list(APPEND priv_req esp_blockdev)
    endif() # IDF_VERSION >= 6.0.0
endif() # CONFIG_TINYUSB_MSC_ENABLED
```

Place this block where the existing MSC block is (replacing lines 38-48); `priv_req` is already declared earlier in the file (line 14) so `list(APPEND priv_req esp_blockdev)` is valid at this point.

**Step 2: Note the new optional dependency in `idf_component.yml`**

No `dependencies:` entry is needed for `esp_blockdev` since it's an IDF built-in component (not a managed component) referenced only via `PRIV_REQUIRES` in CMake — same pattern as `fatfs`/`vfs`/`wear_levelling` today. Just bump the version:

```yaml
version: "2.3.0"
```

(Change from `"2.2.1"` — minor bump since this is purely additive.)

**Step 3: Build the existing test app to confirm nothing broke on IDF < 6.0 and on IDF >= 6.0**

Run: `cd device/esp_tinyusb/test_apps/msc_storage && idf.py set-target esp32s3 && idf.py build`
Expected: Build succeeds; on IDF >= 6.0 toolchain, `storage_blockdev.c` is compiled (check `idf.py build` output lists it); on IDF < 6.0 it's absent.

**Step 4: Commit**

```bash
git add device/esp_tinyusb/CMakeLists.txt device/esp_tinyusb/idf_component.yml
git commit -m "build(device/msc): wire storage_blockdev.c for IDF >= 6.0.0"
```

---

### Task 6: On-target test — round-trip via WL-partition blockdev

Mirrors the existing `storage_init_spiflash()`/`test_msc_storage.c` pattern but builds the medium through `esp_partition_get_blockdev()` + `wl_get_blockdev()` instead of `wl_mount()`, and creates the MSC storage through the new `tinyusb_msc_new_storage_blockdev()` entry point.

**Files:**
- Modify: `device/esp_tinyusb/test_apps/msc_storage/main/storage_common.h`
- Modify: `device/esp_tinyusb/test_apps/msc_storage/main/storage_common.c`
- Create: `device/esp_tinyusb/test_apps/msc_storage/main/test_msc_storage_blockdev.c`
- Modify: `device/esp_tinyusb/test_apps/msc_storage/main/CMakeLists.txt`

**Step 1: Add `storage_init_blockdev_spiflash()` helper to storage_common.h**

```c
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#include "esp_blockdev.h"

/**
 * @brief Build an esp_blockdev handle over the wear-levelled "storage" partition.
 *
 * Chains esp_partition_get_blockdev() -> wl_get_blockdev(). Caller owns both the
 * partition-level and WL-level handles and must release them (in that order,
 * WL first) via `handle->ops->release(handle)` after the MSC storage using this
 * handle has been deleted with tinyusb_msc_delete_storage().
 *
 * @param[out] partition_bdl Partition-level blockdev handle (release last).
 * @param[out] wl_bdl WL-level blockdev handle (release first, this is what you
 *             pass to tinyusb_msc_new_storage_blockdev()).
 */
void storage_init_blockdev_spiflash(esp_blockdev_handle_t *partition_bdl, esp_blockdev_handle_t *wl_bdl);
#endif // ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
```

**Step 2: Implement it in storage_common.c**

```c
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
void storage_init_blockdev_spiflash(esp_blockdev_handle_t *partition_bdl, esp_blockdev_handle_t *wl_bdl)
{
    esp_blockdev_handle_t part = ESP_BLOCKDEV_HANDLE_INVALID;
    esp_blockdev_handle_t wl = ESP_BLOCKDEV_HANDLE_INVALID;

    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK,
        esp_partition_get_blockdev(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "storage", &part),
        "Failed to get partition blockdev for storage partition");
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, wl_get_blockdev(part, &wl), "Failed to wrap partition blockdev with wear levelling");

    printf("Blockdev (WL over SPI flash) initialized successfully\n");
    printf("\tDisk size: %llu bytes\n", wl->geometry.disk_size);
    printf("\tRead size: %u, Write size: %u\n", (unsigned)wl->geometry.read_size, (unsigned)wl->geometry.write_size);

    *partition_bdl = part;
    *wl_bdl = wl;
}
#endif // ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
```

**Step 3: Write the new test file, mirroring an existing spiflash round-trip test from `test_msc_storage.c`**

First, read `test_msc_storage.c` to copy its exact round-trip test structure/assertions before writing this file — do not invent a different test shape. Example skeleton to adapt:

```c
/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "soc/soc_caps.h"

#if SOC_USB_OTG_SUPPORTED
#include "esp_idf_version.h"
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)

#include "unity.h"
#include "esp_err.h"
#include "tinyusb_msc.h"
#include "storage_common.h"
#include "test_msc_common.h"

TEST_CASE("msc storage blockdev spiflash create and delete", "[msc][blockdev]")
{
    esp_blockdev_handle_t partition_bdl, wl_bdl;
    storage_init_blockdev_spiflash(&partition_bdl, &wl_bdl);

    tinyusb_msc_storage_handle_t storage_hdl = NULL;
    const tinyusb_msc_storage_config_t cfg = {
        .medium.blockdev = wl_bdl,
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,
    };
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_msc_new_storage_blockdev(&cfg, &storage_hdl));
    TEST_ASSERT_NOT_NULL(storage_hdl);

    uint32_t sector_count = 0, sector_size = 0;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_msc_get_storage_capacity(storage_hdl, &sector_count));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_msc_get_storage_sector_size(storage_hdl, &sector_size));
    TEST_ASSERT_GREATER_THAN(0, sector_count);
    TEST_ASSERT_GREATER_THAN(0, sector_size);

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_msc_delete_storage(storage_hdl));

    // Release WL handle first, then partition handle (WL wraps partition)
    TEST_ASSERT_EQUAL(ESP_OK, wl_bdl->ops->release(wl_bdl));
    TEST_ASSERT_EQUAL(ESP_OK, partition_bdl->ops->release(partition_bdl));
}

// TODO: add a filesystem read/write round-trip test mirroring
// test_msc_storage.c's existing spiflash filesystem test, using
// tinyusb_msc_new_storage_blockdev() instead of tinyusb_msc_new_storage_spiflash().
// Write this by literally copying the existing spiflash filesystem test case body
// and swapping only the storage creation call + init helper.

#endif // ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#endif // SOC_USB_OTG_SUPPORTED
```

**Step 4: Add the new file to the test app's CMakeLists.txt**

`SRC_DIRS .` already picks up all `.c` files in `main/`, so no CMakeLists change is needed — verify this is true by checking `idf.py build` picks up the new file (it uses `SRC_DIRS .`, confirmed in current `CMakeLists.txt`).

**Step 5: Run the test**

Run: `cd device/esp_tinyusb/test_apps/msc_storage && idf.py -p <PORT> flash monitor` (or via the project's pytest-embedded harness: `pytest pytest_msc_storage.py --target esp32s3`)
Expected: `msc storage blockdev spiflash create and delete` passes; capacity/sector_size match what the equivalent `storage_spiflash` test reports for the same partition.

**Step 6: Commit**

```bash
git add device/esp_tinyusb/test_apps/msc_storage/main/storage_common.h \
        device/esp_tinyusb/test_apps/msc_storage/main/storage_common.c \
        device/esp_tinyusb/test_apps/msc_storage/main/test_msc_storage_blockdev.c
git commit -m "test(device/msc): add blockdev-backed SPI flash round-trip test"
```

---

### Task 7: On-target test — round-trip via SDMMC blockdev

Same shape as Task 6, but using `sdmmc_get_blockdev()` directly (no WL layer — SD cards are managed FAT-friendly already).

**Files:**
- Modify: `device/esp_tinyusb/test_apps/msc_storage/main/storage_common.h`
- Modify: `device/esp_tinyusb/test_apps/msc_storage/main/storage_common.c`
- Modify: `device/esp_tinyusb/test_apps/msc_storage/main/test_msc_storage_blockdev.c`

**Step 1: Add `storage_get_blockdev_sdmmc()` next to the existing `storage_init_sdmmc()` in storage_common.h/.c**

```c
#if (SOC_SDMMC_HOST_SUPPORTED) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
/**
 * @brief Wrap an already-initialized sdmmc_card_t as an esp_blockdev handle.
 *
 * @param[in] card Card previously initialized by storage_init_sdmmc().
 * @param[out] card_bdl Output blockdev handle; release with `card_bdl->ops->release(card_bdl)`.
 */
void storage_get_blockdev_sdmmc(sdmmc_card_t *card, esp_blockdev_handle_t *card_bdl);
#endif
```

```c
#if (SOC_SDMMC_HOST_SUPPORTED) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
void storage_get_blockdev_sdmmc(sdmmc_card_t *card, esp_blockdev_handle_t *card_bdl)
{
    esp_blockdev_handle_t bdl = ESP_BLOCKDEV_HANDLE_INVALID;
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, sdmmc_get_blockdev(card, &bdl), "Failed to get SDMMC blockdev");
    *card_bdl = bdl;
}
#endif
```

**Step 2: Add the test case to test_msc_storage_blockdev.c**

```c
#if (SOC_SDMMC_HOST_SUPPORTED)
TEST_CASE("msc storage blockdev sdmmc create and delete", "[msc][blockdev][sdmmc]")
{
    sdmmc_card_t *card = NULL;
    storage_init_sdmmc(&card);

    esp_blockdev_handle_t card_bdl = ESP_BLOCKDEV_HANDLE_INVALID;
    storage_get_blockdev_sdmmc(card, &card_bdl);

    tinyusb_msc_storage_handle_t storage_hdl = NULL;
    const tinyusb_msc_storage_config_t cfg = {
        .medium.blockdev = card_bdl,
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,
    };
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_msc_new_storage_blockdev(&cfg, &storage_hdl));
    TEST_ASSERT_NOT_NULL(storage_hdl);

    uint32_t sector_count = 0, sector_size = 0;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_msc_get_storage_capacity(storage_hdl, &sector_count));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_msc_get_storage_sector_size(storage_hdl, &sector_size));
    TEST_ASSERT_EQUAL(card->csd.capacity, sector_count);
    TEST_ASSERT_EQUAL(card->csd.sector_size, sector_size);

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_msc_delete_storage(storage_hdl));
    TEST_ASSERT_EQUAL(ESP_OK, card_bdl->ops->release(card_bdl));

    storage_deinit_sdmmc(card);
}
#endif // SOC_SDMMC_HOST_SUPPORTED
```

**Step 3: Run and confirm**

Run: `pytest pytest_msc_storage.py --target esp32p4 -k blockdev` (or whichever target in CI has SDMMC wired)
Expected: Both new test cases (`[blockdev]` tag) pass, capacity/sector_size exactly match the values reported by `card->csd`.

**Step 4: Commit**

```bash
git add device/esp_tinyusb/test_apps/msc_storage/main/storage_common.h \
        device/esp_tinyusb/test_apps/msc_storage/main/storage_common.c \
        device/esp_tinyusb/test_apps/msc_storage/main/test_msc_storage_blockdev.c
git commit -m "test(device/msc): add blockdev-backed SDMMC round-trip test"
```

---

### Task 8: sdkconfig.ci variant for the blockdev path — RESOLVED DIFFERENTLY (see below)

**Original premise was wrong for this repo.** Investigation found:

- This repo selects IDF versions via a **workflow-level CI matrix** (`.github/workflows/build_and_run_main_ci.yml`: `release-v5.3, v5.4, v5.5, v6.0, v6.1, latest`), not via `sdkconfig.ci.*` suffixes. Across all 48 existing `sdkconfig.ci*` files in the repo, the suffix convention is always a feature/target-revision axis (e.g. `esp32p4_eco4`, `pm`, `otg_wake`), **never** an IDF-version pin.
- The existing (empty/default) `sdkconfig.ci` is already built against IDF v6.0, v6.1, and `latest` by that matrix — combined with `storage_blockdev.c`'s own `ESP_IDF_VERSION >= 6.0.0` compile-time guard (Task 5), the blockdev path is **already exercised** on those legs with zero new CI config needed. Adding a dedicated `sdkconfig.ci.blockdev` would be a redundant duplicate build.

**The real gap found instead:** `pytest_msc_storage.py` runs `dut.run_all_single_board_cases(group=['ci'])`, which only executes Unity `TEST_CASE`s tagged `[ci]`. The new tests added in Tasks 6-7 were tagged `[msc][blockdev]`/`[msc][blockdev][sdmmc]` — no `[ci]` tag — so they would have compiled successfully but **never actually run** in CI.

**Fix applied:** tagged `"msc storage blockdev spiflash create and delete"` with `[ci]` (mirroring the existing SPI-flash tests, which don't need external hardware). Left the SDMMC blockdev test untagged, mirroring the existing untagged `"MSC: storage SD/MMC"` test (requires wired hardware, not part of the default CI run). Committed as `test(device/msc): tag blockdev spiflash test [ci] so it runs in the CI matrix`.

No `sdkconfig.ci.blockdev` file was created.

---

### Task 9: Documentation — README, CHANGELOG, usage example

**Files:**
- Modify: `device/esp_tinyusb/README.md`
- Modify: `device/esp_tinyusb/CHANGELOG.md`

**Step 1: Add a "Generic Block Device Storage" section to README.md, after the existing "SD-Card Storage" example (mirrors PR #567's README treatment on the host side)**

```markdown
**Generic Block Device Storage (IDF >= 6.0)**

On ESP-IDF 6.0 and later, any storage medium exposed as an `esp_blockdev_handle_t`
can back an MSC LUN directly — including SPI flash (via `esp_partition_get_blockdev()`
+ `wl_get_blockdev()`) and SD/MMC (via `sdmmc_get_blockdev()`), with the same code path
extending automatically to any future medium IDF ships a blockdev for (eMMC, external
NAND, etc.), with no changes required in this component.

```c
void main(void)
{
  esp_blockdev_handle_t partition_bdl, wl_bdl;
  esp_partition_get_blockdev(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "storage", &partition_bdl);
  wl_get_blockdev(partition_bdl, &wl_bdl);

  tinyusb_msc_storage_handle_t storage_hdl;
  const tinyusb_msc_storage_config_t cfg = {
    .medium.blockdev = wl_bdl,
  };
  tinyusb_msc_new_storage_blockdev(&cfg, &storage_hdl);

  // The block device handle is borrowed by esp_tinyusb; release it only
  // after tinyusb_msc_delete_storage() has been called:
  //   wl_bdl->ops->release(wl_bdl);
  //   partition_bdl->ops->release(partition_bdl);
}
```

> **Note:** `tinyusb_msc_new_storage_spiflash()` and `tinyusb_msc_new_storage_sdmmc()`
> remain available and unchanged on all supported IDF versions.
```

**Step 2: Add a CHANGELOG.md entry (check the file's existing format first and match it exactly)**

**Step 3: Commit**

```bash
git add device/esp_tinyusb/README.md device/esp_tinyusb/CHANGELOG.md
git commit -m "docs(device/msc): document tinyusb_msc_new_storage_blockdev usage"
```

---

## Explicitly out of scope (YAGNI — do not build these unless asked)

- **Deprecating `storage_spiflash.c`/`storage_sdmmc.c`.** Per design discussion, these stay forever as the pre-6.0 / explicit-choice path.
- **eMMC / external NAND example code.** Not needed — any medium with an IDF-shipped `esp_blockdev` factory works through `tinyusb_msc_new_storage_blockdev()` with zero new code here. Do not add speculative example backends for hardware you can't test.
- **Read-only medium support (`storage_info_t.readonly`).** Both SPI flash and SD/MMC are read-write in existing usage; don't add a field nobody's asked for. If a future medium needs it, that's a separate, small follow-up PR.
- **Bounce-buffer / sub-alignment read-write splitting.** TinyUSB never issues misaligned requests in practice; the alignment guard exists to fail loudly on a real bug, not to paper over a real use case.
- **Changing `tinyusb_msc.c`'s SCSI dispatch, LUN mapping, or mount state machine.** Not touched by this plan at all.

## Follow-up TODOs (not yet done, tracked here so they aren't lost before PR)

- **[ ] Documentation: "when to use blockdev vs legacy APIs" guidance.** The README section added in Task 9 shows *how* to call `tinyusb_msc_new_storage_blockdev()`, but not *when a user should prefer it* over the existing `tinyusb_msc_new_storage_spiflash()`/`_sdmmc()`. Nothing forces migration — the legacy APIs remain fully functional and unchanged on IDF 6.0+, this is purely opt-in. Add explicit guidance along the lines of: "prefer the blockdev API if you want forward-compatibility with future media types ESP-IDF adds `esp_blockdev` support for (eMMC, external NAND, etc.) without any esp_tinyusb code changes; otherwise the existing per-medium APIs remain the simplest choice."
- **[ ] IDF-side runnable example.** This repo's README points to `esp-idf/examples/peripherals/usb/device/` for full runnable examples — that's a separate repository (esp-idf), out of scope for this esp-usb PR, but should be filed as a follow-up issue/PR there once this lands, showing the full `esp_partition_get_blockdev()` → `wl_get_blockdev()` → `tinyusb_msc_new_storage_blockdev()` chain end to end.
- **[ ] RST/Doxygen docs (`docs/en/`).** This repo also has a separate Sphinx/Doxygen-based documentation system under `docs/` (see `docs/README.md`) distinct from the component-level `device/esp_tinyusb/README.md` we already updated. Check whether `tinyusb_msc.h` is already included in the Doxygen `INPUT` paths there (it should auto-pick-up the new `tinyusb_msc_new_storage_blockdev()` Doxygen comment if so) and whether any narrative `.rst` page needs a manual update to mention the new API.
- **[ ] Test coverage gap: singleton guard (Fix 4) is untested.** No test currently calls `storage_blockdev_open_medium()` twice to confirm `ESP_ERR_INVALID_STATE` actually fires on a concurrent second open (e.g. SPI-flash-blockdev + SDMMC-blockdev simultaneously). Worth a small dedicated test case.
- **[ ] Design decision needed: lift the single-instance limitation (multi-LUN blockdev support)?** Currently `storage_blockdev.c` supports exactly one active `esp_blockdev_handle_t` at a time (see the singleton guard added in Fix 4 — it now fails loudly with `ESP_ERR_INVALID_STATE` on a second concurrent open, rather than silently corrupting both LUNs). The *real* fix, if this limitation turns out to matter for real users, would be giving each blockdev-backed storage instance its own private context instead of one shared `static esp_blockdev_handle_t _bdl_handle`. Concretely this means:
  - Changing `storage_medium_t` (in `msc_storage.h`) from a single shared vtable with module-static state to something that carries a `void *ctx` (or per-instance vtable instances) that `tinyusb_msc.c`'s `msc_storage_obj_t` passes through on every call.
  - This is a bigger, more invasive refactor than anything else in this PR — it also touches `storage_spiflash.c`/`storage_sdmmc.c`'s calling convention (or at least the abstraction they implement), not just `storage_blockdev.c`.
  - Not decided whether this is worth doing now vs. as a separate follow-up PR once/if a real use case for simultaneous multi-LUN blockdev storage shows up (e.g. someone actually wanting SPI-flash-blockdev + SDMMC-blockdev, or two SD cards, active at once). Flagging here explicitly so the idea isn't lost, not committing to doing it.
- **[ ] Test coverage gap: blockdev backend has no USB-host-round-trip test.** `test_msc_filesystem.c`'s full USB-mount + real host enumeration test only exercises the legacy `storage_spiflash.c` path. The new `storage_blockdev.c` backend's SCSI-layer behavior (as seen by a real USB host, not just APP-mounted VFS) is unverified.

## Verification checklist before calling this done

- [ ] `idf.py build` succeeds for `test_apps/msc_storage` on an IDF < 6.0 toolchain (storage_blockdev.c absent, old behavior unchanged)
- [ ] `idf.py build` succeeds for `test_apps/msc_storage` on an IDF >= 6.0 toolchain (storage_blockdev.c present)
- [ ] New on-target tests pass (Tasks 6 & 7) on real hardware, both SPI flash and SDMMC
- [ ] Existing on-target tests (`test_msc_storage.c`, `test_msc_filesystem.c`, `test_msc_multitask.c`) still pass unmodified — this is a regression guard for the untouched legacy backends
- [ ] `lsp_diagnostics` clean on all modified/created files
- [ ] README/CHANGELOG updated and match existing doc style
