/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "soc/soc_caps.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_mtp.h"
#include "tinyusb_mtp_impl.h"
#include "tinyusb_mtp_test.h"
#include "unity.h"
#include "wear_levelling.h"

#define TEST_MTP_FLASH_BASE_PATH       "/mtp_flash"
#define TEST_MTP_SDCARD_BASE_PATH      "/mtp_sd"
#define TEST_MTP_NAND_BASE_PATH        "/mtp_nand"

static const char *TAG = "test_mtp_storage";

typedef struct {
    const char *base_path;
    const char *partition_label;
    const char *display_name;
    bool removable;
    wl_handle_t wl_handle;
} test_mtp_storage_t;

static test_mtp_storage_t s_test_storages[] = {
    { TEST_MTP_FLASH_BASE_PATH, "mtp_flash", "Flash FATFS", false, WL_INVALID_HANDLE },
    { TEST_MTP_SDCARD_BASE_PATH, "mtp_sd", "SDCard FATFS", true, WL_INVALID_HANDLE },
    { TEST_MTP_NAND_BASE_PATH, "mtp_nand", "NAND FATFS", false, WL_INVALID_HANDLE },
};
static bool s_mtp_storage_warmed_up;

#define TEST_MTP_STORAGE_COUNT         (sizeof(s_test_storages) / sizeof(s_test_storages[0]))

void test_mtp_storage_warm_up(void);

static const char *const s_test_artifacts[] = {
    "hello.txt",
    "empty.txt",
    "empty_replace.txt",
    "direct_edit.txt",
    "cross_edit.txt",
    "4G-usb.md",
    "4G-usb",
    "ascii_name.txt",
    "中文文件.txt",
    "probe.txt",
    "warmup.txt",
    "warmup_renamed.txt",
};

static void test_mtp_build_path(char *path, size_t path_size, const char *base_path, const char *name)
{
    int path_len = snprintf(path, path_size, "%s/%s", base_path, name);
    TEST_ASSERT_TRUE_MESSAGE(path_len > 0 && path_len < (int)path_size, "MTP test path was truncated");
}

static void test_mtp_mount_storage(test_mtp_storage_t *storage)
{
    if (storage->wl_handle != WL_INVALID_HANDLE) {
        return;
    }

    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 8,
        .allocation_unit_size = 4096,
    };
    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, esp_vfs_fat_spiflash_mount_rw_wl(storage->base_path, storage->partition_label, &mount_config, &storage->wl_handle),
                              "Failed to mount MTP test FATFS");
}

static void test_mtp_remove_path_if_exists(const char *path)
{
    if (unlink(path) == 0) {
        return;
    }

    int err = errno;
    if (err == ENOENT) {
        return;
    }

    if (err == EISDIR || err == EPERM) {
        if (rmdir(path) == 0) {
            return;
        }
        err = errno;
        if (err == ENOENT) {
            return;
        }
    }

    ESP_LOGE(TAG, "failed to remove MTP test path %s: errno=%d", path, err);
    TEST_FAIL_MESSAGE("Failed to remove MTP test path");
}

static void test_mtp_clean_storage(test_mtp_storage_t *storage)
{
    for (size_t i = 0; i < sizeof(s_test_artifacts) / sizeof(s_test_artifacts[0]); i++) {
        char path[128];
        test_mtp_build_path(path, sizeof(path), storage->base_path, s_test_artifacts[i]);
        test_mtp_remove_path_if_exists(path);
    }
}

static void test_mtp_clean_all_storages(void)
{
    for (size_t i = 0; i < TEST_MTP_STORAGE_COUNT; i++) {
        test_mtp_clean_storage(&s_test_storages[i]);
    }
}

static void test_mtp_clean_storage_path(test_mtp_storage_t *storage, const char *name)
{
    char path[128];
    test_mtp_build_path(path, sizeof(path), storage->base_path, name);
    test_mtp_remove_path_if_exists(path);
}

