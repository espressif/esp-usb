/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "soc/soc_caps.h"

#if SOC_USB_OTG_SUPPORTED
#include "tinyusb_msc.h"
#if (TINYUSB_MSC_BDL_SUPPORTED)

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "unity.h"
#include "esp_err.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "device_common.h"
#include "storage_common.h"
#include "test_msc_common.h"

static esp_err_t dummy_bdl_read(esp_blockdev_handle_t dev, uint8_t *dst, size_t dst_size,
                                uint64_t src_addr, size_t len)
{
    (void)dev;
    (void)dst;
    (void)dst_size;
    (void)src_addr;
    (void)len;
    return ESP_OK;
}

static esp_err_t dummy_bdl_write(esp_blockdev_handle_t dev, const uint8_t *src,
                                 uint64_t dst_addr, size_t len)
{
    (void)dev;
    (void)src;
    (void)dst_addr;
    (void)len;
    return ESP_OK;
}

static esp_err_t dummy_bdl_erase(esp_blockdev_handle_t dev, uint64_t start, size_t len)
{
    (void)dev;
    (void)start;
    (void)len;
    return ESP_OK;
}

static const esp_blockdev_ops_t s_dummy_bdl_ops = {
    .read = dummy_bdl_read,
    .write = dummy_bdl_write,
    .erase = dummy_bdl_erase,
};

static void init_fake_bdl(esp_blockdev_t *dev, size_t read_size, size_t write_size,
                          size_t erase_size, uint64_t disk_size, bool erase_before_write)
{
    memset(dev, 0, sizeof(*dev));
    dev->ops = &s_dummy_bdl_ops;
    dev->geometry.read_size = read_size;
    dev->geometry.write_size = write_size;
    dev->geometry.erase_size = erase_size;
    dev->geometry.disk_size = disk_size;
    dev->device_flags.erase_before_write = erase_before_write ? 1 : 0;
}

static esp_err_t try_new_storage_blockdev(esp_blockdev_t *dev, tinyusb_msc_storage_handle_t *storage_hdl)
{
    *storage_hdl = NULL;
    const tinyusb_msc_storage_config_t cfg = {
        .medium.blockdev = dev,
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
    };
    return tinyusb_msc_new_storage_blockdev(&cfg, storage_hdl);
}

/**
 * @brief Validate storage_blockdev_compute_msc_block_size() geometry checks
 * (see storage_blockdev.c) against a handful of representative fake devices:
 * a candidate rejected for read/write misalignment, one rejected for
 * erase misalignment, one rejected for disk_size not dividing evenly, and
 * one that is accepted.
 */
TEST_CASE("MSC: storage blockdev geometry validation", "[ci][msc][blockdev][geometry]")
{
    esp_blockdev_t dev;
    tinyusb_msc_storage_handle_t storage_hdl = NULL;

    // block_size floors to 512, which must then be a multiple of read/write size (3).
    init_fake_bdl(&dev, 3, 3, 0, 1536, false);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, try_new_storage_blockdev(&dev, &storage_hdl));
    TEST_ASSERT_NULL(storage_hdl);

    // erase_before_write requires block_size to also be a multiple of erase_size (300).
    init_fake_bdl(&dev, 1, 1, 300, 1536, true);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, try_new_storage_blockdev(&dev, &storage_hdl));
    TEST_ASSERT_NULL(storage_hdl);

    // disk_size (1000) is not an exact multiple of the derived block_size (512).
    init_fake_bdl(&dev, 512, 512, 512, 1000, false);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, try_new_storage_blockdev(&dev, &storage_hdl));
    TEST_ASSERT_NULL(storage_hdl);

    // 512-aligned geometry that evenly divides disk_size is accepted.
    init_fake_bdl(&dev, 512, 512, 512, 4096, false);
    TEST_ASSERT_EQUAL(ESP_OK, try_new_storage_blockdev(&dev, &storage_hdl));
    TEST_ASSERT_NOT_NULL(storage_hdl);

    uint32_t sector_count = 0, sector_size = 0;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_msc_get_storage_capacity(storage_hdl, &sector_count));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_msc_get_storage_sector_size(storage_hdl, &sector_size));
    TEST_ASSERT_EQUAL(8, sector_count);
    TEST_ASSERT_EQUAL(512, sector_size);

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_msc_delete_storage(storage_hdl));
}

/**
 * @brief Blockdev counterpart of "MSC: storage SPI Flash" in test_msc_storage.c.
 */
