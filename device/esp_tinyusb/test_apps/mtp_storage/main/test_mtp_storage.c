/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "soc/soc_caps.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
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
#include "mtp/tinyusb_mtp_context.h"
#include "mtp/tinyusb_mtp_object_store.h"
#include "mtp/tinyusb_mtp_types.h"
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

typedef struct {
    TaskHandle_t owner;
    TaskHandle_t task;
    volatile bool stop;
} test_mtp_callback_race_t;

static void test_mtp_callback_race_task(void *arg)
{
    test_mtp_callback_race_t *race = arg;
    while (!race->stop) {
        (void)tud_mtp_request_cancel_cb(NULL);
        vTaskDelay(1);
    }
    xTaskNotifyGive(race->owner);
    vTaskSuspend(NULL);
}

static const char *const s_test_artifacts[] = {
    "hello.txt",
    "empty.txt",
    "empty_replace.txt",
    "empty_storage_zero.txt",
    "active_pending.txt",
    "cancel_first.txt",
    "cancel_second.txt",
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

    while (true) {
        DIR *dir = opendir(storage->base_path);
        TEST_ASSERT_NOT_NULL_MESSAGE(dir, "Failed to open MTP test storage for cleanup");
        char stale_name[256] = { 0 };
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strncmp(entry->d_name, "ci_perf_", 8) == 0 || strncmp(entry->d_name, "ci_concurrent_", 14) == 0 || strncmp(entry->d_name, "ci_many_", 8) == 0 ||
                    strncmp(entry->d_name, "ci_cache_", 9) == 0) {
                int len = snprintf(stale_name, sizeof(stale_name), "%s", entry->d_name);
                TEST_ASSERT_TRUE_MESSAGE(len > 0 && len < (int)sizeof(stale_name), "MTP stale workspace name was truncated");
                break;
            }
        }
        TEST_ASSERT_EQUAL(0, closedir(dir));
        if (stale_name[0] == '\0') {
            break;
        }
        char path[128];
        test_mtp_build_path(path, sizeof(path), storage->base_path, stale_name);
        TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, mtp_recursive_delete_path(path), "Failed to remove stale MTP host test workspace");
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
    if (file == NULL) {
        ESP_LOGE(TAG, "failed to open MTP test file %s for write: %s", path, strerror(errno));
        for (size_t i = 0; i < TEST_MTP_STORAGE_COUNT; i++) {
            size_t base_len = strlen(s_test_storages[i].base_path);
            if (strncmp(path, s_test_storages[i].base_path, base_len) == 0 && path[base_len] == '/') {
                uint64_t total = 0;
                uint64_t free_bytes = 0;
                esp_err_t ret = esp_vfs_fat_info(s_test_storages[i].base_path, &total, &free_bytes);
                ESP_LOGE(TAG, "storage state: path=%s info=%s total=%" PRIu64 " free=%" PRIu64, s_test_storages[i].base_path, esp_err_to_name(ret), total, free_bytes);
                break;
            }
        }
    }
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

static size_t test_mtp_cached_object_count(tinyusb_mtp_storage_handle_t storage)
{
    size_t count = 0;
    mtp_lock();
    count = mtp_object_count_for_storage_locked(storage);
    mtp_unlock();
    return count;
}

static uint32_t test_mtp_get_object_count(uint16_t operation, uint32_t storage_id, uint32_t object_format, uint32_t parent)
{
    uint8_t response[sizeof(uint32_t) + CONFIG_TINYUSB_MTP_MAX_OBJECTS * sizeof(uint32_t)] = { 0 };
    tinyusb_mtp_test_result_t result;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_execute(&(tinyusb_mtp_test_transaction_t) {
        .operation = operation,
        .phase = MTP_PHASE_COMMAND,
        .param_count = 3,
        .params = { storage_id, object_format, parent },
        .complete_data = operation == MTP_OP_GET_OBJECT_HANDLES,
    }, response, sizeof(response), &result));
    TEST_ASSERT_EQUAL_INT32(MTP_RESP_OK, result.response_code);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(sizeof(uint32_t), result.data_len);

    uint32_t count = 0;
    memcpy(&count, response, sizeof(count));
    return count;
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