static void test_mtp_write_file(const char *path, const char *content)
{
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, "Failed to open MTP test file for write");
    TEST_ASSERT_EQUAL(strlen(content), fwrite(content, 1, strlen(content), file));
    TEST_ASSERT_EQUAL(0, fclose(file));
}

static void test_mtp_read_file(const char *path, char *buffer, size_t buffer_size)
{
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, "Failed to open MTP test file for read");
    size_t read_len = fread(buffer, 1, buffer_size - 1, file);
    TEST_ASSERT_EQUAL(0, fclose(file));
    buffer[read_len] = '\0';
}

static void test_mtp_warmup_storage_io(test_mtp_storage_t *storage)
{
    uint64_t total = 0;
    uint64_t free_bytes = 0;
    TEST_ASSERT_EQUAL(ESP_OK, esp_vfs_fat_info(storage->base_path, &total, &free_bytes));

    char path[128];
    char renamed_path[128];
    test_mtp_build_path(path, sizeof(path), storage->base_path, "warmup.txt");
    test_mtp_build_path(renamed_path, sizeof(renamed_path), storage->base_path, "warmup_renamed.txt");
    test_mtp_write_file(path, "warmup");

    char content[16] = { 0 };
    test_mtp_read_file(path, content, sizeof(content));
    TEST_ASSERT_EQUAL_STRING("warmup", content);

    FILE *file = fopen(path, "r+b");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, "Failed to open MTP warmup file for update");
    TEST_ASSERT_EQUAL(1, fwrite("W", 1, 1, file));
    TEST_ASSERT_EQUAL(0, fclose(file));

    struct stat st;
    TEST_ASSERT_EQUAL(0, stat(path, &st));
    TEST_ASSERT_EQUAL(0, rename(path, renamed_path));
    TEST_ASSERT_EQUAL(0, stat(renamed_path, &st));
    test_mtp_remove_path_if_exists(renamed_path);
    TEST_ASSERT_NOT_EQUAL(0, stat(renamed_path, &st));
}

static void test_mtp_write_file_if_missing(const char *path, const char *content)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        // Keep host-created or host-edited manual test files intact across resets.
        TEST_ASSERT_FALSE_MESSAGE(S_ISDIR(st.st_mode), "MTP manual seed file path points to a directory");
        return;
    }

    int err = errno;
    if (err != ENOENT) {
        ESP_LOGE(TAG, "failed to stat MTP manual seed file %s: errno=%d", path, err);
    }
    TEST_ASSERT_EQUAL_MESSAGE(ENOENT, err, "Failed to stat MTP manual seed file");
    test_mtp_write_file(path, content);
}

static void test_mtp_make_dir_if_missing(const char *path)
{
    if (mkdir(path, 0777) == 0) {
        return;
    }

    int err = errno;
    if (err == EEXIST) {
        struct stat st;
        TEST_ASSERT_EQUAL_MESSAGE(0, stat(path, &st), "Failed to stat existing MTP manual directory");
        TEST_ASSERT_TRUE_MESSAGE(S_ISDIR(st.st_mode), "MTP manual directory path exists but is not a directory");
        return;
    }

    ESP_LOGE(TAG, "failed to create MTP manual directory %s: errno=%d", path, err);
    TEST_FAIL_MESSAGE("Failed to create MTP manual directory");
}

static void test_mtp_register_all_storages(tinyusb_mtp_storage_handle_t handles[TEST_MTP_STORAGE_COUNT])
{
    for (size_t i = 0; i < TEST_MTP_STORAGE_COUNT; i++) {
        TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_register_storage(&(tinyusb_mtp_storage_config_t) {
            .base_path = s_test_storages[i].base_path,
            .display_name = s_test_storages[i].display_name,
            .removable = s_test_storages[i].removable,
        }, &handles[i]));
        TEST_ASSERT_NOT_NULL(handles[i]);
    }
}