TEST_CASE("MSC: storage SPI Flash (blockdev) create and delete", "[ci][msc][blockdev][spiflash]")
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

#if (SOC_SDMMC_HOST_SUPPORTED)
/**
 * @brief Blockdev counterpart of "MSC: storage SD/MMC" in test_msc_storage.c.
 */
TEST_CASE("MSC: storage SD/MMC (blockdev) create and delete", "[msc][blockdev][sdmmc]")
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

/**
 * @brief Storage backed by a second, concurrent blockdev handle must be rejected -
 * only one blockdev-backed instance can be active at a time (singleton guard in
 * storage_blockdev.c), unlike the spiflash/sdmmc backends which each keep their
 * own independent global and can therefore run as dual storage simultaneously.
 */
TEST_CASE("MSC: storage blockdev rejects a second concurrent open", "[ci][msc][blockdev]")
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

    esp_blockdev_handle_t partition_bdl2, wl_bdl2;
    storage_init_blockdev_spiflash(&partition_bdl2, &wl_bdl2);

    tinyusb_msc_storage_handle_t storage_hdl2 = NULL;
    const tinyusb_msc_storage_config_t cfg2 = {
        .medium.blockdev = wl_bdl2,
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,
    };
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, tinyusb_msc_new_storage_blockdev(&cfg2, &storage_hdl2));
    TEST_ASSERT_NULL(storage_hdl2);

    TEST_ASSERT_EQUAL(ESP_OK, wl_bdl2->ops->release(wl_bdl2));
    TEST_ASSERT_EQUAL(ESP_OK, partition_bdl2->ops->release(partition_bdl2));

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_msc_delete_storage(storage_hdl));
    TEST_ASSERT_EQUAL(ESP_OK, wl_bdl->ops->release(wl_bdl));
    TEST_ASSERT_EQUAL(ESP_OK, partition_bdl->ops->release(partition_bdl));
}

/**
 * @brief Blockdev counterpart of "MSC: Verify filesystem access APP and USB" in
 * test_msc_filesystem.c: filesystem access through the blockdev-backed medium
 * follows the same APP/USB mutual-exclusivity rule as the existing spiflash
 * medium - it is never accessible via VFS and exposed to the USB host at the
 * same time, only one or the other depending on the current mount point.
 */
TEST_CASE("MSC: Verify filesystem access APP and USB (blockdev)", "[storage][blockdev][fs]")
{
    tinyusb_msc_driver_config_t driver_cfg = {
        .callback = test_storage_event_cb,
        .callback_arg = NULL,
    };
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_install_driver(&driver_cfg), "Failed to install TinyUSB MSC driver");

    esp_blockdev_handle_t partition_bdl, wl_bdl;
    storage_init_blockdev_spiflash(&partition_bdl, &wl_bdl);

    tinyusb_msc_storage_handle_t storage_hdl;
    tinyusb_msc_storage_config_t storage_cfg = {
        .medium = {
            .blockdev = wl_bdl,
        },
        .fat_fs = {
            .config = {
                .max_files = 4,
            },
            .do_not_format = false,
            .format_flags = FM_ANY,
            .base_path = "/msc_bdl_test",
        },
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP, // Initially mount to APP
    };
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_new_storage_blockdev(&storage_cfg, &storage_hdl), "Failed to create new MSC storage on blockdev");
    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_START);
    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_COMPLETE);

    // Mounted to APP: accessible via VFS.
    const char *file_path = "/msc_bdl_test/hello.txt";
    FILE *f = fopen(file_path, "w");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "Failed to open file for writing on blockdev MSC storage");
    const char *text = "Hello, TinyUSB MSC blockdev!";
    size_t written = fwrite(text, 1, strlen(text), f);
    TEST_ASSERT_EQUAL_MESSAGE(strlen(text), written, "Failed to write the complete text to the file");
    fclose(f);

    f = fopen(file_path, "r");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "Failed to open file for reading on blockdev MSC storage");
    char read_buf[64] = {0};
    size_t read = fread(read_buf, 1, sizeof(read_buf) - 1, f);
    TEST_ASSERT_EQUAL_MESSAGE(written, read, "Failed to read the complete text from the file");
    fclose(f);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(text, read_buf, "Read content does not match written content");

    // Mount the storage to USB: no longer accessible via VFS.
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_set_storage_mount_point(storage_hdl, TINYUSB_MSC_STORAGE_MOUNT_USB), "Failed to set storage mount point to USB");
    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_START);
    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_COMPLETE);

    f = fopen(file_path, "r");
    TEST_ASSERT_NULL_MESSAGE(f, "File should not be accessible when storage is mounted to USB");

    // Install TinyUSB driver so the USB host can see it exposed as MSC.
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(test_device_event_handler);
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));

    test_device_wait();

    vTaskDelay(pdMS_TO_TICKS(TEST_DEVICE_PRESENCE_TIMEOUT_MS));

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_delete_storage(storage_hdl), "Failed to delete TinyUSB MSC storage");
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_uninstall_driver(), "Failed to uninstall TinyUSB MSC driver");

    TEST_ASSERT_EQUAL(ESP_OK, wl_bdl->ops->release(wl_bdl));
    TEST_ASSERT_EQUAL(ESP_OK, partition_bdl->ops->release(partition_bdl));
}