TEST_CASE("MTP: uninstall serializes with callbacks", "[mtp][storage][ci]")
{
    test_mtp_callback_race_t race = {
        .owner = xTaskGetCurrentTaskHandle(),
    };
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(test_mtp_callback_race_task, "mtp_cb_race", 3072, &race, 5, &race.task));
    for (size_t i = 0; i < 20; i++) {
        TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_install_driver(NULL));
        taskYIELD();
        TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_uninstall_driver());
    }
    race.stop = true;
    TEST_ASSERT_EQUAL(1, ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)));
    vTaskDelete(race.task);
    vTaskDelay(1);
}

TEST_CASE("MTP: zero-size SendObjectInfo completes without SendObject", "[mtp][storage][ci]")
{
    test_mtp_storage_t *flash = &s_test_storages[0];
    test_mtp_clean_storage_path(flash, "empty.txt");
    test_mtp_clean_storage_path(flash, "empty_replace.txt");
    test_mtp_clean_storage_path(flash, "empty_storage_zero.txt");

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_install_driver(NULL));
    tinyusb_mtp_storage_handle_t storage = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_register_storage(&(tinyusb_mtp_storage_config_t) {
        .base_path = flash->base_path,
        .display_name = flash->display_name,
        .removable = flash->removable,
    }, &storage));
    TEST_ASSERT_NOT_NULL(storage);

    uint32_t empty_handle = 0;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_send_object_info(storage, TINYUSB_MTP_TEST_STORAGE_ID_REGISTERED, TINYUSB_MTP_TEST_STORAGE_ID_REGISTERED,
                                                                MTP_ROOT_PARENT, "empty.txt", 0, &empty_handle));
    TEST_ASSERT_NOT_EQUAL(0, empty_handle);
    uint8_t response_params[3 * sizeof(uint32_t)] = { 0 };
    tinyusb_mtp_test_result_t send_result;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_execute(&(tinyusb_mtp_test_transaction_t) {
        .operation = MTP_OP_SEND_OBJECT,
        .phase = MTP_PHASE_DATA,
        .complete_data = true,
    }, response_params, sizeof(response_params), &send_result));
    TEST_ASSERT_EQUAL_INT32(MTP_RESP_OK, send_result.response_code);

    uint32_t parent_handle = 0;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_get_parent_handle(empty_handle, &parent_handle));
    TEST_ASSERT_EQUAL_UINT32(MTP_ROOT_PARENT, parent_handle);

    struct stat st;
    TEST_ASSERT_EQUAL(0, stat(TEST_MTP_FLASH_BASE_PATH "/empty.txt", &st));
    TEST_ASSERT_EQUAL(0, (int)st.st_size);

    const char *replace_path = TEST_MTP_FLASH_BASE_PATH "/empty_replace.txt";
    test_mtp_write_file(replace_path, "replace me");
    uint32_t replace_handle = 0;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_send_object_info(storage, TINYUSB_MTP_TEST_STORAGE_ID_REGISTERED, TINYUSB_MTP_TEST_STORAGE_ID_REGISTERED,
                                                                MTP_ROOT_PARENT, "empty_replace.txt", 0, &replace_handle));
    TEST_ASSERT_NOT_EQUAL(0, replace_handle);
    TEST_ASSERT_EQUAL(0, stat(replace_path, &st));
    TEST_ASSERT_EQUAL(0, (int)st.st_size);

    uint32_t zero_storage_handle = 0;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_send_object_info(storage, MTP_STORAGE_ID_UNSPECIFIED, MTP_STORAGE_ID_UNSPECIFIED, MTP_PARENT_ROOT,
                                                                "empty_storage_zero.txt", 0, &zero_storage_handle));
    TEST_ASSERT_NOT_EQUAL(0, zero_storage_handle);
    TEST_ASSERT_EQUAL(0, stat(TEST_MTP_FLASH_BASE_PATH "/empty_storage_zero.txt", &st));

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_unregister_storage(storage));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_uninstall_driver());
}