static void test_mtp_unregister_all_storages(tinyusb_mtp_storage_handle_t handles[TEST_MTP_STORAGE_COUNT])
{
    for (size_t i = 0; i < TEST_MTP_STORAGE_COUNT; i++) {
        if (handles[i] != NULL) {
            TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_unregister_storage(handles[i]));
            handles[i] = NULL;
        }
    }
}

void test_mtp_storage_warm_up(void)
{
    if (s_mtp_storage_warmed_up) {
        return;
    }

    for (size_t i = 0; i < TEST_MTP_STORAGE_COUNT; i++) {
        test_mtp_mount_storage(&s_test_storages[i]);
        test_mtp_clean_storage(&s_test_storages[i]);
        test_mtp_warmup_storage_io(&s_test_storages[i]);
    }

    uint64_t total = 0;
    uint64_t free_bytes = 0;
    (void)esp_vfs_fat_info("/not_mounted", &total, &free_bytes);

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_install_driver(NULL));
    tinyusb_mtp_storage_handle_t handles[TEST_MTP_STORAGE_COUNT] = { 0 };
    test_mtp_register_all_storages(handles);

    const char *file_path = TEST_MTP_FLASH_BASE_PATH "/warmup.txt";
    test_mtp_write_file(file_path, "warmup");

    uint32_t object_handle = 0;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_find_object(handles[0], file_path, &object_handle));
    TEST_ASSERT_NOT_EQUAL(0, object_handle);
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_delete_object(object_handle));

    struct stat st;
    TEST_ASSERT_NOT_EQUAL(0, stat(file_path, &st));

    test_mtp_unregister_all_storages(handles);
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_uninstall_driver());

    test_mtp_clean_all_storages();
    s_mtp_storage_warmed_up = true;
}

TEST_CASE("MTP: register mounted FATFS and delete object through backend", "[mtp][storage][ci]")
{
    test_mtp_storage_t *flash = &s_test_storages[0];
    test_mtp_clean_storage_path(flash, "hello.txt");

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_install_driver(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, tinyusb_mtp_register_storage(&(tinyusb_mtp_storage_config_t) {
        .base_path = "/not_mounted",
        .display_name = "Missing",
    }, NULL));

    tinyusb_mtp_storage_handle_t storage = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_register_storage(&(tinyusb_mtp_storage_config_t) {
        .base_path = flash->base_path,
        .display_name = flash->display_name,
        .removable = flash->removable,
    }, &storage));
    TEST_ASSERT_NOT_NULL(storage);

    const char *file_path = TEST_MTP_FLASH_BASE_PATH "/hello.txt";
    test_mtp_write_file(file_path, "hello mtp");

    uint32_t object_handle = 0;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_find_object(storage, file_path, &object_handle));
    TEST_ASSERT_NOT_EQUAL(0, object_handle);
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_delete_object(object_handle));

    struct stat st;
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, stat(file_path, &st), "MTP backend delete did not remove the file");

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_unregister_storage(storage));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_uninstall_driver());
}

