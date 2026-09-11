/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "soc/soc_caps.h"

#if SOC_USB_OTG_SUPPORTED
#include "esp_idf_version.h"
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)

#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "esp_err.h"
#include "tinyusb_msc.h"
#include "storage_common.h"
#include "test_msc_common.h"

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
 * @brief Filesystem read/write through the generic blockdev-backed storage
 * medium, exercising the actual write path including erase-before-write.
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

#endif // ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#endif // SOC_USB_OTG_SUPPORTED