TEST_CASE("MTP: active USB transfer blocks storage unregister", "[mtp][storage][ci]")
{
    test_mtp_storage_t *flash = &s_test_storages[0];
    test_mtp_clean_storage_path(flash, "active_pending.txt");
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_install_driver(NULL));
    tinyusb_mtp_storage_handle_t storage = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_register_storage(&(tinyusb_mtp_storage_config_t) {
        .base_path = flash->base_path,
        .display_name = flash->display_name,
        .removable = flash->removable,
    }, &storage));

    uint32_t object_handle = 0;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_send_object_info(storage, TINYUSB_MTP_TEST_STORAGE_ID_REGISTERED, TINYUSB_MTP_TEST_STORAGE_ID_REGISTERED,
                                                                MTP_ROOT_PARENT, "active_pending.txt", 1, &object_handle));
    TEST_ASSERT_NOT_EQUAL(0, object_handle);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, tinyusb_mtp_unregister_storage(storage));
    TEST_ASSERT_TRUE(tud_mtp_request_cancel_cb(NULL));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_unregister_storage(storage));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_uninstall_driver());
    test_mtp_clean_storage_path(flash, "active_pending.txt");
}

TEST_CASE("MTP: cancelled read allows the next object read", "[mtp][storage][ci]")
{
    test_mtp_storage_t *flash = &s_test_storages[0];
    char first_path[96];
    char second_path[96];
    test_mtp_build_path(first_path, sizeof(first_path), flash->base_path, "cancel_first.txt");
    test_mtp_build_path(second_path, sizeof(second_path), flash->base_path, "cancel_second.txt");
    test_mtp_write_file(first_path, "first object content must span the test packet so cancellation occurs before transfer completion");
    test_mtp_write_file(second_path, "second object");

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_install_driver(NULL));
    tinyusb_mtp_storage_handle_t storage = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_register_storage(&(tinyusb_mtp_storage_config_t) {
        .base_path = flash->base_path,
        .display_name = flash->display_name,
        .removable = flash->removable,
    }, &storage));
    uint32_t first_handle = 0;
    uint32_t second_handle = 0;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_find_object(storage, first_path, &first_handle));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_find_object(storage, second_path, &second_handle));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_cancel_read_and_restart(first_handle, second_handle));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_unregister_storage(storage));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_uninstall_driver());
    unlink(first_path);
    unlink(second_path);
}