TEST_CASE("MTP: zero-size SendObjectInfo completes without SendObject", "[mtp][storage][ci]")
{
    test_mtp_storage_t *flash = &s_test_storages[0];
    test_mtp_clean_storage_path(flash, "empty.txt");
    test_mtp_clean_storage_path(flash, "empty_replace.txt");

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_install_driver(NULL));
    tinyusb_mtp_storage_handle_t storage = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_register_storage(&(tinyusb_mtp_storage_config_t) {
        .base_path = flash->base_path,
        .display_name = flash->display_name,
        .removable = flash->removable,
    }, &storage));
    TEST_ASSERT_NOT_NULL(storage);

    uint32_t empty_handle = 0;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_send_zero_size_object_info(storage, MTP_ROOT_PARENT, "empty.txt", &empty_handle));
    TEST_ASSERT_NOT_EQUAL(0, empty_handle);

    uint32_t parent_handle = 0;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_get_parent_handle(empty_handle, &parent_handle));
    TEST_ASSERT_EQUAL_UINT32(MTP_ROOT_PARENT, parent_handle);

    struct stat st;
    TEST_ASSERT_EQUAL(0, stat(TEST_MTP_FLASH_BASE_PATH "/empty.txt", &st));
    TEST_ASSERT_EQUAL(0, (int)st.st_size);

    const char *replace_path = TEST_MTP_FLASH_BASE_PATH "/empty_replace.txt";
    test_mtp_write_file(replace_path, "replace me");
    uint32_t replace_handle = 0;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_send_zero_size_object_info(storage, MTP_ROOT_PARENT, "empty_replace.txt", &replace_handle));
    TEST_ASSERT_NOT_EQUAL(0, replace_handle);
    TEST_ASSERT_EQUAL(0, stat(replace_path, &st));
    TEST_ASSERT_EQUAL(0, (int)st.st_size);

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_unregister_storage(storage));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_uninstall_driver());
}

TEST_CASE("MTP: Android direct edit backend updates object ranges", "[mtp][storage][ci]")
{
    test_mtp_storage_t *flash = &s_test_storages[0];
    test_mtp_clean_storage_path(flash, "direct_edit.txt");

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_install_driver(NULL));
    tinyusb_mtp_storage_handle_t storage = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_register_storage(&(tinyusb_mtp_storage_config_t) {
        .base_path = flash->base_path,
        .display_name = flash->display_name,
        .removable = flash->removable,
    }, &storage));

    const char *file_path = TEST_MTP_FLASH_BASE_PATH "/direct_edit.txt";
    test_mtp_write_file(file_path, "0123456789");

    uint32_t object_handle = 0;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_find_object(storage, file_path, &object_handle));
    TEST_ASSERT_NOT_EQUAL(0, object_handle);

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_begin_edit_object(object_handle));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_write_partial_object(object_handle, 3, "abc", 3));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_truncate_object(object_handle, 8));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_write_partial_object(object_handle, 8, "XY", 2));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_end_edit_object(object_handle));

    char content[32] = { 0 };
    test_mtp_read_file(file_path, content, sizeof(content));
    TEST_ASSERT_EQUAL_STRING("012abc67XY", content);

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_unregister_storage(storage));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_uninstall_driver());
}

TEST_CASE("MTP: unregister one storage preserves edit session on another", "[mtp][storage][ci]")
{
    test_mtp_storage_t *flash = &s_test_storages[0];
    test_mtp_storage_t *nand = &s_test_storages[2];
    test_mtp_clean_storage_path(nand, "cross_edit.txt");

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_install_driver(NULL));

    tinyusb_mtp_storage_handle_t flash_handle = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_register_storage(&(tinyusb_mtp_storage_config_t) {
        .base_path = flash->base_path,
        .display_name = flash->display_name,
        .removable = flash->removable,
    }, &flash_handle));

    tinyusb_mtp_storage_handle_t nand_handle = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_register_storage(&(tinyusb_mtp_storage_config_t) {
        .base_path = nand->base_path,
        .display_name = nand->display_name,
        .removable = nand->removable,
    }, &nand_handle));

    const char *file_path = TEST_MTP_NAND_BASE_PATH "/cross_edit.txt";
    test_mtp_write_file(file_path, "012345678901");

    uint32_t object_handle = 0;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_find_object(nand_handle, file_path, &object_handle));
    TEST_ASSERT_NOT_EQUAL(0, object_handle);

    // Open an edit session on nand storage and write the first half.
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_begin_edit_object(object_handle));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_write_partial_object(object_handle, 0, "PROBE_B", 7));

    // Unregister flash storage while nand still holds an active edit session.
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_unregister_storage(flash_handle));
    flash_handle = NULL;

    // The edit session on nand must still be usable after the unrelated unregister.
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_write_partial_object(object_handle, 7, "_SAFE", 5));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_end_edit_object(object_handle));

    char content[32] = { 0 };
    test_mtp_read_file(file_path, content, sizeof(content));
    TEST_ASSERT_EQUAL_STRING("PROBE_B_SAFE", content);

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_unregister_storage(nand_handle));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_uninstall_driver());
}