#if (SOC_SDMMC_HOST_SUPPORTED)
/**
 * @brief Mixed-backend counterpart of "MSC: dual storage SPIFLASH + SDMMC" in
 * test_msc_storage.c: LUN0 stays on the existing direct SPI flash path
 * (tinyusb_msc_new_storage_spiflash), LUN1 is backed by a blockdev handle
 * over the SD/MMC card (tinyusb_msc_new_storage_blockdev). This sidesteps
 * the blockdev singleton guard (only one blockdev-backed LUN at a time) by
 * keeping the SPI flash LUN on its own, independent direct backend.
 */
TEST_CASE("MSC: dual storage SPIFLASH (direct) + SD/MMC (blockdev)", "[storage][blockdev][spiflash][sdmmc][dual]")
{
    wl_handle_t wl_handle = WL_INVALID_HANDLE;
    storage_init_spiflash(&wl_handle);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(WL_INVALID_HANDLE, wl_handle, "Wear leveling handle is invalid, check the partition configuration");

    sdmmc_card_t *card = NULL;
    storage_init_sdmmc(&card);
    TEST_ASSERT_NOT_NULL_MESSAGE(card, "SDMMC card handle is NULL, check the SDMMC configuration");

    esp_blockdev_handle_t card_bdl = ESP_BLOCKDEV_HANDLE_INVALID;
    storage_get_blockdev_sdmmc(card, &card_bdl);

    tinyusb_msc_driver_config_t driver_cfg = {
        .callback = test_storage_event_cb,                  // Register the callback for mount changed events
        .callback_arg = NULL,                               // No additional argument for the callback
    };
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_install_driver(&driver_cfg), "Failed to install TinyUSB MSC driver");

    // LUN0: SPI flash, direct (non-BDL) backend
    tinyusb_msc_storage_config_t config = {
        .medium.wl_handle = wl_handle,
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,       // Initial mount point to APP
        .fat_fs = {
            .base_path = "/custom1",
            .config.max_files = 5,
            .format_flags = 0,
        },
    };
    tinyusb_msc_storage_handle_t storage1_hdl = NULL;
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_new_storage_spiflash(&config, &storage1_hdl), "Failed to initialize LUN0 (SPI flash, direct)");
    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_START);
    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_COMPLETE);

    // LUN1: SD/MMC, blockdev-backed
    tinyusb_msc_storage_config_t config_bdl = {
        .medium.blockdev = card_bdl,
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,
        .fat_fs = {
            .base_path = "/custom2",
            .config.max_files = 5,
            .format_flags = 0,
        },
    };
    tinyusb_msc_storage_handle_t storage2_hdl = NULL;
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_new_storage_blockdev(&config_bdl, &storage2_hdl), "Failed to initialize LUN1 (SD/MMC, blockdev)");
    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_START);
    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_COMPLETE);

    // Install TinyUSB driver: host should now enumerate 2 LUNs
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(test_device_event_handler);
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));

    test_device_wait();

    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_START);
    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_COMPLETE);
    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_START);
    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_COMPLETE);

    vTaskDelay(pdMS_TO_TICKS(TEST_DEVICE_PRESENCE_TIMEOUT_MS)); // Allow some time for the device to be recognized
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
    // tinyusb_driver_uninstall() implicitly remounts both LUNs back to APP (tud_umount_cb);
    // drain those events so the storage event queue doesn't overflow below.
    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_START);
    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_COMPLETE);
    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_START);
    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_COMPLETE);

    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_delete_storage(storage2_hdl), "Failed to delete LUN1 (SD/MMC, blockdev)");
    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_START);
    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_COMPLETE);
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_delete_storage(storage1_hdl), "Failed to delete LUN0 (SPI flash, direct)");
    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_START);
    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_COMPLETE);
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_uninstall_driver(), "Failed to uninstall TinyUSB MSC driver");

    TEST_ASSERT_EQUAL(ESP_OK, card_bdl->ops->release(card_bdl));
    storage_deinit_spiflash(wl_handle);
    storage_deinit_sdmmc(card);
}