TEST_CASE("MTP: all-object property list traverses all depths", "[mtp][storage][ci]")
{
    test_mtp_storage_t *flash = &s_test_storages[0];
    char dir_path[128];
    char child_path[160];
    test_mtp_build_path(dir_path, sizeof(dir_path), flash->base_path, "ci_prop_tree");
    test_mtp_build_path(child_path, sizeof(child_path), dir_path, "child.txt");
    struct stat st;
    if (stat(dir_path, &st) == 0) {
        TEST_ASSERT_EQUAL(ESP_OK, mtp_recursive_delete_path(dir_path));
    }
    test_mtp_make_dir_if_missing(dir_path);
    test_mtp_write_file(child_path, "property");

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_install_driver(NULL));
    tinyusb_mtp_storage_handle_t storage = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_register_storage(&(tinyusb_mtp_storage_config_t) {
        .base_path = flash->base_path,
        .display_name = flash->display_name,
        .removable = flash->removable,
    }, &storage));

    uint32_t counts[2] = { 0 };
    const uint32_t depths[] = { 0, MTP_DEPTH_ALL };
    for (size_t i = 0; i < 2; i++) {
        uint8_t response[1024] = { 0 };
        tinyusb_mtp_test_result_t result;
        TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_execute(&(tinyusb_mtp_test_transaction_t) {
            .operation = MTP_OP_GET_OBJECT_PROP_LIST,
            .phase = MTP_PHASE_COMMAND,
            .param_count = 5,
            .params = { MTP_OBJECT_HANDLE_ALL, 0, MTP_OBJ_PROP_OBJECT_FILE_NAME, 0, depths[i] },
            .complete_data = true,
        }, response, sizeof(response), &result));
        TEST_ASSERT_EQUAL_INT32(MTP_RESP_OK, result.response_code);
        TEST_ASSERT_GREATER_OR_EQUAL_UINT32(sizeof(uint32_t), result.data_len);
        memcpy(&counts[i], response, sizeof(counts[i]));
    }
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(2, counts[0]);
    TEST_ASSERT_EQUAL_UINT32(counts[1], counts[0]);

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_unregister_storage(storage));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_uninstall_driver());
    TEST_ASSERT_EQUAL(ESP_OK, mtp_recursive_delete_path(dir_path));
}

TEST_CASE("MTP: all-object handle queries traverse all depths", "[mtp][storage][ci]")
{
    test_mtp_storage_t *flash = &s_test_storages[0];
    char tree_path[128];
    char root_file[160];
    char dir_path[160];
    char nested_file[192];
    char subdir_path[192];
    char deep_file[224];
    test_mtp_build_path(tree_path, sizeof(tree_path), flash->base_path, "ci_handle_tree");
    test_mtp_build_path(root_file, sizeof(root_file), tree_path, "root.txt");
    test_mtp_build_path(dir_path, sizeof(dir_path), tree_path, "folder");
    test_mtp_build_path(nested_file, sizeof(nested_file), dir_path, "nested.mp3");
    test_mtp_build_path(subdir_path, sizeof(subdir_path), dir_path, "subfolder");
    test_mtp_build_path(deep_file, sizeof(deep_file), subdir_path, "deep.txt");

    struct stat st;
    if (stat(tree_path, &st) == 0) {
        TEST_ASSERT_EQUAL(ESP_OK, mtp_recursive_delete_path(tree_path));
    }

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_install_driver(NULL));
    tinyusb_mtp_storage_handle_t storage = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_register_storage(&(tinyusb_mtp_storage_config_t) {
        .base_path = flash->base_path,
        .display_name = "Handle tree",
    }, &storage));

    uint32_t root_count = test_mtp_get_object_count(MTP_OP_GET_OBJECT_HANDLES, storage->storage_id, 0, MTP_OBJECT_HANDLE_INVALID);
    uint32_t all_count = test_mtp_get_object_count(MTP_OP_GET_OBJECT_HANDLES, storage->storage_id, 0, MTP_OBJECT_HANDLE_ALL);
    uint32_t mp3_count = test_mtp_get_object_count(MTP_OP_GET_OBJECT_HANDLES, storage->storage_id, MTP_OBJ_FORMAT_MP3, MTP_OBJECT_HANDLE_ALL);

    test_mtp_make_dir_if_missing(tree_path);
    test_mtp_write_file(root_file, "root");
    test_mtp_make_dir_if_missing(dir_path);
    test_mtp_write_file(nested_file, "audio");
    test_mtp_make_dir_if_missing(subdir_path);
    test_mtp_write_file(deep_file, "deep");

    TEST_ASSERT_EQUAL_UINT32(root_count + 1, test_mtp_get_object_count(MTP_OP_GET_OBJECT_HANDLES, storage->storage_id, 0, MTP_OBJECT_HANDLE_INVALID));
    TEST_ASSERT_EQUAL_UINT32(all_count + 6, test_mtp_get_object_count(MTP_OP_GET_OBJECT_HANDLES, storage->storage_id, 0, MTP_OBJECT_HANDLE_ALL));
    TEST_ASSERT_EQUAL_UINT32(mp3_count + 1, test_mtp_get_object_count(MTP_OP_GET_OBJECT_HANDLES, storage->storage_id, MTP_OBJ_FORMAT_MP3, MTP_OBJECT_HANDLE_ALL));
    TEST_ASSERT_EQUAL_UINT32(all_count + 6, test_mtp_get_object_count(MTP_OP_GET_NUM_OBJECTS, storage->storage_id, 0, MTP_OBJECT_HANDLE_ALL));

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_unregister_storage(storage));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_uninstall_driver());
    TEST_ASSERT_EQUAL(ESP_OK, mtp_recursive_delete_path(tree_path));
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