TEST_CASE("MTP: Windows Name property update preserves object file extension", "[mtp][storage][ci]")
{
    test_mtp_storage_t *flash = &s_test_storages[0];
    test_mtp_clean_storage_path(flash, "4G-usb.md");
    test_mtp_clean_storage_path(flash, "4G-usb");

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_install_driver(NULL));
    tinyusb_mtp_storage_handle_t storage = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_register_storage(&(tinyusb_mtp_storage_config_t) {
        .base_path = flash->base_path,
        .display_name = flash->display_name,
        .removable = flash->removable,
    }, &storage));

    const char *file_path = TEST_MTP_FLASH_BASE_PATH "/4G-usb.md";
    const char *stem_path = TEST_MTP_FLASH_BASE_PATH "/4G-usb";
    test_mtp_write_file(file_path, "markdown content");

    uint32_t object_handle = 0;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_find_object(storage, file_path, &object_handle));
    TEST_ASSERT_NOT_EQUAL(0, object_handle);
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_set_object_name(object_handle, "4G-usb"));

    struct stat st;
    TEST_ASSERT_EQUAL_MESSAGE(0, stat(file_path, &st), "MTP Name property update removed the file extension");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, stat(stem_path, &st), "MTP Name property update renamed the physical file");

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_unregister_storage(storage));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_uninstall_driver());
}

TEST_CASE("MTP: UTF-16 object filename is stored as UTF-8", "[mtp][storage][ci]")
{
    test_mtp_storage_t *flash = &s_test_storages[0];
    test_mtp_clean_storage_path(flash, "ascii_name.txt");
    test_mtp_clean_storage_path(flash, "中文文件.txt");

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_install_driver(NULL));
    tinyusb_mtp_storage_handle_t storage = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_register_storage(&(tinyusb_mtp_storage_config_t) {
        .base_path = flash->base_path,
        .display_name = flash->display_name,
        .removable = flash->removable,
    }, &storage));

    const char *old_path = TEST_MTP_FLASH_BASE_PATH "/ascii_name.txt";
    const char *utf8_path = TEST_MTP_FLASH_BASE_PATH "/中文文件.txt";
    test_mtp_write_file(old_path, "utf8 filename content");

    uint32_t object_handle = 0;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_find_object(storage, old_path, &object_handle));
    TEST_ASSERT_NOT_EQUAL(0, object_handle);
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_set_object_file_name(object_handle, "中文文件.txt"));

    struct stat st;
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0, stat(old_path, &st), "MTP UTF-16 filename update did not rename the source file");
    TEST_ASSERT_EQUAL_MESSAGE(0, stat(utf8_path, &st), "MTP UTF-16 filename update was not stored as UTF-8");

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_unregister_storage(storage));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_uninstall_driver());
}

TEST_CASE("MTP: register three mounted FATFS paths", "[mtp][storage][ci]")
{
    for (size_t i = 0; i < TEST_MTP_STORAGE_COUNT; i++) {
        test_mtp_clean_storage_path(&s_test_storages[i], "probe.txt");
    }

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_install_driver(NULL));
    tinyusb_mtp_storage_handle_t handles[TEST_MTP_STORAGE_COUNT] = { 0 };
    test_mtp_register_all_storages(handles);

    for (size_t i = 0; i < TEST_MTP_STORAGE_COUNT; i++) {
        char file_path[64];
        int path_len = snprintf(file_path, sizeof(file_path), "%s/%s", s_test_storages[i].base_path, "probe.txt");
        TEST_ASSERT_TRUE_MESSAGE(path_len > 0 && path_len < (int)sizeof(file_path), "MTP test path was truncated");
        test_mtp_write_file(file_path, "probe");

        uint32_t object_handle = 0;
        TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_find_object(handles[i], file_path, &object_handle));
        TEST_ASSERT_NOT_EQUAL(0, object_handle);
    }

    test_mtp_unregister_all_storages(handles);
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_uninstall_driver());
}