/**
 * @brief Mirror of the above with the backends swapped: LUN0 stays on the
 * direct SD/MMC path (tinyusb_msc_new_storage_sdmmc), LUN1 is backed by a
 * blockdev handle over the WL-mounted SPI flash partition
 * (tinyusb_msc_new_storage_blockdev). Confirms the blockdev singleton guard
 * is per-blockdev-instance, not tied to a specific medium type.
 */
TEST_CASE("MSC: dual storage SD/MMC (direct) + SPIFLASH (blockdev)", "[storage][blockdev][spiflash][sdmmc][dual]")
{
    sdmmc_card_t *card = NULL;
    storage_init_sdmmc(&card);
    TEST_ASSERT_NOT_NULL_MESSAGE(card, "SDMMC card handle is NULL, check the SDMMC configuration");

    esp_blockdev_handle_t partition_bdl, wl_bdl;
    storage_init_blockdev_spiflash(&partition_bdl, &wl_bdl);

    tinyusb_msc_driver_config_t driver_cfg = {
        .callback = test_storage_event_cb,                  // Register the callback for mount changed events
        .callback_arg = NULL,                               // No additional argument for the callback
    };
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_install_driver(&driver_cfg), "Failed to install TinyUSB MSC driver");

    // LUN0: SD/MMC, direct (non-BDL) backend
    tinyusb_msc_storage_config_t config = {
        .medium.card = card,
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,       // Initial mount point to APP
        .fat_fs = {
            .base_path = "/custom1",
            .config.max_files = 5,
            .format_flags = 0,
        },
    };
    tinyusb_msc_storage_handle_t storage1_hdl = NULL;
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_new_storage_sdmmc(&config, &storage1_hdl), "Failed to initialize LUN0 (SD/MMC, direct)");
    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_START);
    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_COMPLETE);

    // LUN1: SPI flash, blockdev-backed
    tinyusb_msc_storage_config_t config_bdl = {
        .medium.blockdev = wl_bdl,
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,
        .fat_fs = {
            .base_path = "/custom2",
            .config.max_files = 5,
            .format_flags = 0,
        },
    };
    tinyusb_msc_storage_handle_t storage2_hdl = NULL;
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_new_storage_blockdev(&config_bdl, &storage2_hdl), "Failed to initialize LUN1 (SPI flash, blockdev)");
    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_START);
    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_COMPLETE);

    // Install TinyUSB driver: host should now enumerate 2 LUNs
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(test_device_event_handler);
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));

    test_device_wait();

    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_START);
    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_COMPLETE);
    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_START);
    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_COMPLETE);

    vTaskDelay(pdMS_TO_TICKS(TEST_DEVICE_PRESENCE_TIMEOUT_MS)); // Allow some time for the device to be recognized
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());
    // tinyusb_driver_uninstall() implicitly remounts both LUNs back to APP (tud_umount_cb);
    // drain those events so the storage event queue doesn't overflow below.
    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_START);
    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_COMPLETE);
    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_START);
    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_COMPLETE);

    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_delete_storage(storage2_hdl), "Failed to delete LUN1 (SPI flash, blockdev)");
    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_START);
    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_COMPLETE);
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_delete_storage(storage1_hdl), "Failed to delete LUN0 (SD/MMC, direct)");
    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_START);
    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_COMPLETE);
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_uninstall_driver(), "Failed to uninstall TinyUSB MSC driver");

    TEST_ASSERT_EQUAL(ESP_OK, wl_bdl->ops->release(wl_bdl));
    TEST_ASSERT_EQUAL(ESP_OK, partition_bdl->ops->release(partition_bdl));
    storage_deinit_sdmmc(card);
}
#endif // SOC_SDMMC_HOST_SUPPORTED

#endif // TINYUSB_MSC_BDL_SUPPORTED
#endif // SOC_USB_OTG_SUPPORTED