TEST_CASE("MTP: long ObjectInfo filename fits the endpoint buffer", "[mtp][storage][ci]")
{
    test_mtp_storage_t *flash = &s_test_storages[0];
    char name[235];
    memset(name, 'a', 230);
    memcpy(name + 230, ".txt", 5);

    char path[1024];
    test_mtp_build_path(path, sizeof(path), flash->base_path, name);
    test_mtp_remove_path_if_exists(path);

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_install_driver(NULL));
    tinyusb_mtp_storage_handle_t storage = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_register_storage(&(tinyusb_mtp_storage_config_t) {
        .base_path = flash->base_path,
        .display_name = flash->display_name,
        .removable = flash->removable,
    }, &storage));

    uint32_t object_handle = 0;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_send_object_info(storage, TINYUSB_MTP_TEST_STORAGE_ID_REGISTERED, TINYUSB_MTP_TEST_STORAGE_ID_REGISTERED,
                                                                MTP_ROOT_PARENT, name, 0, &object_handle));
    TEST_ASSERT_NOT_EQUAL(0, object_handle);

    struct stat st;
    TEST_ASSERT_EQUAL(0, stat(path, &st));
    TEST_ASSERT_EQUAL(0, (int)st.st_size);

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_unregister_storage(storage));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_uninstall_driver());
    test_mtp_remove_path_if_exists(path);
}

TEST_CASE("MTP: directory refresh removes stale cached objects", "[mtp][storage][ci]")
{
    test_mtp_storage_t *flash = &s_test_storages[0];
    char dir_path[128];
    char keep_path[160];
    char drop_path[160];
    test_mtp_build_path(dir_path, sizeof(dir_path), flash->base_path, "cache_refresh");
    test_mtp_build_path(keep_path, sizeof(keep_path), dir_path, "keep.txt");
    test_mtp_build_path(drop_path, sizeof(drop_path), dir_path, "drop.txt");
    test_mtp_remove_path_if_exists(keep_path);
    test_mtp_remove_path_if_exists(drop_path);
    test_mtp_remove_path_if_exists(dir_path);
    test_mtp_make_dir_if_missing(dir_path);
    test_mtp_write_file(keep_path, "keep");
    test_mtp_write_file(drop_path, "drop");

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_install_driver(NULL));
    tinyusb_mtp_storage_handle_t storage = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_register_storage(&(tinyusb_mtp_storage_config_t) {
        .base_path = flash->base_path,
        .display_name = flash->display_name,
        .removable = flash->removable,
    }, &storage));

    uint32_t directory_handle = 0;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_find_object(storage, dir_path, &directory_handle));
    TEST_ASSERT_EQUAL_UINT32(1, test_mtp_cached_object_count(storage));

    uint32_t count = 0;
    mtp_lock();
    int32_t scan_ret = mtp_scan_children_locked(storage, directory_handle, 0, NULL, 0, &count);
    mtp_unlock();
    TEST_ASSERT_EQUAL_INT32(0, scan_ret);
    TEST_ASSERT_EQUAL_UINT32(2, count);
    TEST_ASSERT_EQUAL_UINT32(1, test_mtp_cached_object_count(storage));

    uint32_t handles[2] = { 0 };
    mtp_lock();
    scan_ret = mtp_scan_children_locked(storage, directory_handle, 0, handles, 2, &count);
    mtp_unlock();
    TEST_ASSERT_EQUAL_INT32(0, scan_ret);
    TEST_ASSERT_EQUAL_UINT32(2, count);
    TEST_ASSERT_EQUAL_UINT32(3, test_mtp_cached_object_count(storage));
    uint32_t original_handles[2] = { handles[0], handles[1] };

    TEST_ASSERT_EQUAL(0, unlink(drop_path));

    mtp_lock();
    scan_ret = mtp_scan_children_locked(storage, directory_handle, 0, handles, 2, &count);
    mtp_unlock();
    TEST_ASSERT_EQUAL_INT32(0, scan_ret);
    TEST_ASSERT_EQUAL_UINT32(1, count);
    TEST_ASSERT_EQUAL_UINT32(2, test_mtp_cached_object_count(storage));

    size_t valid_handles = 0;
    size_t stale_handles = 0;
    for (size_t i = 0; i < 2; i++) {
        uint32_t parent_handle = 0;
        esp_err_t ret = tinyusb_mtp_test_get_parent_handle(original_handles[i], &parent_handle);
        if (ret == ESP_OK) {
            TEST_ASSERT_EQUAL_UINT32(directory_handle, parent_handle);
            valid_handles++;
        } else {
            TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, ret);
            stale_handles++;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(1, valid_handles);
    TEST_ASSERT_EQUAL_UINT32(1, stale_handles);

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_unregister_storage(storage));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_uninstall_driver());
    test_mtp_remove_path_if_exists(keep_path);
    test_mtp_remove_path_if_exists(dir_path);
}