TEST_CASE("MTP: TinyUSB device stack starts with MTP descriptor", "[mtp][device][ci]")
{
    test_mtp_storage_t *flash = &s_test_storages[0];

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_install_driver(NULL));
    tinyusb_mtp_storage_handle_t storage = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_register_storage(&(tinyusb_mtp_storage_config_t) {
        .base_path = flash->base_path,
        .display_name = flash->display_name,
        .removable = flash->removable,
    }, &storage));

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_uninstall());

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_unregister_storage(storage));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_uninstall_driver());
}

TEST_CASE("MTP: manual PC file access", "[mtp][manual]")
{
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_install_driver(NULL));
    tinyusb_mtp_storage_handle_t handles[TEST_MTP_STORAGE_COUNT] = { 0 };
    test_mtp_register_all_storages(handles);

    for (size_t i = 0; i < TEST_MTP_STORAGE_COUNT; i++) {
        char file_path[96];
        int path_len = snprintf(file_path, sizeof(file_path), "%s/%s", s_test_storages[i].base_path, "host_access_probe.txt");
        TEST_ASSERT_TRUE_MESSAGE(path_len > 0 && path_len < (int)sizeof(file_path), "MTP manual test path was truncated");
        test_mtp_write_file_if_missing(file_path, "This file is pre-created by the MTP manual PC access test.\n");

        path_len = snprintf(file_path, sizeof(file_path), "%s/%s", s_test_storages[i].base_path, "edit_me.txt");
        TEST_ASSERT_TRUE_MESSAGE(path_len > 0 && path_len < (int)sizeof(file_path), "MTP manual edit file path was truncated");
        test_mtp_write_file_if_missing(file_path, "Edit this file from the PC and save it back through MTP.\n");

        path_len = snprintf(file_path, sizeof(file_path), "%s/%s", s_test_storages[i].base_path, "delete_me.txt");
        TEST_ASSERT_TRUE_MESSAGE(path_len > 0 && path_len < (int)sizeof(file_path), "MTP manual delete file path was truncated");
        test_mtp_write_file_if_missing(file_path, "Delete this file from the PC through MTP.\n");

        char dir_path[96];
        path_len = snprintf(dir_path, sizeof(dir_path), "%s/%s", s_test_storages[i].base_path, "folder_from_device");
        TEST_ASSERT_TRUE_MESSAGE(path_len > 0 && path_len < (int)sizeof(dir_path), "MTP manual directory path was truncated");
        test_mtp_make_dir_if_missing(dir_path);

        path_len = snprintf(file_path, sizeof(file_path), "%s/%s", dir_path, "nested.txt");
        TEST_ASSERT_TRUE_MESSAGE(path_len > 0 && path_len < (int)sizeof(file_path), "MTP manual nested file path was truncated");
        test_mtp_write_file_if_missing(file_path, "This nested file verifies folder traversal from the PC.\n");

        ESP_LOGI(TAG, "Registered %s at %s", s_test_storages[i].display_name, s_test_storages[i].base_path);
    }

    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_driver_install(&tusb_cfg));

    ESP_LOGI(TAG, "MTP manual test is running. Connect USB to a PC and test read, write, rename, and delete operations.");
    ESP_LOGI(TAG, "MTP_HOST_READY");
#if CONFIG_TINYUSB_MTP_TRACE_WRITES
    ESP_LOGI(TAG, "MTP write trace logs are enabled for host-side edit validation.");
#endif
    ESP_LOGI(TAG, "This Unity test intentionally never exits. Reset the board to stop it.");

    // Keep the USB device online for manual host-side file access.
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
