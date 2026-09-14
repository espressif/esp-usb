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

static void expect_blockdev_rejected(esp_blockdev_t *dev)
{
    tinyusb_msc_storage_handle_t storage_hdl = NULL;
    const tinyusb_msc_storage_config_t cfg = {
        .medium.blockdev = dev,
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
    };
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, tinyusb_msc_new_storage_blockdev(&cfg, &storage_hdl));
    TEST_ASSERT_NULL(storage_hdl);
}

TEST_CASE("MSC: storage blockdev rejects 512 floor not aligned to read/write", "[ci][msc][blockdev][geometry]")
{
    esp_blockdev_t dev;
    init_fake_bdl(&dev, 3, 3, 0, 1536, false);
    expect_blockdev_rejected(&dev);
}

TEST_CASE("MSC: storage blockdev rejects advertised size not aligned to erase", "[ci][msc][blockdev][geometry]")
{
    esp_blockdev_t dev;
    init_fake_bdl(&dev, 1, 1, 300, 1536, true);
    expect_blockdev_rejected(&dev);
}

TEST_CASE("MSC: storage blockdev rejects disk_size that is not a multiple of block size", "[ci][msc][blockdev][geometry]")
{
    esp_blockdev_t dev;
    init_fake_bdl(&dev, 512, 512, 512, 1000, false);
    expect_blockdev_rejected(&dev);
}

TEST_CASE("MSC: storage blockdev accepts geometry that is 512-aligned and divides disk_size", "[ci][msc][blockdev][geometry]")
{
    esp_blockdev_t dev;
    init_fake_bdl(&dev, 512, 512, 512, 4096, false);

    tinyusb_msc_storage_handle_t storage_hdl = NULL;
    const tinyusb_msc_storage_config_t cfg = {
        .medium.blockdev = &dev,
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
    };
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_msc_new_storage_blockdev(&cfg, &storage_hdl));
    TEST_ASSERT_NOT_NULL(storage_hdl);

    uint32_t sector_count = 0, sector_size = 0;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_msc_get_storage_capacity(storage_hdl, &sector_count));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_msc_get_storage_sector_size(storage_hdl, &sector_size));
    TEST_ASSERT_EQUAL(8, sector_count);
    TEST_ASSERT_EQUAL(512, sector_size);

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_msc_delete_storage(storage_hdl));
}

TEST_CASE("MSC: storage SPI Flash (blockdev) create and delete", "[ci][msc][blockdev]")
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

    // A second concurrent open must fail loudly rather than alias the same
    // handle across two LUNs (see the singleton guard in storage_blockdev.c).
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

#if (SOC_SDMMC_HOST_SUPPORTED)
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
 * @brief Basic filesystem write/read roundtrip through the generic
 * blockdev-backed storage medium (storage_blockdev.c mount/read/write),
 * mirroring the spiflash filesystem test in test_msc_filesystem.c.
 *
 * @note Single small write - does not specifically exercise or verify the
 * erase-before-write path in storage_blockdev_sector_write().
 */
TEST_CASE("MSC: storage blockdev SPI Flash filesystem read/write", "[ci][storage][blockdev][fs]")
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
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,
    };
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_new_storage_blockdev(&storage_cfg, &storage_hdl), "Failed to create new MSC storage on blockdev");
    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_START);
    test_storage_event_wait_callback(TINYUSB_MSC_EVENT_MOUNT_COMPLETE);

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

    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_delete_storage(storage_hdl), "Failed to delete TinyUSB MSC storage");
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, tinyusb_msc_uninstall_driver(), "Failed to uninstall TinyUSB MSC driver");

    TEST_ASSERT_EQUAL(ESP_OK, wl_bdl->ops->release(wl_bdl));
    TEST_ASSERT_EQUAL(ESP_OK, partition_bdl->ops->release(partition_bdl));
}

#endif // TINYUSB_MSC_BDL_SUPPORTED
#endif // SOC_USB_OTG_SUPPORTED