TEST_CASE("MTP: full object cache reclaims missing entries", "[mtp][storage][ci]")
{
    test_mtp_storage_t *flash = &s_test_storages[0];
    char path[128];
    for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_OBJECTS; i++) {
        char name[32];
        snprintf(name, sizeof(name), "ci_cache_%03u.txt", (unsigned)i);
        test_mtp_build_path(path, sizeof(path), flash->base_path, name);
        test_mtp_remove_path_if_exists(path);
    }
    test_mtp_clean_storage_path(flash, "ci_cache_replacement.txt");

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_install_driver(NULL));
    tinyusb_mtp_storage_handle_t storage = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_register_storage(&(tinyusb_mtp_storage_config_t) {
        .base_path = flash->base_path,
        .display_name = flash->display_name,
        .removable = flash->removable,
    }, &storage));

    for (size_t i = 0; i < CONFIG_TINYUSB_MTP_MAX_OBJECTS; i++) {
        char name[32];
        snprintf(name, sizeof(name), "ci_cache_%03u.txt", (unsigned)i);
        test_mtp_build_path(path, sizeof(path), flash->base_path, name);
        test_mtp_write_file(path, "cache");
        uint32_t handle = 0;
        TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_find_object(storage, path, &handle));
    }
    TEST_ASSERT_EQUAL_UINT32(CONFIG_TINYUSB_MTP_MAX_OBJECTS, test_mtp_cached_object_count(storage));

    test_mtp_build_path(path, sizeof(path), flash->base_path, "ci_cache_000.txt");
    TEST_ASSERT_EQUAL(0, unlink(path));
    test_mtp_build_path(path, sizeof(path), flash->base_path, "ci_cache_replacement.txt");
    test_mtp_write_file(path, "replacement");
    uint32_t replacement_handle = 0;
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_test_find_object(storage, path, &replacement_handle));
    TEST_ASSERT_NOT_EQUAL(0, replacement_handle);

    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_unregister_storage(storage));
    TEST_ASSERT_EQUAL(ESP_OK, tinyusb_mtp_uninstall_driver());
    test_mtp_clean_storage(flash);
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
